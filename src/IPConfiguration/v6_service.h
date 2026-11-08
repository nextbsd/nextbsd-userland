/*
 * v6_service.h — the IPv6 service on each interface (#423).
 *
 * Darwin's IPConfiguration runs a service per address family on each
 * interface (bootp ipconfigd.c IFState services and services_v6): IPv4 and
 * IPv6 start and stop independently, each with its own method. This is the
 * IPv6 half; the IPv4 half is the worker table in ipconfigd.c.
 *
 * The method comes from the service's IPv6 entity or from `ipconfig set`
 * (ipconfigd_v6_lookup()):
 *
 *   Automatic, RouterAdvertisement  SLAAC from a Router Advertisement
 *   Manual                          a static address (Addresses, PrefixLength)
 *   LinkLocal                       fe80:: only
 *   off (NONE-V6, or no entity)     ND6_IFF_IFDISABLED, no addresses
 *
 * The one link to IPv4 is the start: a service waits for the interface's
 * first IPv4 result (bound, given up, or IPv4 NONE), because the IPCFG-RA
 * marker must follow IPCFG-BOUND in CI. Darwin starts them independently.
 * Everything else is separate: an IPv4 method change, lease loss or restart
 * leaves IPv6 running, and IPv4's `ipconfig waitall` readiness never waits
 * for IPv6.
 *
 * None of these block: unwinding only sets a flag, and the service's thread
 * does the teardown, as for the IPv4 workers.
 */
#ifndef _IPCFG_V6_SERVICE_H_
#define _IPCFG_V6_SERVICE_H_

#include <signal.h>
#include <stdbool.h>

#include <netinet/in.h>

/*
 * `ifname` has its first IPv4 result: start its IPv6 service, or apply off.
 * Called by the IPv4 side when it binds, when it gives up and for a method
 * of NONE; repeats are harmless.
 */
void	v6_service_v4_result(const char *ifname);

/*
 * The link is down, or the interface has gone. Stop the service; it starts
 * again on the next first IPv4 result.
 */
void	v6_service_link_down(const char *ifname);

/*
 * `ifname`'s IPv6 method may have changed (a preferences apply, or `ipconfig
 * set`). Unwind what is running, if it differs, and start the new method.
 */
void	v6_service_reconfigure(const char *ifname);

/*
 * Does `ifname` need its link up for IPv6? False for off and for an
 * interface whose IPv6 is left alone.
 */
bool	v6_service_wants_link(const char *ifname);

/* A renew request (#572): solicit, if an Automatic service is running. */
void	v6_service_solicit(const char *ifname);

/* IPv6DuplicatedAddress for `addr` on `ifname` (#349). */
void	v6_service_on_duplicated(const char *ifname,
	    const struct in6_addr *addr);

/*
 * Wait for the IPv6 service's first result on `ifname`: its address
 * published, or no address to be had. Returns at once if no service is
 * running, or when `stop` or the daemon's got_term is set; bounded. The DHCP
 * worker uses this to start its renewal clock where it always started, so
 * IPCFG-RA still precedes IPCFG-RENEW; see service_run_bound().
 */
void	v6_service_wait_first_result(const char *ifname,
	    volatile sig_atomic_t *stop);

#endif /* _IPCFG_V6_SERVICE_H_ */
