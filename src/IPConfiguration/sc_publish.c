/*
 * sc_publish.c — SCDynamicStore publish glue for ipconfigd.
 *
 * Builds Apple's State:/Network/Service/<serviceID>/IPv4 (+ optional /DNS)
 * CFDictionary and pushes it into configd via libSystemConfiguration.
 * The <serviceID> is the network service's ID in SCPreferences (#568), so
 * a service's Setup: and State: keys line up, or `ipconfig set`'s
 * <Method>-<if>. An interface no service names gets a deterministic
 * 8-4-4-4-12-hex string derived from its MAC, stable across daemon
 * restarts, so a watcher's regex match stays put. Either way the ID is
 * fixed once per service and cached (#409), so the keys can still be
 * removed after the interface — and its MAC — is gone.
 *
 * Single CF translation unit on purpose: everything else in the
 * daemon stays plain-C / plain-Apple-headers.
 */
#include "sc_publish.h"
#include "bound_state.h"	/* primary election (#41) */
#include "ipconfig_prefs.h"	/* the primary service's Proxies (#573) */
#include "dns_config.h"		/* struct ipconfig_dns (#425) */
#include "DHCPv6.h"			/* vendored: DHCPv6 packet (#576) */
#include "DHCPv6Options.h"		/* vendored: its options */

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCPrivate.h>	/* _SC_NOTIFY_NETWORK_CHANGE */
#include <CoreFoundation/CoreFoundation.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <ifaddrs.h>
#include <notify.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>		/* clock_gettime(CLOCK_MONOTONIC) for /DHCP LeaseStartTime */
#include <string.h>

struct sc_publish {
	SCDynamicStoreRef	store;
};

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[sc] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Tell notify(3) watchers (syslogd, hostnamed) the network changed, as
 * Darwin's IPMonitor does on a primary-service change (ip_plugin.c).
 */
static void
post_network_change(void)
{
	uint32_t status;

	status = notify_post(_SC_NOTIFY_NETWORK_CHANGE);
	if (status != NOTIFY_STATUS_OK)
		xlog("notify_post(" _SC_NOTIFY_NETWORK_CHANGE ") failed: "
		    "status=%u", status);
}

/* CFStringCreateWithCString wrapper — avoids -fconstant-cfstrings. */
static CFStringRef
mkstr(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

static int
get_mac(const char *ifname, uint8_t mac[6])
{
	struct ifaddrs *ifa, *p;
	int ok = -1;

	if (getifaddrs(&ifa) != 0)
		return (-1);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_dl *dl;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK)
			continue;
		if (strcmp(p->ifa_name, ifname) != 0)
			continue;
		dl = (const struct sockaddr_dl *)(const void *)p->ifa_addr;
		if (dl->sdl_alen != 6)
			break;
		(void)memcpy(mac, LLADDR(dl), 6);
		ok = 0;
		break;
	}
	freeifaddrs(ifa);
	return (ok);
}

#define	SVC_UUID_LEN	37	/* 8-4-4-4-12 + NUL */

/*
 * Format an Apple-shaped UUID string from the interface MAC, for an
 * interface no SCNetworkService names (#568); Darwin has no such case,
 * since every configured interface has a service there. Format is real
 * 8-4-4-4-12 lowercase hex so downstream observers that regex-match on
 * UUID keys work unchanged.
 *
 *   00000000-0000-0000-AABB-AABBCCDDEEFF
 *                      ^^^^ ^^^^^^^^^^^^
 *                      MAC[0..1] MAC[0..5]
 *
 * Returns get_mac()'s result; on failure `buf` holds the all-zero MAC.
 */
static int
format_service_uuid(const char *ifname, char buf[SVC_UUID_LEN])
{
	uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
	int rc;

	rc = get_mac(ifname, mac);
	(void)snprintf(buf, SVC_UUID_LEN,
	    "00000000-0000-0000-%02x%02x-%02x%02x%02x%02x%02x%02x",
	    mac[0], mac[1],
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return (rc);
}

/*
 * Per-interface service ID cache (#409). Apple's IPConfiguration keys a
 * service by its SCPreferences service ID, held for the life of the
 * service; re-deriving a MAC-based ID from the live MAC broke removal once
 * the interface departed — get_mac() failed and every key name came out as
 * 00000000-...-000000000000, so the real keys stayed in configd. Shared
 * by every sc_publish session (workers, ra_router, on_link_departed).
 */
#define	SVC_ID_MAX	(2 * BOUND_MAX_IF)

struct svc_id_entry {
	char	ifname[IF_NAMESIZE];
	char	id[SC_PUBLISH_SERVICE_ID_LEN];
};

static struct svc_id_entry	g_svc_id[SVC_ID_MAX];
static struct svc_id_entry	g_svc_id_v6[SVC_ID_MAX];	/* #423 */
static pthread_mutex_t	g_svc_id_lock = PTHREAD_MUTEX_INITIALIZER;

/* Store `id` for `ifname` in `table`; called with g_svc_id_lock held. */
static void
svc_id_store_locked(struct svc_id_entry *table, const char *ifname,
    const char *id)
{
	int i, free_slot = -1;

	for (i = 0; i < SVC_ID_MAX; i++) {
		if (table[i].ifname[0] == '\0') {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(table[i].ifname, ifname) == 0)
			break;
	}
	if (i == SVC_ID_MAX) {
		if (free_slot < 0)
			return;		/* full: callers fall back to the MAC */
		i = free_slot;
		(void)strlcpy(table[i].ifname, ifname, sizeof(table[i].ifname));
	}
	(void)strlcpy(table[i].id, id, sizeof(table[i].id));
}

static void
service_begin(struct svc_id_entry *table, const char *ifname,
    const char *service_id)
{
	char buf[SVC_UUID_LEN];

	if (service_id == NULL || service_id[0] == '\0') {
		/* No MAC: keep whatever an earlier service cached. */
		if (format_service_uuid(ifname, buf) != 0)
			return;
		service_id = buf;
	}
	(void)pthread_mutex_lock(&g_svc_id_lock);
	svc_id_store_locked(table, ifname, service_id);
	(void)pthread_mutex_unlock(&g_svc_id_lock);
}

void
sc_publish_service_begin(const char *ifname, const char *service_id)
{
	service_begin(g_svc_id, ifname, service_id);
}

void
sc_publish_service_begin_v6(const char *ifname, const char *service_id)
{
	service_begin(g_svc_id_v6, ifname, service_id);
}

/*
 * The service ID for `ifname`: the cached one, else derived from the MAC
 * now (and cached, if the MAC was readable). Result is heap-allocated;
 * caller releases with CFRelease.
 */
static CFStringRef
make_service_id(const char *ifname)
{
	char buf[SC_PUBLISH_SERVICE_ID_LEN];
	int i;

	(void)pthread_mutex_lock(&g_svc_id_lock);
	for (i = 0; i < SVC_ID_MAX; i++) {
		if (g_svc_id[i].ifname[0] != '\0' &&
		    strcmp(g_svc_id[i].ifname, ifname) == 0) {
			(void)strlcpy(buf, g_svc_id[i].id, sizeof(buf));
			break;
		}
	}
	if (i == SVC_ID_MAX && format_service_uuid(ifname, buf) == 0)
		svc_id_store_locked(g_svc_id, ifname, buf);
	(void)pthread_mutex_unlock(&g_svc_id_lock);
	return (mkstr(buf));
}

static CFStringRef
make_key(const char *ifname, const char *suffix)
{
	CFStringRef id = make_service_id(ifname);
	CFStringRef key;

	key = CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("State:/Network/Service/%@/%s"), id, suffix);
	CFRelease(id);
	return (key);
}

/*
 * State:/Network/Service/<id>/IPv6 under the IPv6 service's own ID (#423);
 * an `ipconfig set` IPv6 method has one of its own, as on Darwin. Falls
 * back to the interface's IPv4 service ID if the IPv6 one is not cached.
 */
static CFStringRef
make_v6_entity_key(const char *ifname, const char *entity)
{
	char buf[SC_PUBLISH_SERVICE_ID_LEN] = "";
	CFStringRef id, key;
	int i;

	(void)pthread_mutex_lock(&g_svc_id_lock);
	for (i = 0; i < SVC_ID_MAX; i++) {
		if (g_svc_id_v6[i].ifname[0] != '\0' &&
		    strcmp(g_svc_id_v6[i].ifname, ifname) == 0) {
			(void)strlcpy(buf, g_svc_id_v6[i].id, sizeof(buf));
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_svc_id_lock);
	id = (buf[0] != '\0') ? mkstr(buf) : make_service_id(ifname);
	key = CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("State:/Network/Service/%@/%s"), id, entity);
	CFRelease(id);
	return (key);
}

static CFStringRef
make_v6_key(const char *ifname)
{
	return (make_v6_entity_key(ifname, "IPv6"));
}

struct sc_publish *
sc_publish_open(const char *session_name)
{
	struct sc_publish *p;
	CFStringRef name;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return (NULL);

	name = mkstr(session_name);
	p->store = SCDynamicStoreCreate(NULL, name, NULL, NULL);
	CFRelease(name);
	if (p->store == NULL) {
		xlog("SCDynamicStoreCreate failed: %s",
		    SCErrorString(SCError()));
		free(p);
		return (NULL);
	}
	return (p);
}

static void
add_addr_string(CFMutableDictionaryRef dict, CFStringRef key,
    const struct in_addr *a)
{
	char buf[INET_ADDRSTRLEN];
	CFStringRef s;

	if (inet_ntop(AF_INET, a, buf, sizeof(buf)) == NULL)
		return;
	s = mkstr(buf);
	CFDictionarySetValue(dict, key, s);
	CFRelease(s);
}

static void
add_single_addr_array(CFMutableDictionaryRef dict, CFStringRef key,
    const struct in_addr *a)
{
	char buf[INET_ADDRSTRLEN];
	CFStringRef s;
	CFArrayRef arr;
	const void *vals[1];

	if (inet_ntop(AF_INET, a, buf, sizeof(buf)) == NULL)
		return;
	s = mkstr(buf);
	vals[0] = s;
	arr = CFArrayCreate(NULL, vals, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(dict, key, arr);
	CFRelease(arr);
	CFRelease(s);
}

int
sc_publish_ipv4(struct sc_publish *p, const char *ifname,
    const struct dhcp_lease *lease)
{
	CFMutableDictionaryRef dict;
	CFStringRef key, k_addresses, k_masks, k_router, k_iface, k_server;
	CFStringRef iface_str;
	Boolean ok;

	if (p == NULL || p->store == NULL)
		return (-1);

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (-1);

	k_addresses = mkstr("Addresses");
	k_masks = mkstr("SubnetMasks");
	k_router = mkstr("Router");
	k_iface = mkstr("InterfaceName");
	k_server = mkstr("ServerIdentifier");

	add_single_addr_array(dict, k_addresses, &lease->addr);
	add_single_addr_array(dict, k_masks, &lease->netmask);
	/* Absent for a Manual service without a router (#58), as on Apple. */
	if (lease->router.s_addr != INADDR_ANY)
		add_addr_string(dict, k_router, &lease->router);
	if (lease->server.s_addr != INADDR_ANY)
		add_addr_string(dict, k_server, &lease->server);

	iface_str = mkstr(ifname);
	CFDictionarySetValue(dict, k_iface, iface_str);
	CFRelease(iface_str);

	CFRelease(k_addresses);
	CFRelease(k_masks);
	CFRelease(k_router);
	CFRelease(k_iface);
	CFRelease(k_server);

	key = make_key(ifname, "IPv4");
	ok = SCDynamicStoreSetValue(p->store, key, dict);
	CFRelease(key);
	CFRelease(dict);

	if (!ok) {
		xlog("SCDynamicStoreSetValue(IPv4) failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}

	/*
	 * State:/Network/Global/IPv4 is NOT written here any more. This
	 * used to set PrimaryService/PrimaryInterface to whichever service
	 * had just bound — last-binder-wins (#41). Single-NIC CI never
	 * noticed, but on a laptop with em0 and wlan0 "primary" became a
	 * race, and both mDNSResponder (#62) and hostnamed consume that key,
	 * so a wrong answer propagates into Bonjour and hostname resolution.
	 *
	 * The election now lives in sc_publish_update_primary(), which asks
	 * bound_state for the highest-ranked *currently bound* interface. The
	 * caller runs it after any bind or teardown, once bound_state is
	 * settled — which is also why it cannot happen inline here: at this
	 * point the caller may not have recorded the binding yet.
	 *
	 * Nor is .../DNS written here: it is the service's merged DNS, which
	 * also depends on Setup and on IPv6 (#425). See sc_publish_dns().
	 */
	return (0);
}

/*
 * A CFArray of the strings in a fixed-size list. A string that is not
 * valid UTF-8 is skipped rather than handed to CFArrayAppendValue as NULL.
 */
static void
dict_set_string_list(CFMutableDictionaryRef dict, const char *key,
    const char *list, size_t stride, unsigned count)
{
	CFMutableArrayRef arr;
	CFStringRef k;
	unsigned i;

	if (count == 0)
		return;
	arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	k = mkstr(key);
	if (arr != NULL && k != NULL) {
		for (i = 0; i < count; i++) {
			CFStringRef s = mkstr(list + i * stride);

			if (s == NULL)
				continue;
			CFArrayAppendValue(arr, s);
			CFRelease(s);
		}
		if (CFArrayGetCount(arr) > 0)
			CFDictionarySetValue(dict, k, arr);
	}
	if (k != NULL)
		CFRelease(k);
	if (arr != NULL)
		CFRelease(arr);
}

static void
dict_set_string(CFMutableDictionaryRef dict, const char *key, const char *val)
{
	CFStringRef k, v;

	if (val[0] == '\0')
		return;
	k = mkstr(key);
	v = mkstr(val);
	if (k != NULL && v != NULL)
		CFDictionarySetValue(dict, k, v);
	if (k != NULL)
		CFRelease(k);
	if (v != NULL)
		CFRelease(v);
}

static void
dict_set_int32(CFMutableDictionaryRef dict, const char *key, int32_t val)
{
	CFStringRef k = mkstr(key);
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &val);

	if (k != NULL && n != NULL)
		CFDictionarySetValue(dict, k, n);
	if (k != NULL)
		CFRelease(k);
	if (n != NULL)
		CFRelease(n);
}

/* `dns` in Apple's kSCEntNetDNS shape; `ifname` NULL leaves InterfaceName out. */
static CFMutableDictionaryRef
dns_dict_create(const struct ipconfig_dns *dns, const char *ifname)
{
	CFMutableDictionaryRef dict;

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (NULL);
	dict_set_string_list(dict, "ServerAddresses", dns->servers[0],
	    IPCFG_DNS_ADDR_LEN, dns->server_count);
	dict_set_string_list(dict, "SearchDomains", dns->search[0],
	    IPCFG_DNS_NAME_LEN, dns->search_count);
	dict_set_string_list(dict, "SortList", dns->sortlist[0],
	    IPCFG_DNS_SORT_LEN, dns->sort_count);
	dict_set_string(dict, "DomainName", dns->domain);
	dict_set_string(dict, "Options", dns->options);
	if (dns->has_search_order)
		dict_set_int32(dict, "SearchOrder", dns->search_order);
	if (dns->has_port)
		dict_set_int32(dict, "ServerPort", dns->port);
	if (dns->has_timeout)
		dict_set_int32(dict, "ServerTimeout", dns->timeout);
	if (dns->has_service_id)
		dict_set_int32(dict, "ServiceIdentifier", dns->service_id);
	if (ifname != NULL)
		dict_set_string(dict, "InterfaceName", ifname);
	return (dict);
}

int
sc_publish_dns(struct sc_publish *p, const char *ifname,
    const struct ipconfig_dns *dns)
{
	CFMutableDictionaryRef dict;
	CFStringRef key;
	Boolean ok;

	if (p == NULL || p->store == NULL)
		return (-1);
	dict = dns_dict_create(dns, ifname);
	if (dict == NULL)
		return (-1);
	key = make_key(ifname, "DNS");
	ok = SCDynamicStoreSetValue(p->store, key, dict);
	CFRelease(key);
	CFRelease(dict);
	if (!ok) {
		xlog("SCDynamicStoreSetValue(DNS) failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}
	return (0);
}

int
sc_publish_remove_dns(struct sc_publish *p, const char *ifname)
{
	CFStringRef key;

	if (p == NULL || p->store == NULL)
		return (-1);
	key = make_key(ifname, "DNS");
	(void)SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);
	return (0);
}

int
sc_publish_dns_global(struct sc_publish *p, const struct ipconfig_dns *dns)
{
	CFMutableDictionaryRef dict;
	CFStringRef key;
	Boolean ok;

	if (p == NULL || p->store == NULL)
		return (-1);
	dict = dns_dict_create(dns, NULL);
	if (dict == NULL)
		return (-1);
	key = mkstr("State:/Network/Global/DNS");
	ok = SCDynamicStoreSetValue(p->store, key, dict);
	CFRelease(key);
	CFRelease(dict);
	if (!ok) {
		xlog("SCDynamicStoreSetValue(Global/DNS) failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}
	return (0);
}

int
sc_publish_remove_dns_global(struct sc_publish *p)
{
	CFStringRef key;

	if (p == NULL || p->store == NULL)
		return (-1);
	key = mkstr("State:/Network/Global/DNS");
	(void)SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);
	return (0);
}

int
sc_publish_dhcp(struct sc_publish *p, const char *ifname,
    const struct dhcp_lease *lease)
{
	CFMutableDictionaryRef dict;
	CFStringRef key, k_lease, k_iface, k_opt12;
	CFStringRef iface_str, host_str;
	CFNumberRef lease_num;
	struct timespec ts;
	int32_t lease_start;
	Boolean ok;

	if (p == NULL || p->store == NULL || lease == NULL)
		return (-1);

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (-1);

	/* InterfaceName + LeaseStartTime are always set so the dict is
	 * never empty — observers can watch for the /DHCP key to appear
	 * even on a lease that didn't carry Option_12. LeaseStartTime is
	 * CLOCK_MONOTONIC seconds at bind, matching what bound_state.c
	 * already tracks; SCPrefs / IPMonitor consumers don't depend on
	 * absolute wall-clock here. */
	k_iface = mkstr("InterfaceName");
	k_lease = mkstr("LeaseStartTime");
	iface_str = mkstr(ifname);
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	lease_start = (int32_t)ts.tv_sec;
	lease_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &lease_start);

	if (k_iface != NULL && k_lease != NULL && iface_str != NULL &&
	    lease_num != NULL) {
		CFDictionarySetValue(dict, k_iface, iface_str);
		CFDictionarySetValue(dict, k_lease, lease_num);
	}

	/* Option_12 (host name) — only added when the lease carried it.
	 * Stored as CFString rather than CFData since the option is
	 * printable per RFC 2132 §3.14 and hostnamed iter 3 reads it as
	 * a string. Issue #88. */
	k_opt12 = mkstr("Option_12");
	host_str = NULL;
	if (lease->host_name_len > 0 && k_opt12 != NULL) {
		host_str = mkstr(lease->host_name);
		if (host_str != NULL)
			CFDictionarySetValue(dict, k_opt12, host_str);
	}

	key = make_key(ifname, "DHCP");
	ok = SCDynamicStoreSetValue(p->store, key, dict);
	CFRelease(key);
	CFRelease(dict);
	if (host_str != NULL) CFRelease(host_str);
	if (k_opt12 != NULL) CFRelease(k_opt12);
	if (lease_num != NULL) CFRelease(lease_num);
	if (iface_str != NULL) CFRelease(iface_str);
	if (k_lease != NULL) CFRelease(k_lease);
	if (k_iface != NULL) CFRelease(k_iface);

	if (!ok) {
		xlog("SCDynamicStoreSetValue(DHCP) failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}
	return (0);
}

int
sc_publish_ipv6(struct sc_publish *p, const char *ifname,
    const struct in6_addr *addr, uint8_t prefix_len,
    const struct in6_addr *router_lladdr)
{
	return (sc_publish_ipv6_addrs(p, ifname, addr, &prefix_len, 1,
	    router_lladdr));
}

int
sc_publish_ipv6_addrs(struct sc_publish *p, const char *ifname,
    const struct in6_addr *addrs, const uint8_t *prefix_lens, int count,
    const struct in6_addr *router_lladdr)
{
	CFMutableDictionaryRef dict;
	CFMutableArrayRef addr_arr, plen_arr;
	CFStringRef key, k_addresses, k_prefixlen, k_router;
	CFStringRef k_iface, k_flags;
	CFStringRef iface_str, router_str;
	CFNumberRef flags_num;
	char rbuf[INET6_ADDRSTRLEN + IFNAMSIZ + 2];
	int flags_i = 0, i;
	Boolean ok;

	if (p == NULL || p->store == NULL || count <= 0)
		return (-1);

	if (router_lladdr != NULL) {
		char rbase[INET6_ADDRSTRLEN];

		if (inet_ntop(AF_INET6, router_lladdr, rbase,
		    sizeof(rbase)) == NULL)
			return (-1);
		(void)snprintf(rbuf, sizeof(rbuf), "%s%%%s", rbase, ifname);
	}

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (-1);

	k_addresses = mkstr("Addresses");
	k_prefixlen = mkstr("PrefixLength");
	k_router = mkstr("Router");
	k_iface = mkstr("InterfaceName");
	k_flags = mkstr("Flags");

	addr_arr = CFArrayCreateMutable(NULL, count, &kCFTypeArrayCallBacks);
	plen_arr = CFArrayCreateMutable(NULL, count, &kCFTypeArrayCallBacks);
	for (i = 0; i < count; i++) {
		CFStringRef addr_str;
		CFNumberRef plen_num;
		char abuf[INET6_ADDRSTRLEN];
		int plen_i = prefix_lens[i];

		if (inet_ntop(AF_INET6, &addrs[i], abuf, sizeof(abuf)) == NULL)
			continue;
		addr_str = mkstr(abuf);
		CFArrayAppendValue(addr_arr, addr_str);
		CFRelease(addr_str);
		plen_num = CFNumberCreate(NULL, kCFNumberIntType, &plen_i);
		CFArrayAppendValue(plen_arr, plen_num);
		CFRelease(plen_num);
	}
	CFDictionarySetValue(dict, k_addresses, addr_arr);
	CFDictionarySetValue(dict, k_prefixlen, plen_arr);
	CFRelease(addr_arr);
	CFRelease(plen_arr);

	/* No default router (its lifetime expired): omit Router. */
	if (router_lladdr != NULL) {
		router_str = mkstr(rbuf);
		CFDictionarySetValue(dict, k_router, router_str);
		CFRelease(router_str);
	}

	iface_str = mkstr(ifname);
	CFDictionarySetValue(dict, k_iface, iface_str);
	CFRelease(iface_str);

	flags_num = CFNumberCreate(NULL, kCFNumberIntType, &flags_i);
	CFDictionarySetValue(dict, k_flags, flags_num);
	CFRelease(flags_num);

	CFRelease(k_addresses);
	CFRelease(k_prefixlen);
	CFRelease(k_router);
	CFRelease(k_iface);
	CFRelease(k_flags);

	key = make_v6_key(ifname);
	ok = SCDynamicStoreSetValue(p->store, key, dict);
	CFRelease(key);
	CFRelease(dict);

	if (!ok) {
		xlog("SCDynamicStoreSetValue(IPv6) failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}
	return (0);
}

/* DHCPv6InfoDictionaryCreate(), for the options Apple's client accepts. */
static bool
dhcpv6_option_is_ok(int option)
{
	switch (option) {
	case kDHCPv6OPTION_CLIENTID:
	case kDHCPv6OPTION_SERVERID:
	case kDHCPv6OPTION_ORO:
	case kDHCPv6OPTION_ELAPSED_TIME:
	case kDHCPv6OPTION_UNICAST:
	case kDHCPv6OPTION_RAPID_COMMIT:
	case kDHCPv6OPTION_IA_NA:
	case kDHCPv6OPTION_IAADDR:
	case kDHCPv6OPTION_STATUS_CODE:
	case kDHCPv6OPTION_IA_TA:
	case kDHCPv6OPTION_PREFERENCE:
	case kDHCPv6OPTION_RELAY_MSG:
	case kDHCPv6OPTION_AUTH:
	case kDHCPv6OPTION_USER_CLASS:
	case kDHCPv6OPTION_VENDOR_CLASS:
	case kDHCPv6OPTION_VENDOR_OPTS:
	case kDHCPv6OPTION_INTERFACE_ID:
	case kDHCPv6OPTION_RECONF_MSG:
	case kDHCPv6OPTION_RECONF_ACCEPT:
	case kDHCPv6OPTION_DNS_SERVERS:
	case kDHCPv6OPTION_DOMAIN_LIST:
		return (true);
	default:
		return (false);
	}
}

int
sc_publish_dhcpv6(struct sc_publish *p, const char *ifname, const void *pkt,
    size_t pkt_len)
{
	DHCPv6OptionErrorString err;
	DHCPv6OptionListRef options;
	CFMutableDictionaryRef dict;
	CFStringRef key;
	int count, i;
	Boolean ok = FALSE;

	if (p == NULL || p->store == NULL)
		return (-1);
	options = DHCPv6OptionListCreateWithPacket(
	    (DHCPv6PacketRef)(uintptr_t)pkt, (int)pkt_len, &err);
	if (options == NULL)
		return (-1);
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	count = DHCPv6OptionListGetCount(options);
	for (i = 0; i < count; i++) {
		DHCPv6OptionRef option;
		CFMutableArrayRef array;
		CFDataRef data;
		CFStringRef opt_key;
		int code;

		option = DHCPv6OptionListGetOptionAtIndex(options, i);
		code = DHCPv6OptionGetCode(option);
		if (!dhcpv6_option_is_ok(code))
			continue;
		data = CFDataCreate(NULL, DHCPv6OptionGetData(option),
		    DHCPv6OptionGetLength(option));
		opt_key = CFStringCreateWithFormat(NULL, NULL,
		    CFSTR("Option_%d"), code);
		array = (CFMutableArrayRef)(uintptr_t)
		    CFDictionaryGetValue(dict, opt_key);
		if (array == NULL) {
			/* allocate an array to hold the value(s) */
			array = CFArrayCreateMutable(NULL, 0,
			    &kCFTypeArrayCallBacks);
			CFDictionarySetValue(dict, opt_key, array);
			CFRelease(array);
		}
		CFArrayAppendValue(array, data);
		CFRelease(opt_key);
		CFRelease(data);
	}
	DHCPv6OptionListRelease(&options);
	key = make_v6_entity_key(ifname, "DHCPv6");
	if (CFDictionaryGetCount(dict) > 0)
		ok = SCDynamicStoreSetValue(p->store, key, dict);
	else
		ok = SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);
	CFRelease(dict);
	return (ok ? 0 : -1);
}

int
sc_publish_remove_dhcpv6(struct sc_publish *p, const char *ifname)
{
	CFStringRef key;
	Boolean ok;

	if (p == NULL || p->store == NULL)
		return (-1);
	key = make_v6_entity_key(ifname, "DHCPv6");
	ok = SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);
	return (ok ? 0 : -1);
}

/*
 * Bring State:/Network/Global/Proxies in line with the Proxies entity of
 * the service on `primary` (NULL: no primary), as ip_plugin.c does (#573).
 * Absent when there is no primary or its service has no Proxies entity,
 * never a placeholder: SCDynamicStoreCopyProxies answers an empty
 * dictionary for a missing key. Written only on a change, which is also
 * what posts network_change.proxy; Apple's IPMonitor does the same.
 * It also layers on scoped and supplemental proxies, which NextBSD has no
 * source for yet.
 */
static void
update_proxies(SCDynamicStoreRef store, const char *primary)
{
	CFStringRef key;
	CFDictionaryRef want;
	CFPropertyListRef have;
	Boolean changed = FALSE;

	key = mkstr("State:/Network/Global/Proxies");
	if (key == NULL)
		return;
	want = (primary != NULL) ?
	    (CFDictionaryRef)ipconfig_prefs_copy_proxies(primary) : NULL;
	have = SCDynamicStoreCopyValue(store, key);
	if (want != NULL) {
		if (have == NULL || !CFEqual(have, want)) {
			if (SCDynamicStoreSetValue(store, key, want)) {
				xlog("published proxies from %s", primary);
				changed = TRUE;
			} else {
				xlog("SCDynamicStoreSetValue("
				    "State:/Network/Global/Proxies) failed: %s",
				    SCErrorString(SCError()));
			}
		}
	} else if (have != NULL) {
		if (SCDynamicStoreRemoveValue(store, key)) {
			xlog("cleared State:/Network/Global/Proxies");
			changed = TRUE;
		}
	}
	if (have != NULL)
		CFRelease(have);
	if (want != NULL)
		CFRelease(want);
	CFRelease(key);
	if (changed) {
		uint32_t status;

		status = notify_post(_SC_NOTIFY_NETWORK_CHANGE_PROXY);
		if (status != NOTIFY_STATUS_OK)
			xlog("notify_post(" _SC_NOTIFY_NETWORK_CHANGE_PROXY
			    ") failed: status=%u", status);
	}
}

void
sc_publish_refresh_proxies(void)
{
	SCDynamicStoreRef store;
	char primary[IF_NAMESIZE];

	store = SCDynamicStoreCreate(NULL, CFSTR("ipconfigd-proxies"), NULL,
	    NULL);
	if (store == NULL)
		return;
	update_proxies(store, bound_state_primary(primary, sizeof(primary)) ?
	    primary : NULL);
	CFRelease(store);
}

/*
 * Elect the primary service and write State:/Network/Global/IPv4 (#41).
 *
 * The primary is the highest-ranked *currently bound* interface — wired
 * beats wireless (bound_state_primary()) — not, as before, whichever one
 * happened to bind most recently. Run this after any bind or teardown,
 * once bound_state reflects reality.
 *
 * When nothing is bound the key is removed rather than left stale: a
 * consumer that reads PrimaryInterface must not be told em0 is primary
 * when em0 no longer has a lease. mDNSResponder (#62) and hostnamed both
 * read this key. Either way the election ends with a network_change
 * post (#339).
 */
int
sc_publish_update_primary(struct sc_publish *p)
{
	CFMutableDictionaryRef gdict;
	CFStringRef gkey, gk_svc, gk_iface, svc_id, iface_str;
	char primary[IF_NAMESIZE];
	int rc = -1;

	if (p == NULL || p->store == NULL)
		return (-1);

	gkey = mkstr("State:/Network/Global/IPv4");
	if (gkey == NULL)
		return (-1);

	if (!bound_state_primary(primary, sizeof(primary))) {
		if (!SCDynamicStoreRemoveValue(p->store, gkey)) {
			/* Not an error when the key was never set. */
			xlog("no bound interface; State:/Network/Global/IPv4 "
			    "already absent");
		} else {
			xlog("no bound interface; cleared "
			    "State:/Network/Global/IPv4");
		}
		CFRelease(gkey);
		update_proxies(p->store, NULL);
		post_network_change();
		return (0);
	}

	gdict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	gk_svc = mkstr("PrimaryService");
	gk_iface = mkstr("PrimaryInterface");
	svc_id = make_service_id(primary);
	iface_str = mkstr(primary);

	if (gdict != NULL && gk_svc != NULL && gk_iface != NULL &&
	    svc_id != NULL && iface_str != NULL) {
		CFDictionarySetValue(gdict, gk_svc, svc_id);
		CFDictionarySetValue(gdict, gk_iface, iface_str);
		if (SCDynamicStoreSetValue(p->store, gkey, gdict)) {
			xlog("primary interface: %s (rank %d, %d bound)",
			    primary, iface_rank(primary), bound_state_count());
			rc = 0;
		} else {
			xlog("SCDynamicStoreSetValue("
			    "State:/Network/Global/IPv4) failed: %s",
			    SCErrorString(SCError()));
		}
	}

	if (iface_str != NULL) CFRelease(iface_str);
	if (svc_id != NULL) CFRelease(svc_id);
	if (gk_iface != NULL) CFRelease(gk_iface);
	if (gk_svc != NULL) CFRelease(gk_svc);
	if (gdict != NULL) CFRelease(gdict);
	CFRelease(gkey);
	if (rc == 0) {
		update_proxies(p->store, primary);
		post_network_change();
	}
	return (rc);
}

int
sc_publish_remove(struct sc_publish *p, const char *ifname)
{
	CFStringRef key;
	Boolean ok = TRUE;

	if (p == NULL || p->store == NULL)
		return (-1);

	key = make_key(ifname, "IPv4");
	if (!SCDynamicStoreRemoveValue(p->store, key))
		ok = FALSE;
	CFRelease(key);

	key = make_key(ifname, "DNS");
	(void)SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);

	key = make_key(ifname, "DHCP");
	(void)SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);

	return (ok ? 0 : -1);
}

int
sc_publish_remove_ipv6(struct sc_publish *p, const char *ifname)
{
	CFStringRef key;
	Boolean ok;

	if (p == NULL || p->store == NULL)
		return (-1);
	key = make_v6_key(ifname);
	ok = SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);
	return (ok ? 0 : -1);
}

void
sc_publish_close(struct sc_publish *p)
{
	if (p == NULL)
		return;
	if (p->store != NULL)
		CFRelease(p->store);
	free(p);
}

/*
 * Apple's unblock_startup(): the key's value is the key itself. A
 * plain SetValue, not a session-temporary one, so it stays in the store
 * after this session closes.
 */
int
sc_publish_startup_ready(void)
{
	SCDynamicStoreRef store;
	CFStringRef key = CFSTR("Plugin:IPConfiguration");
	Boolean ok;

	store = SCDynamicStoreCreate(NULL, CFSTR("ipconfigd-startup"),
	    NULL, NULL);
	if (store == NULL)
		return (-1);
	ok = SCDynamicStoreSetValue(store, key, key);
	CFRelease(store);
	return (ok ? 0 : -1);
}
