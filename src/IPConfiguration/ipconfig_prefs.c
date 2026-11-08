/*
 * ipconfig_prefs.c — per-interface IPv4 methods from SCPreferences (#58).
 *
 * See ipconfig_prefs.h for the model. This is the only CF translation
 * unit on the configuration side: ipconfigd.c sees plain-C
 * struct ipconfig_method_info.
 */
#include "ipconfig_prefs.h"
#include "bound_state.h"	/* BOUND_MAX_IF */
#include "sc_publish.h"		/* sc_publish_refresh_proxies */
#include "dns_config.h"

#include <SystemConfiguration/SCNetworkConfiguration.h>
#include <SystemConfiguration/SCPreferences.h>
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
#include <string.h>

/*
 * The SCSchema names the IPv4 / DNS entities use. Apple takes them from
 * SCSchemaDefinitions.h; ours carries only the subset other daemons
 * needed so far, so spell them here.
 */
#define	kIPCfgPropConfigMethod		CFSTR("ConfigMethod")
#define	kIPCfgPropDNSServerAddresses	CFSTR("ServerAddresses")
#define	kIPCfgPropDNSSearchDomains	CFSTR("SearchDomains")
#define	kIPCfgPropDNSDomainName		CFSTR("DomainName")
#define	kIPCfgPropDNSSortList		CFSTR("SortList")
#define	kIPCfgPropDNSOptions		CFSTR("Options")
#define	kIPCfgPropDNSSearchOrder	CFSTR("SearchOrder")
#define	kIPCfgPropDNSServerPort		CFSTR("ServerPort")
#define	kIPCfgPropDNSServerTimeout	CFSTR("ServerTimeout")
#define	kIPCfgPropDNSServiceIdentifier	CFSTR("ServiceIdentifier")

struct prefs_entry {
	char				ifname[IFNAMSIZ];
	struct ipconfig_method_info	info;
	bool				have_dns;
	struct ipconfig_dns		dns;	/* zeroed unless have_dns */
};

struct prefs_table {
	struct prefs_entry	e[BOUND_MAX_IF];
	int			n;
};

static pthread_mutex_t		g_prefs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct prefs_table	g_table;	/* guarded by g_prefs_lock */
static ipconfig_prefs_cb	g_cb;
static ipconfig_prefs_cb	g_dns_cb;
static SCPreferencesRef		g_prefs;

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
	return (m == ipconfig_method_dhcp_e || m == ipconfig_method_manual_e);
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
static struct in_addr
classful_mask(struct in_addr a)
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

	/* Manual and INFORM carry the address; Manual must have one. */
	if (out->method == ipconfig_method_manual_e ||
	    out->method == ipconfig_method_inform_e) {
		if (!cf_first_in_addr(CFDictionaryGetValue(d,
		    kSCPropNetIPv4Addresses), &out->addr) ||
		    out->addr.s_addr == INADDR_ANY) {
			if (out->method == ipconfig_method_manual_e)
				return (false);
		} else if (!cf_first_in_addr(CFDictionaryGetValue(d,
		    kSCPropNetIPv4SubnetMasks), &out->mask) ||
		    out->mask.s_addr == INADDR_ANY) {
			out->mask = classful_mask(out->addr);
		}
		(void)cf_to_in_addr(CFDictionaryGetValue(d,
		    kSCPropNetIPv4Router), &out->router);
	}
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
}

/* An enabled protocol entity of `service`, or NULL. Not retained. */
static CFDictionaryRef
service_entity(SCNetworkServiceRef service, CFStringRef type,
    SCNetworkProtocolRef *proto_out)
{
	SCNetworkProtocolRef proto;
	CFDictionaryRef config;

	*proto_out = NULL;
	proto = SCNetworkServiceCopyProtocol(service, type);
	if (proto == NULL)
		return (NULL);
	if (!SCNetworkProtocolGetEnabled(proto)) {
		CFRelease(proto);
		return (NULL);
	}
	config = SCNetworkProtocolGetConfiguration(proto);
	if (config == NULL) {
		CFRelease(proto);
		return (NULL);
	}
	*proto_out = proto;	/* keeps `config` alive */
	return (config);
}

/* Rebuild `t` from the current set in `prefs`. */
static void
prefs_read(SCPreferencesRef prefs, struct prefs_table *t)
{
	SCNetworkSetRef set;
	CFArrayRef services;
	CFIndex i, n;
	int j;

	(void)memset(t, 0, sizeof(*t));
	set = SCNetworkSetCopyCurrent(prefs);
	if (set == NULL)
		return;		/* no network preferences: all DHCP */
	services = SCNetworkSetCopyServices(set);
	CFRelease(set);
	if (services == NULL)
		return;

	n = CFArrayGetCount(services);
	for (i = 0; i < n; i++) {
		SCNetworkServiceRef service;
		SCNetworkInterfaceRef iface;
		SCNetworkProtocolRef p4, pdns;
		CFDictionaryRef ipv4, dns;
		CFStringRef bsd;
		struct prefs_entry e;
		bool dup = false;

		service = CFArrayGetValueAtIndex(services, i);
		iface = SCNetworkServiceGetInterface(service);
		bsd = (iface != NULL) ? SCNetworkInterfaceGetBSDName(iface)
		    : NULL;
		(void)memset(&e, 0, sizeof(e));
		if (bsd == NULL || !CFStringGetCString(bsd, e.ifname,
		    sizeof(e.ifname), kCFStringEncodingASCII))
			continue;

		for (j = 0; j < t->n; j++) {
			if (strcmp(t->e[j].ifname, e.ifname) == 0)
				dup = true;
		}
		if (dup) {
			xlog("%s: more than one service names it; using the "
			    "first", e.ifname);
			continue;
		}
		if (t->n == BOUND_MAX_IF) {
			xlog("%s: too many configured services; ignoring",
			    e.ifname);
			continue;
		}

		ipv4 = service_entity(service, kSCEntNetIPv4, &p4);
		dns = service_entity(service, kSCEntNetDNS, &pdns);
		if (ipv4 == NULL) {
			/* A service with IPv4 off: Apple runs no v4 service. */
			e.info.method = ipconfig_method_none_e;
		} else if (!ipconfig_method_info_from_dict(
		    (const struct __CFDictionary *)ipv4, &e.info)) {
			/*
			 * Unusable ConfigMethod (or Manual with no address).
			 * Leaving the interface alone is safer than DHCP'ing
			 * over what the administrator meant.
			 */
			xlog("%s: invalid IPv4 configuration; leaving it "
			    "alone", e.ifname);
			(void)memset(&e.info, 0, sizeof(e.info));
			e.info.method = ipconfig_method_none_e;
		}
		if (dns != NULL) {
			dns_from_dict(dns, &e.dns);
			e.have_dns = true;
		}
		if (p4 != NULL)
			CFRelease(p4);
		if (pdns != NULL)
			CFRelease(pdns);

		if (!ipconfig_method_is_served(e.info.method) &&
		    e.info.method != ipconfig_method_none_e)
			xlog("%s: ConfigMethod %s is not supported; leaving "
			    "it alone", e.ifname,
			    ipconfig_method_string(e.info.method));
		t->e[t->n++] = e;
	}
	CFRelease(services);
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
	return (memcmp(&x->info, &y->info, sizeof(x->info)) != 0);
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
	}
}

/*
 * SCPreferences commit from another process (a network pane, scutil
 * --prefs). Re-read, swap the table, and tell ipconfigd about each
 * interface whose method or DNS moved. The tables are static because they
 * are large for a queue thread's stack; the queue is serial.
 */
static void
prefs_changed(SCPreferencesRef prefs, SCPreferencesNotification type,
    void *info)
{
	static struct prefs_table next, prev;
	int i;

	(void)info;
	/* As Apple's PreferencesMonitor: a commit alone changes nothing. */
	if ((type & kSCPreferencesNotificationApply) == 0)
		return;
	SCPreferencesSynchronize(prefs);
	prefs_read(prefs, &next);

	pthread_mutex_lock(&g_prefs_lock);
	prev = g_table;
	g_table = next;
	pthread_mutex_unlock(&g_prefs_lock);

	log_table(&next);
	/* A commit can change the primary service's Proxies alone (#573). */
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
}

int
ipconfig_prefs_start(ipconfig_prefs_cb cb, ipconfig_prefs_cb dns_cb)
{
	SCPreferencesContext ctx;
	dispatch_queue_t q;
	static struct prefs_table t;

	g_cb = cb;
	g_dns_cb = dns_cb;
	g_prefs = SCPreferencesCreate(NULL, CFSTR("com.apple.IPConfiguration"),
	    CFSTR("preferences.plist"));
	if (g_prefs == NULL) {
		xlog("SCPreferencesCreate failed; every interface is DHCP");
		return (-1);
	}
	prefs_read(g_prefs, &t);
	pthread_mutex_lock(&g_prefs_lock);
	g_table = t;
	pthread_mutex_unlock(&g_prefs_lock);
	log_table(&t);

	(void)memset(&ctx, 0, sizeof(ctx));
	q = dispatch_queue_create("com.apple.IPConfiguration.prefs", NULL);
	if (q == NULL ||
	    !SCPreferencesSetCallback(g_prefs, prefs_changed, &ctx) ||
	    !SCPreferencesSetDispatchQueue(g_prefs, q)) {
		xlog("cannot watch SCPreferences; configuration is read "
		    "once at startup");
		return (-1);
	}
	return (0);
}

const struct __CFDictionary *
ipconfig_prefs_copy_proxies(const char *ifname)
{
	SCPreferencesRef prefs;
	SCNetworkSetRef set;
	CFArrayRef services;
	CFStringRef name;
	CFDictionaryRef copy = NULL;
	CFIndex i, n;

	name = CFStringCreateWithCString(NULL, ifname, kCFStringEncodingASCII);
	if (name == NULL)
		return (NULL);
	prefs = SCPreferencesCreate(NULL, CFSTR("ipconfigd-proxies"),
	    CFSTR("preferences.plist"));
	if (prefs == NULL) {
		CFRelease(name);
		return (NULL);
	}
	set = SCNetworkSetCopyCurrent(prefs);
	services = (set != NULL) ? SCNetworkSetCopyServices(set) : NULL;
	if (set != NULL)
		CFRelease(set);

	n = (services != NULL) ? CFArrayGetCount(services) : 0;
	for (i = 0; i < n; i++) {
		SCNetworkServiceRef service;
		SCNetworkInterfaceRef iface;
		SCNetworkProtocolRef proto;
		CFDictionaryRef config;
		CFStringRef bsd;

		service = CFArrayGetValueAtIndex(services, i);
		iface = SCNetworkServiceGetInterface(service);
		bsd = (iface != NULL) ? SCNetworkInterfaceGetBSDName(iface)
		    : NULL;
		if (bsd == NULL || !CFEqual(bsd, name))
			continue;
		/* The first service naming it, as prefs_read() takes. */
		config = service_entity(service, kSCEntNetProxies, &proto);
		if (config != NULL) {
			copy = CFDictionaryCreateCopy(NULL, config);
			CFRelease(proto);
		}
		break;
	}
	if (services != NULL)
		CFRelease(services);
	CFRelease(prefs);
	CFRelease(name);
	return ((const struct __CFDictionary *)copy);
}

bool
ipconfig_prefs_lookup(const char *ifname, struct ipconfig_method_info *out)
{
	const struct prefs_entry *e;
	bool hit = false;

	pthread_mutex_lock(&g_prefs_lock);
	e = table_find(&g_table, ifname);
	if (e != NULL) {
		*out = e->info;
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
