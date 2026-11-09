/*
 * bound_state.h — thread-safe table of ipconfigd's currently-bound
 * DHCPv4 leases, one entry per interface.
 *
 * This was a single global holding one lease, which capped the daemon
 * at exactly one bound interface for its lifetime (#38) and — because
 * nothing ever cleared it — made the post-expiry re-arm dead code
 * (#37). It is now a small fixed table: each interface's DHCP worker
 * writes its own entry on BOUND and clears it on teardown, and the
 * Mach service thread reads across them. One mutex guards the whole
 * table; the access pattern is a couple of writers plus a low rate of
 * readers, so per-entry locking would buy nothing.
 *
 * The table also answers "who is primary?" (#41). State:/Network/
 * Global/IPv4 used to be last-binder-wins — whichever service bound
 * most recently declared itself primary — so on a laptop with em0 and
 * wlan0 the answer was a race. Primary is now the highest-ranked bound
 * interface by iface_rank(): wired beats wireless beats everything
 * else, which is macOS's default service order. Unplug the Ethernet
 * and wlan0 is promoted; plug it back in and em0 reclaims it. A
 * user-reorderable service list (Apple's real model, driven from
 * preferences.plist) can replace iface_rank() later without touching
 * a single caller.
 */
#ifndef _IPCFG_BOUND_STATE_H_
#define _IPCFG_BOUND_STATE_H_

#include <net/if.h>		/* IF_NAMESIZE */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dhcp_packet.h"

/*
 * Max simultaneously-bound interfaces. A desktop has one, a laptop two
 * (em0 + wlan0). Eight is slack for USB Ethernet, a dock, and a second
 * radio; the table is a flat scanned array, so the bound only exists to
 * keep it stack-cheap.
 */
#define	BOUND_MAX_IF	8

/* Service-order ranks. Highest bound rank wins the primary election (#41). */
#define	IFRANK_WIRED	100
#define	IFRANK_WIRELESS	50
#define	IFRANK_OTHER	10
#define	IFRANK_NEVER	0	/* loopback — never a primary service */

/* Initialize the table. Safe to call before any writer. */
void	bound_state_init(void);

/*
 * Rank `ifname` for the primary election. wlanN is wireless — the same
 * name-prefix heuristic libSystemConfiguration's SCNetworkInterface.c
 * uses to type an interface kSCNetworkInterfaceTypeIEEE80211, and it
 * has to be a name check: FreeBSD's 802.11 clones go through
 * ether_ifattach and therefore report IFT_ETHER, so the media type
 * cannot distinguish wlan0 from em0. lo* never participates.
 */
int	iface_rank(const char *ifname);

/* Mark `ifname` BOUND with `lease` (insert or update). */
void	bound_state_set(const char *ifname, const struct dhcp_lease *lease);

/*
 * The same for a 169.254 address (#426), from a LinkLocal service or a DHCP
 * service's fallback. It counts as bound everywhere except the election: a
 * link-local binding is primary only when nothing else is bound, so a DHCP
 * failure on em0 cannot take the default service away from a healthy wlan0.
 * bound_state_set() on the same interface clears the flag.
 */
void	bound_state_set_linklocal(const char *ifname,
	    const struct dhcp_lease *lease);

/* Is this interface's binding a link-local one? */
bool	bound_state_is_linklocal(const char *ifname);

/*
 * Drop `ifname`'s binding. The absence of this call is what made #37's
 * re-arm dead code: on_link_event cleared its in-flight flag after a
 * lease expired, but bound_state_any() still reported the interface
 * bound, so the very next guard short-circuited and DHCP never ran
 * again. No-op if `ifname` is not bound.
 */
void	bound_state_clear(const char *ifname);

/* Is this specific interface bound? */
bool	bound_state_is_bound(const char *ifname);

/* Number of currently-bound interfaces. */
int	bound_state_count(void);

/* Copy out the bound interface names, at most `max`; returns how many. */
int	bound_state_list(char names[][IF_NAMESIZE], int max);

/*
 * The primary service: the highest-ranked bound interface (#41), with
 * link-local bindings considered only when no other is bound (#426).
 * Returns false when nothing is bound. Ties (two wired NICs) are broken
 * by table order, which is stable for a given boot.
 */
bool	bound_state_primary(char *name_out, size_t name_out_sz);

/*
 * Look up `ifname`'s bound IPv4 address. On hit returns true and writes
 * the address into *addr_out (network byte order, packed as uint32_t —
 * what ip_address_t carries on the MIG wire).
 */
bool	bound_state_get_addr(const char *ifname, uint32_t *addr_out);

/*
 * Copy out `ifname`'s full bound lease. Used to regenerate
 * /etc/resolv.conf from whichever interface currently holds primary, so
 * that a secondary binding cannot overwrite the primary's DNS.
 */
bool	bound_state_get_lease(const char *ifname, struct dhcp_lease *out);

/*
 * `ifname`'s saved DHCP packet (#430). The DHCP service records every ACK
 * it accepts; bound_state_set() leaves this alone, and a Manual service
 * has none. No-op if `ifname` is not bound.
 */
void	bound_state_set_packet(const char *ifname,
	    const struct saved_pkt *pkt);

/* Copy out `ifname`'s saved packet. False if it has no packet. */
bool	bound_state_get_packet(const char *ifname, struct saved_pkt *out);

/*
 * The names of the bound interfaces, primary first, for a caller that
 * walks every service (ipconfig getoption ""). Returns how many were
 * written, at most `max`.
 */
int	bound_state_names(char names[][IF_NAMESIZE], int max);

#endif /* _IPCFG_BOUND_STATE_H_ */
