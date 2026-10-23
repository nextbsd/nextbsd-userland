/*
 * ipconfigd.h — the daemon-side hooks the MIG routines call.
 *
 * ipconfigd.c owns the per-interface DHCP workers; ipconfigd_mig.c
 * (the `ipconfig set` / `getsummary` server bodies, #59) runs on the
 * Mach service thread and drives them only through these calls. They
 * are thread-safe and never block on a worker: stopping one sets its
 * stop flag and returns, exactly as a link-down does.
 *
 * Apple's model is a service per interface with a config method
 * (DHCP, MANUAL, INFORM, BOOTP, NONE). ipconfigd has the DHCP path
 * only, so `set` can select DHCP or NONE today; the static methods
 * need the MANUAL service type (#58).
 */
#ifndef _IPCFG_IPCONFIGD_H_
#define _IPCFG_IPCONFIGD_H_

#include <stdbool.h>

#include "ipconfig_mig_types.h"

/*
 * `ipconfig set <if> DHCP`: lift any NONE hold on `ifname` and, if its
 * link is up and no worker is running, start one. A link that is down
 * is brought administratively up and DHCP starts on link-active, as it
 * does for any interface.
 */
ipconfig_status_t	ipconfigd_set_dhcp(const char *ifname);

/*
 * `ipconfig set <if> NONE`: unwind `ifname`'s worker (deconfiguring
 * its lease) and hold the interface so later link events leave it
 * alone, until `set <if> DHCP` or an ipconfigd restart. Like Apple's,
 * the setting is not persisted.
 */
ipconfig_status_t	ipconfigd_set_none(const char *ifname);

/* Does `ifname` currently have a DHCP worker (acquiring or bound)? */
bool			ipconfigd_dhcp_active(const char *ifname);

#endif /* _IPCFG_IPCONFIGD_H_ */
