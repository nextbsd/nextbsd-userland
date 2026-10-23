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
 * them with, and watches for commits.
 *
 * The current set's services are what count. For each one with an
 * Interface DeviceName:
 *   - an enabled IPv4 entity      -> its ConfigMethod
 *   - no IPv4, or IPv4 disabled   -> NONE (leave the interface alone)
 * An interface no service names is not in the table; the caller treats
 * that as DHCP.
 */
#ifndef _IPCFG_IPCONFIG_PREFS_H_
#define _IPCFG_IPCONFIG_PREFS_H_

#include <stdbool.h>

#include "ipconfig_method.h"

/*
 * Invoked on a libdispatch queue, once per interface whose configured
 * method changed in a commit (including an interface that gained or
 * lost its service).
 */
typedef void (*ipconfig_prefs_cb)(const char *ifname);

/*
 * Load the preferences synchronously, then watch them for commits.
 * Returns 0 on success; on failure the table stays empty, so every
 * interface is DHCP'd as before.
 */
int	ipconfig_prefs_start(ipconfig_prefs_cb cb);

/*
 * `ifname`'s configured method. Returns false when no service in the
 * current set names the interface.
 */
bool	ipconfig_prefs_lookup(const char *ifname,
	    struct ipconfig_method_info *out);

#endif /* _IPCFG_IPCONFIG_PREFS_H_ */
