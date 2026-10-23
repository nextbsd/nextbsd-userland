/*
 * ipconfig_prefs.c — per-interface IPv4 methods from SCPreferences (#58).
 *
 * See ipconfig_prefs.h for the model. This is the only CF translation
 * unit on the configuration side: ipconfigd.c sees plain-C
 * struct ipconfig_method_info.
 */
#include "ipconfig_prefs.h"
#include "bound_state.h"	/* BOUND_MAX_IF */

#include <SystemConfiguration/SCNetworkConfiguration.h>
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>
#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

struct prefs_entry {
	char				ifname[IFNAMSIZ];
	struct ipconfig_method_info	info;
};

struct prefs_table {
	struct prefs_entry	e[BOUND_MAX_IF];
	int			n;
};

static pthread_mutex_t		g_prefs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct prefs_table	g_table;	/* guarded by g_prefs_lock */
static ipconfig_prefs_cb	g_cb;
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
    const struct __CFDictionary *dns, struct ipconfig_method_info *out)
{
	CFDictionaryRef d = (CFDictionaryRef)ipv4;
	CFTypeRef method, v;
	CFIndex i, n;
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

	if (dns != NULL &&
	    CFGetTypeID((CFDictionaryRef)dns) == CFDictionaryGetTypeID()) {
		v = CFDictionaryGetValue((CFDictionaryRef)dns,
		    kIPCfgPropDNSServerAddresses);
		if (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID()) {
			n = CFArrayGetCount(v);
			for (i = 0; i < n &&
			    out->dns_count < DHCP_LEASE_MAX_DNS; i++) {
				/* IPv6 resolvers are skipped, not fatal. */
				if (cf_to_in_addr(CFArrayGetValueAtIndex(v, i),
				    &out->dns[out->dns_count]))
					out->dns_count++;
			}
		}
	}
	return (true);
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
		    (const struct __CFDictionary *)ipv4,
		    (const struct __CFDictionary *)dns, &e.info)) {
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

/* Did `ifname`'s configuration differ between `a` and `b`? */
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
 * interface whose method moved.
 */
static void
prefs_changed(SCPreferencesRef prefs, SCPreferencesNotification type,
    void *info)
{
	struct prefs_table next, prev;
	int i;

	(void)info;
	/*
	 * Apple's PreferencesMonitor acts on Apply; our SCPreferences only
	 * delivers Commit, so take either.
	 */
	if ((type & (kSCPreferencesNotificationCommit |
	    kSCPreferencesNotificationApply)) == 0)
		return;
	SCPreferencesSynchronize(prefs);
	prefs_read(prefs, &next);

	pthread_mutex_lock(&g_prefs_lock);
	prev = g_table;
	g_table = next;
	pthread_mutex_unlock(&g_prefs_lock);

	log_table(&next);
	if (g_cb == NULL)
		return;
	for (i = 0; i < prev.n; i++) {
		if (table_changed(&prev, &next, prev.e[i].ifname))
			g_cb(prev.e[i].ifname);
	}
	for (i = 0; i < next.n; i++) {
		if (table_find(&prev, next.e[i].ifname) == NULL)
			g_cb(next.e[i].ifname);
	}
}

int
ipconfig_prefs_start(ipconfig_prefs_cb cb)
{
	SCPreferencesContext ctx;
	dispatch_queue_t q;
	struct prefs_table t;

	g_cb = cb;
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
