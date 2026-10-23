/*
 * sc_link_watch.h — ipconfigd SCDynamicStore interface-state watcher.
 *
 * Watches two entities per interface:
 *
 *   State:/Network/Interface/<if>/Link      configd's KernelEventMonitor
 *                                           publishes it from PF_ROUTE.
 *   State:/Network/Interface/<if>/AirPort   wland publishes it (802.11).
 *
 * The callback fires when an interface should be admin-upped (link down) or
 * DHCP'd (link up). DHCP requires BOTH Link{Active} AND
 * AirPort{Authenticated} — because net80211 reports the link UP at
 * ASSOCIATION, which is before the WPA 4-way handshake, and DHCP onto an
 * unauthenticated port is simply dropped.
 *
 * An interface with NO AirPort key is treated as ready. That default is what
 * keeps every wired NIC behaving exactly as it did before the gate existed.
 * See sc_link_watch.c for the full rationale.
 */
#ifndef SC_LINK_WATCH_H
#define SC_LINK_WATCH_H

#include <stdint.h>

/*
 * Invoked (on a libdispatch worker thread) for every change to a watched
 * interface's Link entity, AND for each one found by the initial scan.
 * `ifname` is the BSD interface name; `active` is non-zero when the link
 * is up (Active:true), zero when it is present but down, and
 * SC_LINK_GONE when the Link key has been removed — configd drops it when
 * the interface departs, which on Darwin is what ends the service.
 * `lease_cap_secs` is forwarded verbatim from sc_link_watch_start. The
 * callback decides what to do (e.g. admin-up the interface when down so
 * its link can negotiate; DHCP when up, skipping if already bound; tear
 * the interface's worker down when gone) and may block in the DHCP +
 * lease loop. lo0 is filtered by the watcher.
 */
#define	SC_LINK_GONE	(-1)

typedef void (*sc_link_watch_cb)(const char *ifname, int active,
	    uint32_t lease_cap_secs);

/*
 * Invoked (on a libdispatch worker thread) for each address-conflict
 * notification configd posts (#348/#349). These keys are notify-only —
 * configd sends a change for them with SCDynamicStoreNotifyValue but never
 * stores a value — so everything is carried in the key itself:
 *
 *   State:/Network/Interface/<if>/IPv4ARPCollision/<ip>/<hw>
 *       another host claimed <ip> on <if> from hardware address <hw>;
 *       `af` is AF_INET and `addr` a struct in_addr.
 *   State:/Network/Interface/<if>/IPv6DuplicatedAddress/<ip>/<hw>
 *       DAD found <ip> in use; `af` is AF_INET6 and `addr` a struct
 *       in6_addr. `hw` is all zero until the kernel records the
 *       defender's address.
 *
 * `hw` / `hw_len` are the colon-hex <hw> component decoded (hw_len 0 when
 * the key has none). lo0 is filtered.
 */
typedef void (*sc_addr_conflict_cb)(const char *ifname, int af,
	    const void *addr, const uint8_t *hw, int hw_len);

/*
 * Open a configd session, watch State:/Network/Interface/<if>/Link,
 * and arrange `cb` to run when any interface links up. Also performs an
 * immediate scan of existing keys so a link that was already Active
 * before the watch registered still fires. `conflict_cb` (may be NULL)
 * receives the ARP-collision and duplicated-address notifications on the
 * same session. Returns 0 on success, -1 if the session or watch could
 * not be established (non-fatal to the caller — DHCP simply won't be
 * link-triggered).
 */
int sc_link_watch_start(sc_link_watch_cb cb, sc_addr_conflict_cb conflict_cb,
    uint32_t lease_cap_secs);

/*
 * Synchronously read State:/Network/Interface/<if>/Link {Active} from configd.
 * Returns non-zero if the interface's link is Active. The store *value* is
 * authoritative even when the change *wakeup* was lost — ipconfigd polls this
 * after admin-up to recover a dropped Active notification (#250).
 */
int link_active_in_store(const char *ifname);

/*
 * Wait for duplicate-address detection on `addr` (a struct in6_addr just
 * installed on `ifname`) to finish, as Darwin's rtadv.c waits before it
 * publishes (#407). Reads State:/Network/Interface/<if>/IPv6, which
 * configd republishes when the kernel's DAD-completion event changes the
 * address's flags, again on each change the link watch sees (and at least
 * every 250 ms), and returns once that entry lists `addr` without
 * IN6_IFF_TENTATIVE:
 *
 *   SC_V6_DAD_OK          DAD passed
 *   SC_V6_DAD_DUPLICATED  DAD found the address in use
 *   SC_V6_DAD_TIMEOUT     no answer within `timeout_ms`
 *
 * Blocks the calling thread; never the link-watch queue.
 */
#define	SC_V6_DAD_OK		0
#define	SC_V6_DAD_DUPLICATED	1
#define	SC_V6_DAD_TIMEOUT	2

int sc_v6_dad_wait(const char *ifname, const void *addr, unsigned timeout_ms);

#endif /* SC_LINK_WATCH_H */
