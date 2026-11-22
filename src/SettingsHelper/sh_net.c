/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_net.c — CommitNetworkPreferences and SetComputerName: Network.app's
 * operations (nextbsd/nextbsd#638).
 *
 *   CommitNetworkPreferences  one service's IPv4, IPv6 and DNS entities
 *                             in SCPreferences, as macOS's Network pane
 *                             writes them: the IPv4 and IPv6 entities are
 *                             replaced (ConfigMethod and its addresses),
 *                             the DNS entity keeps its other keys. Then
 *                             commit and apply: PreferencesMonitor
 *                             flattens the current set into Setup:, and
 *                             ipconfigd configures the service again.
 *   SetComputerName           scutil --set's way: the ComputerName and
 *                             LocalHostName setters, commit and apply;
 *                             hostnamed publishes the names and sets the
 *                             host name.
 *
 * Everything from the wire is checked before SCPreferences is opened:
 * addresses with inet_pton, domain names and the LocalHostName by their
 * characters, the ComputerName as UTF-8 without control characters. The
 * service must already exist; nothing here creates or removes one.
 */
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <ctype.h>
#include <string.h>

#include "SettingsHelper.h"

#define	PREFS_NAME	CFSTR("SettingsHelper")

/* Darwin's name for it; SCSchemaDefinitions.h has no constant. */
#define	V6_LINK_LOCAL	CFSTR("LinkLocal")

/* One parsed list from the wire: at most SH_NET_LIST_ENTRIES lines. */
struct net_list {
	int	count;
	char	entry[SH_NET_LIST_ENTRIES][SH_NET_ADDRESS_MAX + 1];
};

/* Also the longest search domain this takes. */
#define	DOMAIN_MAX	SH_NET_ADDRESS_MAX

static bool
v4_ok(const char *s, struct in_addr *out)
{
	struct in_addr a;

	return (inet_pton(AF_INET, s, out != NULL ? out : &a) == 1);
}

static bool
v6_ok(const char *s, struct in6_addr *out)
{
	struct in6_addr a;

	return (inet_pton(AF_INET6, s, out != NULL ? out : &a) == 1);
}

/* A subnet mask: ones, then zeroes, and not all zeroes. */
static bool
mask_ok(const char *s)
{
	struct in_addr a;
	uint32_t m;

	if (!v4_ok(s, &a))
		return (false);
	m = ntohl(a.s_addr);
	return (m != 0 && (~m & (~m + 1)) == 0);
}

/* A domain name: labels of letters, digits and hyphens, dot-separated. */
static bool
domain_ok(const char *s)
{
	const char *p;
	size_t label = 0;

	if (s[0] == '\0' || strlen(s) > DOMAIN_MAX)
		return (false);
	for (p = s; *p != '\0'; p++) {
		if (*p == '.') {
			if (label == 0 || p[-1] == '-')
				return (false);
			label = 0;
			continue;
		}
		if (!isalnum((unsigned char)*p) && *p != '-')
			return (false);
		if (label == 0 && *p == '-')
			return (false);
		if (++label > 63)
			return (false);
	}
	return (label > 0 && p[-1] != '-');
}

/*
 * Split `wire` (already NUL-terminated) into lines, each checked by
 * `ok`. Blank lines are skipped. False for too many entries, one too
 * long, or one `ok` refuses.
 */
static bool
parse_list(const char *wire, bool (*ok)(const char *), struct net_list *out)
{
	const char *p, *eol;
	size_t n;

	(void)memset(out, 0, sizeof(*out));
	for (p = wire; *p != '\0'; p = *eol != '\0' ? eol + 1 : eol) {
		eol = strchr(p, '\n');
		if (eol == NULL)
			eol = p + strlen(p);
		n = (size_t)(eol - p);
		if (n == 0)
			continue;
		if (out->count == SH_NET_LIST_ENTRIES ||
		    n > SH_NET_ADDRESS_MAX)
			return (false);
		(void)memcpy(out->entry[out->count], p, n);
		out->entry[out->count][n] = '\0';
		if (!ok(out->entry[out->count]))
			return (false);
		out->count++;
	}
	return (true);
}

static bool
server_ok(const char *s)
{
	return (v4_ok(s, NULL) || v6_ok(s, NULL));
}

static CFStringRef
cfstr(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

/* A CFArray of one string. */
static CFArrayRef
cfarray1(const char *s)
{
	CFStringRef v = cfstr(s);
	CFArrayRef a;

	a = CFArrayCreate(NULL, (const void **)&v, 1, &kCFTypeArrayCallBacks);
	CFRelease(v);
	return (a);
}

static CFArrayRef
cfarray_list(const struct net_list *l)
{
	CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0,
	    &kCFTypeArrayCallBacks);
	CFStringRef v;
	int i;

	for (i = 0; i < l->count; i++) {
		v = cfstr(l->entry[i]);
		CFArrayAppendValue(a, v);
		CFRelease(v);
	}
	return (a);
}

static CFMutableDictionaryRef
new_dict(void)
{
	return (CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

/* The service's protocol of this type, added if it has none. */
static SCNetworkProtocolRef
protocol(SCNetworkServiceRef service, CFStringRef type)
{
	SCNetworkProtocolRef p;

	p = SCNetworkServiceCopyProtocol(service, type);
	if (p == NULL && SCNetworkServiceAddProtocolType(service, type))
		p = SCNetworkServiceCopyProtocol(service, type);
	return (p);
}

/* Set a protocol's configuration (replacing it; NULL leaves it) and
 * whether it's on. */
static bool
set_protocol(SCNetworkServiceRef service, CFStringRef type,
    CFDictionaryRef config, bool enabled)
{
	SCNetworkProtocolRef p = protocol(service, type);
	bool ok;

	if (p == NULL)
		return (false);
	ok = (config == NULL || SCNetworkProtocolSetConfiguration(p, config)) &&
	    SCNetworkProtocolSetEnabled(p, enabled);
	CFRelease(p);
	return (ok);
}

static CFDictionaryRef
ipv4_config(int method, const char *addr, const char *mask,
    const char *router)
{
	CFMutableDictionaryRef d = new_dict();
	CFArrayRef a;
	CFStringRef s;

	switch (method) {
	case SH_NET_V4_MANUAL:
		CFDictionarySetValue(d, kSCPropNetIPv4ConfigMethod,
		    kSCValNetIPv4ConfigMethodManual);
		a = cfarray1(addr);
		CFDictionarySetValue(d, kSCPropNetIPv4Addresses, a);
		CFRelease(a);
		a = cfarray1(mask);
		CFDictionarySetValue(d, kSCPropNetIPv4SubnetMasks, a);
		CFRelease(a);
		if (router[0] != '\0') {
			s = cfstr(router);
			CFDictionarySetValue(d, kSCPropNetIPv4Router, s);
			CFRelease(s);
		}
		break;
	case SH_NET_V4_DHCP_MANUAL:
		CFDictionarySetValue(d, kSCPropNetIPv4ConfigMethod,
		    kSCValNetIPv4ConfigMethodINFORM);
		a = cfarray1(addr);
		CFDictionarySetValue(d, kSCPropNetIPv4Addresses, a);
		CFRelease(a);
		break;
	default:
		CFDictionarySetValue(d, kSCPropNetIPv4ConfigMethod,
		    kSCValNetIPv4ConfigMethodDHCP);
		break;
	}
	return (d);
}

static CFDictionaryRef
ipv6_config(int method, const char *addr, int prefix, const char *router)
{
	CFMutableDictionaryRef d = new_dict();
	CFArrayRef a;
	CFNumberRef n;
	CFStringRef s;

	switch (method) {
	case SH_NET_V6_MANUAL:
		CFDictionarySetValue(d, kSCPropNetIPv6ConfigMethod,
		    kSCValNetIPv6ConfigMethodManual);
		a = cfarray1(addr);
		CFDictionarySetValue(d, kSCPropNetIPv6Addresses, a);
		CFRelease(a);
		n = CFNumberCreate(NULL, kCFNumberIntType, &prefix);
		a = CFArrayCreate(NULL, (const void **)&n, 1,
		    &kCFTypeArrayCallBacks);
		CFRelease(n);
		CFDictionarySetValue(d, kSCPropNetIPv6PrefixLength, a);
		CFRelease(a);
		if (router[0] != '\0') {
			s = cfstr(router);
			CFDictionarySetValue(d, kSCPropNetIPv6Router, s);
			CFRelease(s);
		}
		break;
	case SH_NET_V6_LINK_LOCAL:
		CFDictionarySetValue(d, kSCPropNetIPv6ConfigMethod,
		    V6_LINK_LOCAL);
		break;
	default:
		CFDictionarySetValue(d, kSCPropNetIPv6ConfigMethod,
		    kSCValNetIPv6ConfigMethodAutomatic);
		break;
	}
	return (d);
}

/* The DNS entity as it was, with these servers and search domains. */
static CFDictionaryRef
dns_config(SCNetworkServiceRef service, const struct net_list *servers,
    const struct net_list *domains)
{
	SCNetworkProtocolRef p = protocol(service, kSCNetworkProtocolTypeDNS);
	CFDictionaryRef cur = p != NULL ?
	    SCNetworkProtocolGetConfiguration(p) : NULL;
	CFMutableDictionaryRef d;
	CFArrayRef a;

	d = cur != NULL ? CFDictionaryCreateMutableCopy(NULL, 0, cur) :
	    new_dict();
	if (p != NULL)
		CFRelease(p);
	CFDictionaryRemoveValue(d, kSCPropNetDNSServerAddresses);
	CFDictionaryRemoveValue(d, kSCPropNetDNSSearchDomains);
	if (servers->count > 0) {
		a = cfarray_list(servers);
		CFDictionarySetValue(d, kSCPropNetDNSServerAddresses, a);
		CFRelease(a);
	}
	if (domains->count > 0) {
		a = cfarray_list(domains);
		CFDictionarySetValue(d, kSCPropNetDNSSearchDomains, a);
		CFRelease(a);
	}
	return (d);
}

/* Commit and apply, then unlock. */
static int
commit(SCPreferencesRef prefs)
{
	int rc = SH_OK;

	if (!SCPreferencesCommitChanges(prefs)) {
		sh_log("SCPreferencesCommitChanges: %s", SCErrorString(SCError()));
		rc = SH_ERR_FAILED;
	} else if (!SCPreferencesApplyChanges(prefs)) {
		sh_log("SCPreferencesApplyChanges: %s", SCErrorString(SCError()));
		rc = SH_ERR_FAILED;
	}
	(void)SCPreferencesUnlock(prefs);
	return (rc);
}

static SCPreferencesRef
open_prefs(void)
{
	SCPreferencesRef prefs = SCPreferencesCreate(NULL, PREFS_NAME, NULL);

	if (prefs == NULL)
		return (NULL);
	if (!SCPreferencesLock(prefs, true)) {
		sh_log("SCPreferencesLock: %s", SCErrorString(SCError()));
		CFRelease(prefs);
		return (NULL);
	}
	return (prefs);
}

int
sh_net_commit(const struct sh_net_service *s)
{
	struct net_list servers, domains;
	struct in6_addr a6;
	SCPreferencesRef prefs;
	SCNetworkServiceRef service;
	CFDictionaryRef v4, v6, dns;
	CFStringRef id;
	const char *p;
	bool ok;
	int rc;

	/* The service ID: a UUID's characters, as SCPreferences makes it. */
	if (s->service_id[0] == '\0')
		return (SH_ERR_INVALID);
	for (p = s->service_id; *p != '\0'; p++)
		if (!isxdigit((unsigned char)*p) && *p != '-')
			return (SH_ERR_INVALID);

	switch (s->ipv4_method) {
	case SH_NET_V4_DHCP:
	case SH_NET_V4_OFF:
		break;
	case SH_NET_V4_MANUAL:
		if (!mask_ok(s->ipv4_mask))
			return (SH_ERR_INVALID);
		if (s->ipv4_router[0] != '\0' && !v4_ok(s->ipv4_router, NULL))
			return (SH_ERR_INVALID);
		/* FALLTHROUGH */
	case SH_NET_V4_DHCP_MANUAL:
		if (!v4_ok(s->ipv4_address, NULL))
			return (SH_ERR_INVALID);
		break;
	default:
		return (SH_ERR_INVALID);
	}
	switch (s->ipv6_method) {
	case SH_NET_V6_AUTOMATIC:
	case SH_NET_V6_LINK_LOCAL:
	case SH_NET_V6_OFF:
		break;
	case SH_NET_V6_MANUAL:
		/* ipconfigd won't take a link-local address as Manual. */
		if (!v6_ok(s->ipv6_address, &a6) || IN6_IS_ADDR_LINKLOCAL(&a6) ||
		    s->ipv6_prefix < 1 || s->ipv6_prefix > 128)
			return (SH_ERR_INVALID);
		if (s->ipv6_router[0] != '\0' && !v6_ok(s->ipv6_router, NULL))
			return (SH_ERR_INVALID);
		break;
	default:
		return (SH_ERR_INVALID);
	}
	if (!parse_list(s->dns_servers, server_ok, &servers) ||
	    !parse_list(s->search_domains, domain_ok, &domains))
		return (SH_ERR_INVALID);

	prefs = open_prefs();
	if (prefs == NULL)
		return (SH_ERR_FAILED);
	id = cfstr(s->service_id);
	service = SCNetworkServiceCopy(prefs, id);
	CFRelease(id);
	if (service == NULL) {
		(void)SCPreferencesUnlock(prefs);
		CFRelease(prefs);
		return (SH_ERR_INVALID);	/* no such service */
	}

	/* Off keeps the entity's settings for when it's on again. */
	v4 = s->ipv4_method == SH_NET_V4_OFF ? NULL :
	    ipv4_config(s->ipv4_method, s->ipv4_address, s->ipv4_mask,
	    s->ipv4_router);
	v6 = s->ipv6_method == SH_NET_V6_OFF ? NULL :
	    ipv6_config(s->ipv6_method, s->ipv6_address, s->ipv6_prefix,
	    s->ipv6_router);
	dns = dns_config(service, &servers, &domains);
	ok = set_protocol(service, kSCNetworkProtocolTypeIPv4, v4,
	    s->ipv4_method != SH_NET_V4_OFF) &&
	    set_protocol(service, kSCNetworkProtocolTypeIPv6, v6,
	    s->ipv6_method != SH_NET_V6_OFF) &&
	    set_protocol(service, kSCNetworkProtocolTypeDNS, dns, true);
	if (v4 != NULL)
		CFRelease(v4);
	if (v6 != NULL)
		CFRelease(v6);
	CFRelease(dns);
	CFRelease(service);
	if (!ok) {
		sh_log("network service %s: %s", s->service_id,
		    SCErrorString(SCError()));
		(void)SCPreferencesUnlock(prefs);
		CFRelease(prefs);
		return (SH_ERR_FAILED);
	}
	rc = commit(prefs);
	CFRelease(prefs);
	return (rc);
}

/* Valid UTF-8 with no control characters. */
static bool
computer_name_ok(const char *name)
{
	const unsigned char *p;
	CFStringRef s;

	if (name[0] == '\0' || strlen(name) > SH_COMPUTER_NAME_MAX)
		return (false);
	for (p = (const unsigned char *)name; *p != '\0'; p++)
		if (*p < 0x20 || *p == 0x7f)
			return (false);
	s = cfstr(name);		/* NULL: not UTF-8 */
	if (s == NULL)
		return (false);
	CFRelease(s);
	return (true);
}

/* A DNS label: letters, digits and hyphens, not at either end. */
static bool
local_name_ok(const char *name)
{
	size_t n = strlen(name);
	const char *p;

	if (n == 0 || n > SH_LOCAL_NAME_MAX || name[0] == '-' ||
	    name[n - 1] == '-')
		return (false);
	for (p = name; *p != '\0'; p++)
		if (!isalnum((unsigned char)*p) && *p != '-')
			return (false);
	return (true);
}

int
sh_computer_name_set(const char *computer, const char *local)
{
	SCPreferencesRef prefs;
	CFStringRef c, l;
	bool ok;
	int rc;

	if (!computer_name_ok(computer) || !local_name_ok(local))
		return (SH_ERR_INVALID);
	prefs = open_prefs();
	if (prefs == NULL)
		return (SH_ERR_FAILED);
	c = cfstr(computer);
	l = cfstr(local);
	ok = SCPreferencesSetComputerName(prefs, c, kCFStringEncodingUTF8) &&
	    SCPreferencesSetLocalHostName(prefs, l);
	CFRelease(c);
	CFRelease(l);
	if (!ok) {
		sh_log("computer name: %s", SCErrorString(SCError()));
		(void)SCPreferencesUnlock(prefs);
		CFRelease(prefs);
		return (SH_ERR_FAILED);
	}
	rc = commit(prefs);
	CFRelease(prefs);
	return (rc);
}
