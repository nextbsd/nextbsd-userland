/*
 * ipconfig_prefs.h — per-interface IPv4 and IPv6 methods from Setup: (#570,
 * #58, #423).
 *
 * PreferencesMonitor flattens the current set's services into
 * Setup:/Network/Service/<id>/{Interface,IPv4,IPv6,DNS,Proxies}
 * on every Apply, and ipconfigd configures each interface from those keys,
 * as Darwin's IPConfiguration does. For each one with an Interface
 * DeviceName:
 *   - an IPv4 entity      -> its ConfigMethod
 *   - no IPv4 entity      -> NONE (leave the interface alone)
 * A service or protocol that is __INACTIVE__ never reaches Setup:, so a
 * disabled service's interface simply has no service. Each entry keeps the
 * service's ID, which ipconfigd publishes its state under.
 * An interface no service names is not in the table, and ipconfigd leaves
 * it alone (#570): it is neither brought up nor configured.
 *
 * IPv6 (#423), from the same service:
 *   - an IPv6 entity      -> its ConfigMethod
 *   - no IPv6 entity      -> off, as on Darwin
 *
 * The primary election (#570) orders services by PrimaryRank and then by
 * Setup:/Network/Global/IPv4's ServiceOrder.
 */
#ifndef _IPCFG_IPCONFIG_PREFS_H_
#define _IPCFG_IPCONFIG_PREFS_H_

#include <stdbool.h>

#include <stddef.h>

#include "ipconfig_method.h"

/*
 * Invoked on a libdispatch queue, once per interface whose configured
 * method changed in an apply (including an interface that gained or
 * lost its service).
 */
typedef void (*ipconfig_prefs_cb)(const char *ifname);

typedef void (*ipconfig_prefs_event_cb)(void);

/*
 * Read Setup: synchronously, calling `cb` for every service found, then
 * watch it for changes. `dns_cb` runs, on the same queue, for each
 * interface whose DNS entity changed; it is kept apart from `cb` so a
 * DNS-only change re-merges DNS without restarting the service (#425).
 * `loaded_cb` runs once, when PreferencesMonitor has written Setup: for the
 * first time, and `order_cb` whenever ServiceOrder or a PrimaryRank moved.
 * Returns 0 on success; on failure the table stays empty and no interface
 * is configured from preferences.
 */
int	ipconfig_prefs_start(ipconfig_prefs_cb cb, ipconfig_prefs_cb dns_cb,
	    ipconfig_prefs_event_cb loaded_cb,
	    ipconfig_prefs_event_cb order_cb);

/* Has PreferencesMonitor written Setup: yet? */
bool	ipconfig_prefs_loaded(void);

#define	IPCFG_ORDER_UNLISTED		1000000
#define	IPCFG_PRIMARY_RANK_FIRST	(-1)
#define	IPCFG_PRIMARY_RANK_DEFAULT	0
#define	IPCFG_PRIMARY_RANK_LAST		1
#define	IPCFG_PRIMARY_RANK_NEVER	2

/*
 * `ifname`'s standing in the primary election: its service's PrimaryRank and
 * its place in ServiceOrder. An interface with no service, such as one
 * `ipconfig set` configured, has the default rank and comes last.
 */
void	ipconfig_prefs_primary_rank(const char *ifname, int *rank_out,
	    int *order_out);

/*
 * `ifname`'s configured method, and in `service_id` (`len` bytes, may be
 * NULL) the ID of the service that configures it. Returns false when no
 * service in the current set names the interface. The service ID is empty
 * if it is too long to keep, and for a disabled service.
 */
bool	ipconfig_prefs_lookup(const char *ifname,
	    struct ipconfig_method_info *out, char *service_id, size_t len);

/*
 * `ifname`'s configured IPv6 method (#423), and its service's ID as above.
 * Returns false when no service in the current set names the interface.
 */
bool	ipconfig_prefs_lookup_v6(const char *ifname,
	    struct ipconfig_v6_method_info *out, char *service_id, size_t len);

/*
 * A copy of the enabled Proxies entity of the service that names `ifname`,
 * from Setup:, or NULL when there is none. Read from a fresh session, so it
 * is safe on any thread. The result is a
 * CFDictionaryRef the caller releases, spelled as its underlying type so
 * plain-C callers need not pull CoreFoundation.
 */
struct __CFDictionary;
const struct __CFDictionary *ipconfig_prefs_copy_proxies(const char *ifname);

/*
 * `ifname`'s service's enabled DNS entity (Darwin's Setup:/.../DNS).
 * Returns false when it has none.
 */
struct ipconfig_dns;
bool	ipconfig_prefs_lookup_dns(const char *ifname,
	    struct ipconfig_dns *out);

#endif /* _IPCFG_IPCONFIG_PREFS_H_ */
