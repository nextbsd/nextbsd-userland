/*
 * ipconfig_prefs.h — read per-interface IPv4 methods from SCPreferences
 * (#58).
 *
 * On Darwin, configd's PreferencesMonitor mirrors the current set's
 * network services from preferences.plist into Setup:/Network/Service/
 * <id>/{Interface,IPv4,DNS}, and IPConfiguration configures each
 * interface from the IPv4 entity's ConfigMethod. Our configd has no
 * plugin host, so — as hostnamed's prefs_monitor.c does for the host
 * names — ipconfigd reads the same entities from SCPreferences itself,
 * through the SCNetworkSet / SCNetworkService API a network pane writes
 * them with, and watches for applies.
 *
 * The current set's services are what count. For each one with an
 * Interface DeviceName:
 *   - an enabled IPv4 entity      -> its ConfigMethod
 *   - no IPv4, or IPv4 disabled   -> NONE (leave the interface alone)
 *   - the service disabled (#568) -> NONE, unless an enabled service names
 *     the interface too: PreferencesMonitor drops a disabled service, so
 *     it never counts against an enabled one
 * Each entry keeps the service's ID (#568), which ipconfigd publishes its
 * state under. An interface no service names is not in the table; the
 * caller treats that as DHCP, and as Automatic for IPv6.
 *
 * IPv6 (#423), from the same service:
 *   - an enabled IPv6 entity        -> its ConfigMethod
 *   - no IPv6, or IPv6 disabled     -> off, as on Darwin
 *   - the service disabled          -> left alone
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

/*
 * Load the preferences synchronously, then watch them for applies.
 * `dns_cb` runs, on the same queue, for each interface whose DNS entity
 * changed in an apply; it is kept apart from `cb` so a DNS-only change
 * re-merges DNS without restarting the service (#425). Returns 0 on
 * success; on failure the table stays empty, so every interface is
 * DHCP'd as before.
 */
int	ipconfig_prefs_start(ipconfig_prefs_cb cb, ipconfig_prefs_cb dns_cb);

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
 * Returns false when no service in the current set names the interface;
 * the caller treats that as Automatic.
 */
bool	ipconfig_prefs_lookup_v6(const char *ifname,
	    struct ipconfig_v6_method_info *out, char *service_id, size_t len);

/*
 * A copy of the enabled Proxies entity of the service that names `ifname`
 * in the current set, or NULL when there is none. Read from a fresh
 * SCPreferences session, so it is safe on any thread. The result is a
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
