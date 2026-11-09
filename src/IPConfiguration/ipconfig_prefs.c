/*
 * ipconfig_prefs.c — per-interface IPv4 and IPv6 methods from Setup: (#570,
 * #58, #423).
 *
 * See ipconfig_prefs.h for the model. This is the only CF translation
 * unit on the configuration side: ipconfigd.c sees plain-C
 * struct ipconfig_method_info.
 */
#include "ipconfig_prefs.h"
#include "bound_state.h"	/* BOUND_MAX_IF */
#include "sc_publish.h"		/* SC_PUBLISH_SERVICE_ID_LEN, proxies */
#include "dns_config.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCNetworkConfiguration.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>
#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The SCSchema names the IPv4 / DNS entities use. Apple takes them from
 * SCSchemaDefinitions.h; ours carries only the subset other daemons
 * needed so far, so spell them here.
 */
#define	kIPCfgPropConfigMethod		CFSTR("ConfigMethod")
#define	kIPCfgPropFailoverAddressTimeout CFSTR("FailoverAddressTimeout")
#define	kIPCfgPropDNSServerAddresses	CFSTR("ServerAddresses")
#define	kIPCfgPropDNSSearchDomains	CFSTR("SearchDomains")
#define	kIPCfgPropDNSDomainName		CFSTR("DomainName")
#define	kIPCfgPropDNSSortList		CFSTR("SortList")
#define	kIPCfgPropDNSOptions		CFSTR("Options")
#define	kIPCfgPropDNSSearchOrder	CFSTR("SearchOrder")
#define	kIPCfgPropDNSServerPort		CFSTR("ServerPort")
#define	kIPCfgPropDNSServerTimeout	CFSTR("ServerTimeout")
#define	kIPCfgPropDNSServiceIdentifier	CFSTR("ServiceIdentifier")
#define	kIPCfgPropDNSMatchDomains	CFSTR("SupplementalMatchDomains")
#define	kIPCfgPropDNSMatchOrders	CFSTR("SupplementalMatchOrders")
#define	kIPCfgPropDNSMatchNoSearch	CFSTR("SupplementalMatchDomainsNoSearch")
#define	kIPCfgPropServiceOrder		CFSTR("ServiceOrder")

struct prefs_entry {
	char				ifname[IFNAMSIZ];
	char				service_id[SC_PUBLISH_SERVICE_ID_LEN];
	struct ipconfig_method_info	info;
	struct ipconfig_v6_method_info	v6;
	bool				have_dns;
	struct ipconfig_dns		dns;	/* zeroed unless have_dns */
	int				order;	/* in ServiceOrder (#570) */
	int				primary_rank; /* IPCFG_PRIMARY_RANK_* */
};

struct prefs_table {
	struct prefs_entry	e[BOUND_MAX_IF];
	int			n;
};

static pthread_mutex_t		g_prefs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct prefs_table	g_table;	/* guarded by g_prefs_lock */
static ipconfig_prefs_cb	g_cb;
static ipconfig_prefs_cb	g_dns_cb;
static ipconfig_prefs_event_cb	g_loaded_cb;
static ipconfig_prefs_event_cb	g_order_cb;
static SCDynamicStoreRef	g_store;
static bool			g_loaded;	/* guarded by g_prefs_lock */

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[prefs] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static const struct {
	const char		*name;
	ipconfig_method_t	method;
} g_methods[] = {
	{ "None",	ipconfig_method_none_e },
	{ "Manual",	ipconfig_method_manual_e },
	{ "BOOTP",	ipconfig_method_bootp_e },
	{ "DHCP",	ipconfig_method_dhcp_e },
	{ "INFORM",	ipconfig_method_inform_e },
	{ "LinkLocal",	ipconfig_method_linklocal_e },
	{ "Failover",	ipconfig_method_failover_e },
};

const char *
ipconfig_method_string(ipconfig_method_t m)
{
	size_t i;

	for (i = 0; i < sizeof(g_methods) / sizeof(g_methods[0]); i++) {
		if (g_methods[i].method == m)
			return (g_methods[i].name);
	}
	return ("?");
}

bool
ipconfig_method_is_served(ipconfig_method_t m)
{
	switch (m) {
	case ipconfig_method_manual_e:
	case ipconfig_method_bootp_e:
	case ipconfig_method_dhcp_e:
	case ipconfig_method_inform_e:
	case ipconfig_method_linklocal_e:
	case ipconfig_method_failover_e:
		return (true);
	default:
		return (false);
	}
}

/* The IPv6 ConfigMethod names (ipconfig_method_from_cfstring_ipv6). */
static const struct {
	const char		*name;
	ipconfig_method_t	method;
} g_v6_methods[] = {
	{ "Manual",		ipconfig_method_manual_v6_e },
	{ "Automatic",		ipconfig_method_automatic_v6_e },
	{ "RouterAdvertisement", ipconfig_method_rtadv_e },
	{ "6to4",		ipconfig_method_stf_e },
	{ "LinkLocal",		ipconfig_method_linklocal_v6_e },
};

const char *
ipconfig_v6_method_string(ipconfig_method_t m)
{
	switch (m) {
	case ipconfig_method_none_v6_e:
		return ("NONE-V6");
	case ipconfig_method_manual_v6_e:
		return ("MANUAL-V6");
	case ipconfig_method_automatic_v6_e:
		return ("AUTOMATIC-V6");
	case ipconfig_method_rtadv_e:
		return ("RTADV");
	case ipconfig_method_stf_e:
		return ("6TO4");
	case ipconfig_method_linklocal_v6_e:
		return ("LINKLOCAL-V6");
	case ipconfig_method_unmanaged_v6_e:
		return ("UNMANAGED-V6");
	default:
		return ("?");
	}
}

/* A CFString holding a dotted-quad IPv4 address. */
static bool
cf_to_in_addr(CFTypeRef v, struct in_addr *out)
{
	char buf[INET_ADDRSTRLEN];

	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return (false);
	if (!CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingASCII))
		return (false);
	return (inet_pton(AF_INET, buf, out) == 1);
}

/* Element 0 of a CFArray of address strings (Addresses, SubnetMasks). */
static bool
cf_first_in_addr(CFTypeRef v, struct in_addr *out)
{
	if (v == NULL || CFGetTypeID(v) != CFArrayGetTypeID() ||
	    CFArrayGetCount(v) == 0)
		return (false);
	return (cf_to_in_addr(CFArrayGetValueAtIndex(v, 0), out));
}

/*
 * The classful mask for `a`, which is what a Manual service with no
 * SubnetMasks gets (Apple's ip_mask_for_addr fallback).
 */
struct in_addr
ipconfig_classful_mask(struct in_addr a)
{
	struct in_addr m;
	uint32_t h = ntohl(a.s_addr);

	if (IN_CLASSA(h))
		m.s_addr = htonl(IN_CLASSA_NET);
	else if (IN_CLASSB(h))
		m.s_addr = htonl(IN_CLASSB_NET);
	else
		m.s_addr = htonl(IN_CLASSC_NET);
	return (m);
}

static bool	cf_is_array(CFTypeRef v);
static bool	cf_to_int32(CFTypeRef v, int32_t *out);

bool
ipconfig_method_info_from_dict(const struct __CFDictionary *ipv4,
    struct ipconfig_method_info *out)
{
	CFDictionaryRef d = (CFDictionaryRef)ipv4;
	CFTypeRef method;
	size_t k;
	bool known = false;

	(void)memset(out, 0, sizeof(*out));
	if (d == NULL || CFGetTypeID(d) != CFDictionaryGetTypeID())
		return (false);

	method = CFDictionaryGetValue(d, kIPCfgPropConfigMethod);
	if (method == NULL || CFGetTypeID(method) != CFStringGetTypeID())
		return (false);
	for (k = 0; k < sizeof(g_methods) / sizeof(g_methods[0]); k++) {
		CFStringRef s;

		s = CFStringCreateWithCString(NULL, g_methods[k].name,
		    kCFStringEncodingASCII);
		if (s == NULL)
			continue;
		if (CFEqual(method, s)) {
			out->method = g_methods[k].method;
			known = true;
		}
		CFRelease(s);
		if (known)
			break;
	}
	if (!known)
		return (false);

	/* ipconfig_method_is_manual(): each of these must have an address. */
	if (out->method == ipconfig_method_manual_e ||
	    out->method == ipconfig_method_inform_e ||
	    out->method == ipconfig_method_failover_e) {
		if (!cf_first_in_addr(CFDictionaryGetValue(d,
		    kSCPropNetIPv4Addresses), &out->addr) ||
		    out->addr.s_addr == INADDR_ANY)
			return (false);
		if (!cf_first_in_addr(CFDictionaryGetValue(d,
		    kSCPropNetIPv4SubnetMasks), &out->mask) ||
		    out->mask.s_addr == INADDR_ANY) {
			/* INFORM takes the server's mask if it has none. */
			out->mask.s_addr = INADDR_ANY;
			if (out->method != ipconfig_method_inform_e)
				out->mask = ipconfig_classful_mask(out->addr);
		}
	}
	if (out->method == ipconfig_method_manual_e)
		(void)cf_to_in_addr(CFDictionaryGetValue(d,
		    kSCPropNetIPv4Router), &out->router);
	if (out->method == ipconfig_method_failover_e) {
		int32_t timeout;

		if (cf_to_int32(CFDictionaryGetValue(d,
		    kIPCfgPropFailoverAddressTimeout), &timeout) &&
		    timeout > 0)
			out->failover_timeout = (uint32_t)timeout;
	}
	return (true);
}

/* A CFString holding an IPv6 address (my_CFStringToIPv6Address). */
static bool
cf_to_in6_addr(CFTypeRef v, struct in6_addr *out)
{
	char buf[64];

	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return (false);
	if (!CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingASCII))
		return (false);
	return (inet_pton(AF_INET6, buf, out) == 1);
}

bool
ipconfig_v6_method_info_from_dict(const struct __CFDictionary *ipv6,
    struct ipconfig_v6_method_info *out)
{
	CFDictionaryRef d = (CFDictionaryRef)ipv6;
	CFTypeRef method, addresses, prefixes;
	size_t k;
	bool known = false;

	(void)memset(out, 0, sizeof(*out));
	if (d == NULL || CFGetTypeID(d) != CFDictionaryGetTypeID())
		return (false);

	method = CFDictionaryGetValue(d, kIPCfgPropConfigMethod);
	if (method == NULL || CFGetTypeID(method) != CFStringGetTypeID())
		return (false);
	for (k = 0; k < sizeof(g_v6_methods) / sizeof(g_v6_methods[0]); k++) {
		CFStringRef s;

		s = CFStringCreateWithCString(NULL, g_v6_methods[k].name,
		    kCFStringEncodingASCII);
		if (s == NULL)
			continue;
		if (CFEqual(method, s)) {
			out->method = g_v6_methods[k].method;
			known = true;
		}
		CFRelease(s);
		if (known)
			break;
	}
	if (!known)
		return (false);
	if (out->method != ipconfig_method_manual_v6_e)
		return (true);

	/* method_info_from_ipv6_dict(): the first address is the one used. */
	addresses = CFDictionaryGetValue(d, kSCPropNetIPv6Addresses);
	prefixes = CFDictionaryGetValue(d, kSCPropNetIPv6PrefixLength);
	if (!cf_is_array(addresses) || CFArrayGetCount(addresses) == 0 ||
	    !cf_to_in6_addr(CFArrayGetValueAtIndex(addresses, 0), &out->addr))
		return (false);
	if (IN6_IS_ADDR_LINKLOCAL(&out->addr))
		return (false);
	if (prefixes != NULL) {
		int32_t prefix;

		if (!cf_is_array(prefixes) ||
		    CFArrayGetCount(prefixes) != CFArrayGetCount(addresses) ||
		    !cf_to_int32(CFArrayGetValueAtIndex(prefixes, 0), &prefix) ||
		    prefix < 0 || prefix > 128)
			return (false);
		out->prefix_len = (uint8_t)prefix;
	}
	/*
	 * Darwin leaves a missing PrefixLength as 0, which would install
	 * the address with no usable on-link prefix; /64 is the usual case.
	 */
	if (out->prefix_len == 0)
		out->prefix_len = 64;
	return (true);
}

/*
 * A CFString as a trimmed C string; false if it is not one, is too long
 * or is empty once trimmed (Apple's sanitize_prop).
 */
static bool
cf_to_trimmed(CFTypeRef v, char *out, size_t outsz)
{
	size_t len, start = 0;

	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID() ||
	    !CFStringGetCString(v, out, (CFIndex)outsz,
	    kCFStringEncodingUTF8))
		return (false);
	len = strlen(out);
	while (start < len && isspace((unsigned char)out[start]))
		start++;
	while (len > start && isspace((unsigned char)out[len - 1]))
		len--;
	if (len == start)
		return (false);
	(void)memmove(out, out + start, len - start);
	(void)memset(out + (len - start), 0, outsz - (len - start));
	return (true);
}

/* A CFArray of strings into a fixed-size list; returns the number kept. */
static unsigned
cf_string_list(CFTypeRef v, char *list, size_t stride, unsigned max)
{
	CFIndex i, n;
	unsigned count = 0;

	n = CFArrayGetCount(v);
	for (i = 0; i < n && count < max; i++) {
		if (cf_to_trimmed(CFArrayGetValueAtIndex(v, i),
		    list + count * stride, stride))
			count++;
		else
			(void)memset(list + count * stride, 0, stride);
	}
	return (count);
}

/*
 * SupplementalMatchDomains: as cf_string_list(), but an empty string is a
 * real entry (merge_array_prop()'s ALLOW_EMPTY_STRING): it matches all names.
 */
static unsigned
cf_match_list(CFTypeRef v, char *list, size_t stride, unsigned max)
{
	CFIndex i, n;
	unsigned count = 0;

	n = CFArrayGetCount(v);
	for (i = 0; i < n && count < max; i++) {
		CFTypeRef e = CFArrayGetValueAtIndex(v, i);

		if (e == NULL || CFGetTypeID(e) != CFStringGetTypeID())
			continue;
		if (!cf_to_trimmed(e, list + count * stride, stride))
			(void)memset(list + count * stride, 0, stride);
		count++;
	}
	return (count);
}

static bool
cf_is_array(CFTypeRef v)
{
	return (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID());
}

static bool
cf_to_int32(CFTypeRef v, int32_t *out)
{
	return (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID() &&
	    CFNumberGetValue(v, kCFNumberSInt32Type, out));
}

/*
 * The service's DNS entity (Darwin's Setup:/Network/Service/<id>/DNS).
 * Addresses are kept as written and validated by the merge, as
 * accumulate_dns_servers() validates them: IPv4 and IPv6 alike.
 */
static void
dns_from_dict(CFDictionaryRef d, struct ipconfig_dns *out)
{
	CFTypeRef v;

	(void)memset(out, 0, sizeof(*out));
	v = CFDictionaryGetValue(d, kIPCfgPropDNSServerAddresses);
	if (cf_is_array(v))
		out->server_count = cf_string_list(v, out->servers[0],
		    IPCFG_DNS_ADDR_LEN, IPCFG_DNS_MAX_SERVERS);
	v = CFDictionaryGetValue(d, kIPCfgPropDNSSearchDomains);
	if (cf_is_array(v)) {
		out->search_count = cf_string_list(v, out->search[0],
		    IPCFG_DNS_NAME_LEN, IPCFG_DNS_MAX_SEARCH);
		out->has_search = true;
	}
	v = CFDictionaryGetValue(d, kIPCfgPropDNSSortList);
	if (cf_is_array(v)) {
		out->sort_count = cf_string_list(v, out->sortlist[0],
		    IPCFG_DNS_SORT_LEN, IPCFG_DNS_MAX_SORT);
		out->has_sortlist = true;
	}
	if (!cf_to_trimmed(CFDictionaryGetValue(d, kIPCfgPropDNSDomainName),
	    out->domain, sizeof(out->domain)))
		(void)memset(out->domain, 0, sizeof(out->domain));
	if (!cf_to_trimmed(CFDictionaryGetValue(d, kIPCfgPropDNSOptions),
	    out->options, sizeof(out->options)))
		(void)memset(out->options, 0, sizeof(out->options));
	out->has_search_order = cf_to_int32(CFDictionaryGetValue(d,
	    kIPCfgPropDNSSearchOrder), &out->search_order);
	out->has_port = cf_to_int32(CFDictionaryGetValue(d,
	    kIPCfgPropDNSServerPort), &out->port);
	out->has_timeout = cf_to_int32(CFDictionaryGetValue(d,
	    kIPCfgPropDNSServerTimeout), &out->timeout);
	out->has_service_id = cf_to_int32(CFDictionaryGetValue(d,
	    kIPCfgPropDNSServiceIdentifier), &out->service_id);
	v = CFDictionaryGetValue(d, kIPCfgPropDNSMatchDomains);
	if (cf_is_array(v)) {
		out->match_count = cf_match_list(v, out->match_domains[0],
		    IPCFG_DNS_NAME_LEN, IPCFG_DNS_MAX_MATCH);
		out->has_match_domains = true;
	}
	v = CFDictionaryGetValue(d, kIPCfgPropDNSMatchOrders);
	if (cf_is_array(v)) {
		CFIndex i, n = CFArrayGetCount(v);

		for (i = 0; i < n &&
		    out->match_order_count < IPCFG_DNS_MAX_MATCH; i++) {
			if (cf_to_int32(CFArrayGetValueAtIndex(v, i),
			    &out->match_orders[out->match_order_count]))
				out->match_order_count++;
		}
		out->has_match_orders = true;
	}
	out->has_no_search = cf_to_int32(CFDictionaryGetValue(d,
	    kIPCfgPropDNSMatchNoSearch), &out->no_search);
}

/* The value of the Setup: key `key` in `values`, if it is a dictionary. */
static CFDictionaryRef
dict_value(CFDictionaryRef values, CFStringRef key)
{
	CFTypeRef v = CFDictionaryGetValue(values, key);

	return (v != NULL && CFGetTypeID(v) == CFDictionaryGetTypeID()
	    ? (CFDictionaryRef)v : NULL);
}

/* Setup:/Network/Service/<id>[/<entity>]. Caller releases. */
static CFStringRef
service_key(CFStringRef sid, CFStringRef entity)
{
	if (entity == NULL)
		return (CFStringCreateWithFormat(NULL, NULL,
		    CFSTR("Setup:/Network/Service/%@"), sid));
	return (CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("Setup:/Network/Service/%@/%@"), sid, entity));
}

/* The entity of service `sid` from `values`, or NULL. */
static CFDictionaryRef
service_entity(CFDictionaryRef values, CFStringRef sid, CFStringRef entity)
{
	CFStringRef key = service_key(sid, entity);
	CFDictionaryRef d = dict_value(values, key);

	CFRelease(key);
	return (d);
}

/*
 * Does a service with this Interface Type get configured? Darwin's
 * copy_ipv4_service_dict: Ethernet, FireWire, Loopback. Bond, Bridge, VLAN
 * and an older Wi-Fi entity (IEEE80211) are Ethernet-class here: SCNetwork
 * writes those Type values for them.
 */
static bool
interface_type_is_configured(CFStringRef type)
{
	return (CFEqual(type, CFSTR("Ethernet")) ||
	    CFEqual(type, CFSTR("FireWire")) ||
	    CFEqual(type, CFSTR("Loopback")) ||
	    CFEqual(type, CFSTR("IEEE80211")) ||
	    CFEqual(type, CFSTR("Bond")) ||
	    CFEqual(type, CFSTR("Bridge")) ||
	    CFEqual(type, CFSTR("VLAN")));
}

static const struct prefs_entry *table_find(const struct prefs_table *t,
    const char *ifname);

/* One service's table entry, from Setup:. False: skip this service. */
static bool
service_entry(CFDictionaryRef values, CFStringRef sid, int order,
    struct prefs_entry *e)
{
	CFDictionaryRef iface, ipv4, ipv6, dns, svc;
	CFStringRef dev, type, rank;

	(void)memset(e, 0, sizeof(*e));
	e->order = order;
	e->primary_rank = IPCFG_PRIMARY_RANK_DEFAULT;

	iface = service_entity(values, sid, kSCEntNetInterface);
	if (iface == NULL)
		return (false);
	dev = CFDictionaryGetValue(iface, kSCPropNetInterfaceDeviceName);
	type = CFDictionaryGetValue(iface, kSCPropNetInterfaceType);
	if (dev == NULL || CFGetTypeID(dev) != CFStringGetTypeID() ||
	    !CFStringGetCString(dev, e->ifname, sizeof(e->ifname),
	    kCFStringEncodingASCII))
		return (false);
	if (type != NULL && (CFGetTypeID(type) != CFStringGetTypeID() ||
	    !interface_type_is_configured(type))) {
		xlog("%s: not a kind of interface ipconfigd configures",
		    e->ifname);
		return (false);
	}

	if (!CFStringGetCString(sid, e->service_id, sizeof(e->service_id),
	    kCFStringEncodingUTF8)) {
		xlog("%s: service ID is too long", e->ifname);
		e->service_id[0] = '\0';
	}

	svc = service_entity(values, sid, NULL);
	rank = (svc != NULL) ? CFDictionaryGetValue(svc,
	    kSCPropNetServicePrimaryRank) : NULL;
	if (rank != NULL && CFGetTypeID(rank) == CFStringGetTypeID()) {
		if (CFEqual(rank, kSCValNetServicePrimaryRankFirst))
			e->primary_rank = IPCFG_PRIMARY_RANK_FIRST;
		else if (CFEqual(rank, kSCValNetServicePrimaryRankLast))
			e->primary_rank = IPCFG_PRIMARY_RANK_LAST;
		else if (CFEqual(rank, kSCValNetServicePrimaryRankNever))
			e->primary_rank = IPCFG_PRIMARY_RANK_NEVER;
	}

	ipv4 = service_entity(values, sid, kSCEntNetIPv4);
	ipv6 = service_entity(values, sid, kSCEntNetIPv6);
	dns = service_entity(values, sid, kSCEntNetDNS);
	if (ipv4 == NULL) {
		/* A service with IPv4 off: Apple runs no v4 service. */
		e->info.method = ipconfig_method_none_e;
	} else if (!ipconfig_method_info_from_dict(
	    (const struct __CFDictionary *)ipv4, &e->info)) {
		/*
		 * Unusable ConfigMethod (or Manual with no address).
		 * Leaving the interface alone is safer than DHCP'ing
		 * over what the administrator meant.
		 */
		xlog("%s: invalid IPv4 configuration; leaving it alone",
		    e->ifname);
		(void)memset(&e->info, 0, sizeof(e->info));
		e->info.method = ipconfig_method_none_e;
	}
	/*
	 * IPv6 (#423). No entity, or an invalid one, means no IPv6 service
	 * and IPv6 off, as on Darwin (an inactive one never reaches Setup:).
	 * 6to4 parses but has no service behind it, so that interface's IPv6
	 * is left alone, like an unserved IPv4 method. Loopback does Manual
	 * or nothing (#570).
	 */
	e->v6.method = ipconfig_method_none_v6_e;
	if (ipv6 != NULL && !ipconfig_v6_method_info_from_dict(
	    (const struct __CFDictionary *)ipv6, &e->v6)) {
		xlog("%s: invalid IPv6 configuration; IPv6 off", e->ifname);
		(void)memset(&e->v6, 0, sizeof(e->v6));
		e->v6.method = ipconfig_method_none_v6_e;
	}
	if (e->v6.method == ipconfig_method_stf_e) {
		xlog("%s: IPv6 ConfigMethod 6to4 is not supported; "
		    "leaving IPv6 alone", e->ifname);
		(void)memset(&e->v6, 0, sizeof(e->v6));
		e->v6.method = ipconfig_method_unmanaged_v6_e;
	}
	if (strncmp(e->ifname, "lo", 2) == 0 &&
	    e->v6.method != ipconfig_method_manual_v6_e) {
		(void)memset(&e->v6, 0, sizeof(e->v6));
		e->v6.method = ipconfig_method_unmanaged_v6_e;
	}
	if (dns != NULL) {
		dns_from_dict(dns, &e->dns);
		e->have_dns = true;
	}
	if (!ipconfig_method_is_served(e->info.method) &&
	    e->info.method != ipconfig_method_none_e)
		xlog("%s: ConfigMethod %s is not supported; leaving it alone",
		    e->ifname, ipconfig_method_string(e->info.method));
	return (true);
}

/* Add `sid`'s service to `t`; the first service naming an interface wins. */
static void
table_add(struct prefs_table *t, CFDictionaryRef values, CFStringRef sid,
    int order)
{
	struct prefs_entry e;

	if (!service_entry(values, sid, order, &e))
		return;
	if (table_find(t, e.ifname) != NULL) {
		xlog("%s: more than one service names it; using the first",
		    e.ifname);
		return;
	}
	if (t->n == BOUND_MAX_IF) {
		xlog("%s: too many configured services; ignoring", e.ifname);
		return;
	}
	t->e[t->n++] = e;
}

/* The service ID out of a Setup:/Network/Service/<id>/Interface key. */
static CFStringRef
service_id_of_key(CFStringRef key)
{
	CFArrayRef parts;
	CFStringRef sid = NULL;

	parts = CFStringCreateArrayBySeparatingStrings(NULL, key, CFSTR("/"));
	if (parts != NULL && CFArrayGetCount(parts) >= 4)
		sid = CFRetain(CFArrayGetValueAtIndex(parts, 3));
	if (parts != NULL)
		CFRelease(parts);
	return (sid);
}

static CFComparisonResult
compare_ids(const void *a, const void *b, void *ctx)
{
	(void)ctx;
	return (CFStringCompare(a, b, 0));
}

/*
 * Rebuild `t` from Setup: (#570), in service order as Darwin's
 * make_ordered does: PreferencesMonitor flattens the current set there on
 * every Apply, so an inactive service is simply not present. Returns whether
 * the configuration has been read at all: a Setup: key says it has, and an
 * interface no service names is then really unconfigured.
 */
static bool
prefs_read(struct prefs_table *t)
{
	CFStringRef patterns_v[1];
	CFStringRef keys_v[2];
	CFArrayRef patterns, keys, order;
	CFDictionaryRef values, global;
	CFMutableArrayRef ids;
	CFIndex i, n;
	const void **all;
	bool loaded;

	(void)memset(t, 0, sizeof(*t));
	if (g_store == NULL)
		return (false);

	keys_v[0] = kSCDynamicStoreDomainSetup;
	keys_v[1] = CFSTR("Setup:/Network/Global/IPv4");
	keys = CFArrayCreate(NULL, (const void **)keys_v, 2,
	    &kCFTypeArrayCallBacks);
	patterns_v[0] = CFSTR("Setup:/Network/Service/.*");
	patterns = CFArrayCreate(NULL, (const void **)patterns_v, 1,
	    &kCFTypeArrayCallBacks);
	values = SCDynamicStoreCopyMultiple(g_store, keys, patterns);
	CFRelease(keys);
	CFRelease(patterns);
	if (values == NULL)
		return (false);

	loaded = CFDictionaryContainsKey(values, kSCDynamicStoreDomainSetup);

	ids = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	n = CFDictionaryGetCount(values);
	all = calloc((size_t)(n > 0 ? n : 1), sizeof(*all));
	if (n > 0)
		CFDictionaryGetKeysAndValues(values, all, NULL);
	for (i = 0; i < n; i++) {
		CFStringRef key = all[i], sid;

		if (!CFStringHasSuffix(key, CFSTR("/Interface")))
			continue;
		sid = service_id_of_key(key);
		if (sid != NULL) {
			CFArrayAppendValue(ids, sid);
			CFRelease(sid);
		}
	}
	free(all);
	/* A dictionary's order means nothing: stable ties. */
	CFArraySortValues(ids, CFRangeMake(0, CFArrayGetCount(ids)),
	    compare_ids, NULL);

	global = dict_value(values, keys_v[1]);
	order = (global != NULL) ? CFDictionaryGetValue(global,
	    kIPCfgPropServiceOrder) : NULL;
	n = (order != NULL && CFGetTypeID(order) == CFArrayGetTypeID())
	    ? CFArrayGetCount(order) : 0;
	for (i = 0; i < n; i++) {
		CFStringRef sid = CFArrayGetValueAtIndex(order, i);
		CFIndex at;

		if (CFGetTypeID(sid) != CFStringGetTypeID())
			continue;
		at = CFArrayGetFirstIndexOfValue(ids,
		    CFRangeMake(0, CFArrayGetCount(ids)), sid);
		if (at == kCFNotFound)
			continue;
		table_add(t, values, sid, (int)i);
		CFArrayRemoveValueAtIndex(ids, at);
	}
	/* Services ServiceOrder does not list come after those it does. */
	n = CFArrayGetCount(ids);
	for (i = 0; i < n; i++)
		table_add(t, values, CFArrayGetValueAtIndex(ids, i),
		    IPCFG_ORDER_UNLISTED);

	CFRelease(ids);
	CFRelease(values);
	return (loaded);
}

static const struct prefs_entry *
table_find(const struct prefs_table *t, const char *ifname)
{
	int i;

	for (i = 0; i < t->n; i++) {
		if (strcmp(t->e[i].ifname, ifname) == 0)
			return (&t->e[i]);
	}
	return (NULL);
}

/*
 * Did `ifname`'s configured method differ between `a` and `b`? DNS is not
 * part of this: see table_dns_changed().
 */
static bool
table_changed(const struct prefs_table *a, const struct prefs_table *b,
    const char *ifname)
{
	const struct prefs_entry *x = table_find(a, ifname);
	const struct prefs_entry *y = table_find(b, ifname);

	if (x == NULL || y == NULL)
		return (x != y);
	return (memcmp(&x->info, &y->info, sizeof(x->info)) != 0 ||
	    memcmp(&x->v6, &y->v6, sizeof(x->v6)) != 0 ||
	    strcmp(x->service_id, y->service_id) != 0);
}

/* Did `ifname`'s DNS entity differ between `a` and `b`? */
static bool
table_dns_changed(const struct prefs_table *a, const struct prefs_table *b,
    const char *ifname)
{
	const struct prefs_entry *x = table_find(a, ifname);
	const struct prefs_entry *y = table_find(b, ifname);
	bool hx = x != NULL && x->have_dns, hy = y != NULL && y->have_dns;

	if (!hx || !hy)
		return (hx != hy);
	return (memcmp(&x->dns, &y->dns, sizeof(x->dns)) != 0);
}

static void
log_table(const struct prefs_table *t)
{
	char a[INET_ADDRSTRLEN];
	int i;

	for (i = 0; i < t->n; i++) {
		(void)inet_ntop(AF_INET, &t->e[i].info.addr, a, sizeof(a));
		xlog("%s: ConfigMethod %s%s%s", t->e[i].ifname,
		    ipconfig_method_string(t->e[i].info.method),
		    t->e[i].info.method == ipconfig_method_manual_e
		        ? " " : "",
		    t->e[i].info.method == ipconfig_method_manual_e ? a : "");
		xlog("%s: IPv6 ConfigMethod %s", t->e[i].ifname,
		    ipconfig_v6_method_string(t->e[i].v6.method));
	}
}

/* Did a service's place in the election move between `a` and `b`? */
static bool
table_order_changed(const struct prefs_table *a, const struct prefs_table *b)
{
	int i;

	if (a->n != b->n)
		return (true);
	for (i = 0; i < a->n; i++) {
		if (strcmp(a->e[i].ifname, b->e[i].ifname) != 0 ||
		    a->e[i].order != b->e[i].order ||
		    a->e[i].primary_rank != b->e[i].primary_rank)
			return (true);
	}
	return (false);
}

/*
 * Setup: changed: PreferencesMonitor flattened an Apply. Re-read it, swap
 * the table, and tell ipconfigd about each interface whose method or DNS
 * moved. The tables are static because they are large for a queue thread's
 * stack; the queue is serial.
 */
static void
prefs_refresh(void *ctx)
{
	static struct prefs_table next, prev;
	bool loaded, first_load;
	int i;

	(void)ctx;
	loaded = prefs_read(&next);

	pthread_mutex_lock(&g_prefs_lock);
	prev = g_table;
	g_table = next;
	first_load = loaded && !g_loaded;
	if (loaded)
		g_loaded = true;
	pthread_mutex_unlock(&g_prefs_lock);

	log_table(&next);
	/* An Apply can change the primary service's Proxies alone (#573). */
	sc_publish_refresh_proxies();
	if (g_cb != NULL) {
		for (i = 0; i < prev.n; i++) {
			if (table_changed(&prev, &next, prev.e[i].ifname))
				g_cb(prev.e[i].ifname);
		}
		for (i = 0; i < next.n; i++) {
			if (table_find(&prev, next.e[i].ifname) == NULL)
				g_cb(next.e[i].ifname);
		}
	}
	if (g_dns_cb != NULL) {
		for (i = 0; i < prev.n; i++) {
			if (table_dns_changed(&prev, &next, prev.e[i].ifname))
				g_dns_cb(prev.e[i].ifname);
		}
		for (i = 0; i < next.n; i++) {
			if (table_find(&prev, next.e[i].ifname) == NULL &&
			    table_dns_changed(&prev, &next, next.e[i].ifname))
				g_dns_cb(next.e[i].ifname);
		}
	}
	if (g_order_cb != NULL && table_order_changed(&prev, &next))
		g_order_cb();
	if (first_load && g_loaded_cb != NULL)
		g_loaded_cb();
}

static void
setup_changed(SCDynamicStoreRef store, CFArrayRef changed, void *info)
{
	(void)store;
	(void)changed;
	(void)info;
	prefs_refresh(NULL);
}

int
ipconfig_prefs_start(ipconfig_prefs_cb cb, ipconfig_prefs_cb dns_cb,
    ipconfig_prefs_event_cb loaded_cb, ipconfig_prefs_event_cb order_cb)
{
	dispatch_queue_t q;
	CFStringRef keys_v[2], patterns_v[1];
	CFArrayRef keys, patterns;
	int tries;

	g_cb = cb;
	g_dns_cb = dns_cb;
	g_loaded_cb = loaded_cb;
	g_order_cb = order_cb;

	/* ipconfigd and configd are both RunAtLoad, with no ordering. */
	for (tries = 0; tries < 60; tries++) {
		g_store = SCDynamicStoreCreate(NULL, CFSTR("ipconfigd-setup"),
		    setup_changed, NULL);
		if (g_store != NULL)
			break;
		(void)sleep(1);
	}
	if (g_store == NULL) {
		xlog("no configd session; no service can be configured");
		return (-1);
	}

	keys_v[0] = kSCDynamicStoreDomainSetup;
	keys_v[1] = CFSTR("Setup:/Network/Global/IPv4");
	keys = CFArrayCreate(NULL, (const void **)keys_v, 2,
	    &kCFTypeArrayCallBacks);
	patterns_v[0] = CFSTR("Setup:/Network/Service/.*");
	patterns = CFArrayCreate(NULL, (const void **)patterns_v, 1,
	    &kCFTypeArrayCallBacks);
	q = dispatch_queue_create("com.apple.IPConfiguration.prefs", NULL);
	if (q == NULL || !SCDynamicStoreSetNotificationKeys(g_store, keys,
	    patterns) || !SCDynamicStoreSetDispatchQueue(g_store, q)) {
		xlog("cannot watch Setup:; configuration is read once at "
		    "startup");
		CFRelease(keys);
		CFRelease(patterns);
		prefs_refresh(NULL);
		return (-1);
	}
	CFRelease(keys);
	CFRelease(patterns);
	/* On the queue, so a change cannot overtake the first read. */
	dispatch_sync_f(q, NULL, prefs_refresh);
	return (0);
}

bool
ipconfig_prefs_loaded(void)
{
	bool loaded;

	pthread_mutex_lock(&g_prefs_lock);
	loaded = g_loaded;
	pthread_mutex_unlock(&g_prefs_lock);
	return (loaded);
}

const struct __CFDictionary *
ipconfig_prefs_copy_proxies(const char *ifname)
{
	SCDynamicStoreRef store;
	const struct prefs_entry *e;
	char sid[SC_PUBLISH_SERVICE_ID_LEN] = "";
	CFStringRef key;
	CFDictionaryRef copy;

	pthread_mutex_lock(&g_prefs_lock);
	e = table_find(&g_table, ifname);
	if (e != NULL)
		(void)strlcpy(sid, e->service_id, sizeof(sid));
	pthread_mutex_unlock(&g_prefs_lock);
	if (sid[0] == '\0')
		return (NULL);

	store = SCDynamicStoreCreate(NULL, CFSTR("ipconfigd-proxies"), NULL,
	    NULL);
	if (store == NULL)
		return (NULL);
	key = CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("Setup:/Network/Service/%s/Proxies"), sid);
	copy = SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	CFRelease(store);
	if (copy != NULL && CFGetTypeID(copy) != CFDictionaryGetTypeID()) {
		CFRelease(copy);
		copy = NULL;
	}
	return ((const struct __CFDictionary *)copy);
}

void
ipconfig_prefs_primary_rank(const char *ifname, int *rank_out, int *order_out)
{
	const struct prefs_entry *e;

	*rank_out = IPCFG_PRIMARY_RANK_DEFAULT;
	*order_out = IPCFG_ORDER_UNLISTED;
	pthread_mutex_lock(&g_prefs_lock);
	e = table_find(&g_table, ifname);
	if (e != NULL) {
		*rank_out = e->primary_rank;
		*order_out = e->order;
	}
	pthread_mutex_unlock(&g_prefs_lock);
}

bool
ipconfig_prefs_lookup(const char *ifname, struct ipconfig_method_info *out,
    char *service_id, size_t len)
{
	const struct prefs_entry *e;
	bool hit = false;

	pthread_mutex_lock(&g_prefs_lock);
	e = table_find(&g_table, ifname);
	if (e != NULL) {
		*out = e->info;
		if (service_id != NULL)
			(void)strlcpy(service_id, e->service_id, len);
		hit = true;
	}
	pthread_mutex_unlock(&g_prefs_lock);
	return (hit);
}

bool
ipconfig_prefs_lookup_v6(const char *ifname,
    struct ipconfig_v6_method_info *out, char *service_id, size_t len)
{
	const struct prefs_entry *e;
	bool hit = false;

	pthread_mutex_lock(&g_prefs_lock);
	e = table_find(&g_table, ifname);
	if (e != NULL) {
		*out = e->v6;
		if (service_id != NULL)
			(void)strlcpy(service_id, e->service_id, len);
		hit = true;
	}
	pthread_mutex_unlock(&g_prefs_lock);
	return (hit);
}

bool
ipconfig_prefs_lookup_dns(const char *ifname, struct ipconfig_dns *out)
{
	const struct prefs_entry *e;
	bool hit = false;

	pthread_mutex_lock(&g_prefs_lock);
	e = table_find(&g_table, ifname);
	if (e != NULL && e->have_dns) {
		*out = e->dns;
		hit = true;
	}
	pthread_mutex_unlock(&g_prefs_lock);
	return (hit);
}
