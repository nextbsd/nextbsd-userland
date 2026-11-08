/*
 * lease_loop.h — post-BOUND lease management for ipconfigd.
 *
 * iter 4 owns the BOUND lifecycle:
 *   BOUND      -> sleep until T1 (lease/2)
 *   T1 fires   -> RENEWING: dhcp_renew(); on success re-publish + reset
 *                            timers; on fail, sleep to T2
 *   T2 fires   -> REBINDING: dhcp_renew() (same broadcast path);
 *                            on success re-publish + reset; on fail,
 *                            sleep until lease expiry, then unpublish
 *                            and exit (caller may restart from INIT)
 *
 * Runs on that interface's DHCP worker thread — sleeps in 1s ticks so
 * SIGTERM, or a link-down on this interface, shortens the wait.
 */
#ifndef _IPCFG_LEASE_LOOP_H_
#define _IPCFG_LEASE_LOOP_H_

#include <signal.h>
#include <stdint.h>

#include "dhcp_packet.h"

struct sc_publish;

/*
 * Run the BOUND lease loop for `ifname` until lease expiry, SIGTERM, or
 * `*stop`. `lease` is the freshly-bound lease (updated in-place on every
 * successful renewal). `pub` is the (optional) configd publish session —
 * re-publish after each renewal; NULL means no publishing.
 *
 * `lease_cap_secs` caps the effective lease time used to derive T1 / T2 —
 * useful for CI where SLIRP hands out 86400s leases that would otherwise
 * mean T1 = 12 hours. 0 = no cap. Both the initial lease and post-renewal
 * leases are capped. The cap does NOT touch `lease->lease_time` (the
 * published lease authoritatively reports the server's value); it only
 * shortens the daemon's renewal-trigger timing.
 *
 * `renew` asks for a renewal now (`scutil --renew`): the loop breaks the
 * T1 or T2 wait it is in and sends the DHCPREQUEST at once. An unanswered
 * request changes nothing and the wait carries on. May be NULL.
 *
 * `stop` is this interface's cancellation flag, owned by ipconfigd.c's
 * worker table and set when the interface's link goes down or it is
 * removed. Without it a worker parked in a multi-hour T1 sleep could not
 * be unwound, so an interface that lost its link would keep its lease,
 * its address, and its default route until the daemon exited — the state
 * #39 is about. May be NULL (no cancellation).
 *
 * Returns 0 on clean shutdown (signal or *stop), -1 on lease loss.
 */
int	lease_loop_run(const char *ifname, struct dhcp_lease *lease,
	    struct sc_publish *pub, uint32_t lease_cap_secs,
	    volatile sig_atomic_t *renew, volatile sig_atomic_t *stop);

#endif /* _IPCFG_LEASE_LOOP_H_ */
