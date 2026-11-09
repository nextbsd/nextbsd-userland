/*
 * dhcp_discover.h — DHCPv4 client front door.
 *
 * Despite the historical name (iter 2 was just DISCOVER/OFFER),
 * this module now runs the full RFC 2131 INIT → SELECTING →
 * REQUESTING → BOUND state machine: brings the interface up,
 * opens a BPF descriptor, sends DHCPDISCOVER (retransmitted on
 * Darwin's ladder, 1/2/4/8s with fuzz, #426), parses the first
 * DHCPOFFER it sees, sends DHCPREQUEST, parses the DHCPACK, and
 * hands the caller a struct dhcp_lease.
 *
 * Long BPF read waits are signal-aware: a SIGTERM/SIGHUP arriving
 * during a wait short-circuits the loop (the daemon's global
 * `got_term` flag is checked between reads + on EINTR).
 *
 * Lease persistence, T1/T2 renewal, RENEWING/REBINDING — iter 4.
 */
#ifndef _IPCFG_DHCP_DISCOVER_H_
#define _IPCFG_DHCP_DISCOVER_H_

#include <signal.h>	/* sig_atomic_t */
#include <stddef.h>	/* size_t */

#include "dhcp_packet.h"

/*
 * The daemon's signal-asserted shutdown flag (defined in
 * ipconfigd.c). dhcp_discover poll loops check it between BPF
 * reads so a SIGTERM during the (potentially multi-second) DHCP
 * retransmit window short-circuits the wait — fixes iter 2's
 * 10-second deafness.
 */
extern volatile sig_atomic_t got_term;

/*
 * Run one INIT → BOUND exchange on `ifname` over Darwin's retransmit
 * ladder (#426). On success returns DHCP_ACQUIRE_OK and fills `*out` with
 * the bound lease and `*saved` with the ACK that carried it. The caller
 * passes the lease to apply_lease() to actually configure the interface.
 *
 * Anything else is no lease: the caller logs IPCFG-BOUND-FAIL once per
 * acquire, not this function. NO_SERVER is RetryCount+1 unanswered
 * DISCOVERs (dhcp_no_server()), NO_ACK an offer that was never confirmed
 * (back to INIT), DECLINED an offer another host answered ARP for. ERROR is
 * a local failure, or a stop.
 *
 * `*stop` (may be NULL) is the worker's per-interface stop request:
 * it is checked alongside got_term between sends and on every BPF
 * read wakeup, so a stopped service gives up within about a second
 * instead of finishing the retransmit ladder (#410). The caller checks
 * for a stop before treating a result as a failure.
 *
 * `on_try` (may be NULL) is called after each DISCOVER is sent, with the
 * try number counting from 1; it runs on this thread, inside the wait for
 * that try's reply. See ipconfigd.c's link-local fallback.
 */
#define	DHCP_ACQUIRE_OK		0
#define	DHCP_ACQUIRE_NO_SERVER	(-1)
#define	DHCP_ACQUIRE_NO_ACK	(-2)
#define	DHCP_ACQUIRE_DECLINED	(-3)
#define	DHCP_ACQUIRE_ERROR	(-4)

typedef void (*dhcp_try_cb)(int try, void *arg);

int	dhcp_lease_acquire(const char *ifname, struct dhcp_lease *out,
	    struct saved_pkt *saved, volatile sig_atomic_t *stop,
	    dhcp_try_cb on_try, void *on_try_arg);

/*
 * Renew a bound lease — send a DHCPREQUEST in BOUND/RENEWING form
 * (ciaddr=current addr, no opt 50/54, broadcast flag set so SLIRP
 * and other simple servers accept it without ARP-resolving the
 * client first), wait for the DHCPACK, fill *new_lease and *saved.
 *
 * iter 4 keeps RENEWING and REBINDING on the same broadcast-BPF
 * path; RFC 2131 §4.3.2 says RENEWING SHOULD unicast but servers
 * MAY (and most do) accept the broadcast form. Splitting unicast
 * out is iter 5+ work alongside the threading rework.
 *
 * Returns 0 on success, -1 on timeout / shutdown / fatal error.
 */
int	dhcp_renew(const char *ifname,
	    const struct dhcp_lease *existing,
	    struct dhcp_lease *new_lease, struct saved_pkt *saved);

/*
 * Send one DHCPDECLINE for `lease` (requested IP = lease->addr, server
 * id = lease->server) — the bound address turned out to be in use by
 * another host (an ARP collision, #349; RFC 2131 §3.1.5, Apple's
 * dhcp_decline). The caller has already taken the address off the
 * interface. Returns 0 if the DECLINE was sent.
 */
int	dhcp_decline(const char *ifname, const struct dhcp_lease *lease);

/*
 * Pick the first non-loopback Ethernet interface (the QEMU guest's
 * em0 in CI). Writes the name into ifname_out (buffer is at least
 * IFNAMSIZ).  Returns 0 on success.
 */
int	dhcp_pick_interface(char *ifname_out, size_t ifname_sz);

#endif /* _IPCFG_DHCP_DISCOVER_H_ */
