/*
 * ipconfigd.h — the daemon-side hooks the MIG routines call.
 *
 * ipconfigd.c owns the per-interface service workers; ipconfigd_mig.c
 * (the `ipconfig set` / `getsummary` server bodies, #59) runs on the
 * Mach service thread and drives them only through these calls. They
 * are thread-safe and never block on a worker: stopping one sets its
 * stop flag and returns, exactly as a link-down does.
 *
 * Apple's model is a service per interface with a config method
 * (DHCP, MANUAL, INFORM, BOOTP, NONE) — see ipconfig_method.h (#58).
 * `set` selects one through ipconfigd_set_method(), Apple's set_if().
 */
#ifndef _IPCFG_IPCONFIGD_H_
#define _IPCFG_IPCONFIGD_H_

#include <stdbool.h>

#include "ipconfig_method.h"
#include "ipconfig_mig_types.h"

/*
 * `ipconfig set <if> <method>`: make `info` the interface's method until
 * ipconfigd exits or the next `set`, taking precedence over
 * SCPreferences; like Apple's, it is not persisted. Any running service
 * on the interface is unwound (deconfiguring what it installed) and the
 * new one started once that is done — at once if nothing was running and
 * the link is up; a link that is down is brought administratively up and
 * the service starts on link-active, as for any interface. NONE unwinds
 * IPv4 and then leaves it alone, so link events no longer touch it; IPv6 is
 * unaffected (as Darwin's set_if(), #423).
 *
 * Returns interface_does_not_exist, invalid_operation (lo*),
 * invalid_parameter (Manual with no address), operation_not_supported (a
 * method ipconfigd does not serve: INFORM, BOOTP, LinkLocal, Failover),
 * resource_unavailable, or success.
 */
ipconfig_status_t	ipconfigd_set_method(const char *ifname,
			    const struct ipconfig_method_info *info);

/*
 * `ipconfig set <if> {AUTOMATIC-V6,MANUAL-V6,NONE-V6}` (#423): make `info`
 * the interface's IPv6 method, as ipconfigd_set_method() does for IPv4 and
 * leaving IPv4 untouched. Returns interface_does_not_exist,
 * invalid_operation (lo*), operation_not_supported (6TO4),
 * resource_unavailable, or success.
 */
struct ipconfig_v6_method_info;
ipconfig_status_t	ipconfigd_set_v6_method(const char *ifname,
			    const struct ipconfig_v6_method_info *info);

/*
 * The IPv6 method `ifname` should run and the service ID it publishes under
 * (SC_PUBLISH_SERVICE_ID_LEN bytes; empty for an interface no service names).
 */
void			ipconfigd_v6_lookup(const char *ifname,
			    struct ipconfig_v6_method_info *out,
			    char *service_id);

/*
 * The method of the service currently running on `ifname` — acquiring or
 * bound, not unwinding. Returns false when none is.
 */
bool			ipconfigd_get_method(const char *ifname,
			    struct ipconfig_method_info *out);

/* Is an `ipconfig set` method in force on `ifname`? (It hides Setup DNS.) */
bool			ipconfigd_set_method_active(const char *ifname);

#endif /* _IPCFG_IPCONFIGD_H_ */
