/*
 * apply_lease_v6.h — install an IPv6 SLAAC address + default route
 * derived from a Router Advertisement.
 *
 * Iter 7a scope: one address, one default route, best-effort. No
 * lifetime tracking. Duplicate-address detection is the kernel's;
 * ipconfigd.c waits for it to finish before the route and the publish
 * (#407), and acts on a duplicate when configd's IPv6DuplicatedAddress
 * notification for it arrives (#349).
 */
#ifndef APPLY_LEASE_V6_H
#define APPLY_LEASE_V6_H

#include <stdbool.h>
#include <stdint.h>

#include "ra_listen.h"

/*
 * Clear ND6_IFF_IFDISABLED on `ifname` and install an EUI-64
 * link-local fe80::… address. This is the precondition for sending
 * an ND_ROUTER_SOLICIT: ff02::2 is link-local scope, so the kernel
 * needs an LL source on the egress interface. FreeBSD's per-image
 * default for `net.inet6.ip6.auto_linklocal` in this build is 0 (the
 * stack leaves IFDISABLED set on attach), so we cannot rely on the
 * kernel to auto-configure the LL. `accept_rtadv` sets
 * ND6_IFF_ACCEPT_RTADV (Automatic) or clears it (every other method,
 * #423). `ll_out`, if not NULL, gets the link-local address. Returns 0 on
 * success.
 */
int bring_v6_up(const char *ifname, bool accept_rtadv, struct in6_addr *ll_out);

/*
 * Take IPv6 off `ifname` (#423): set ND6_IFF_IFDISABLED, clear
 * ACCEPT_RTADV and AUTO_LINKLOCAL, and remove every IPv6 address on it.
 */
int v6_turn_off(const char *ifname);

/*
 * Install a Manual IPv6 address (#423): infinite lifetimes and no
 * IN6_IFF_AUTOCONF, as Apple's manual_v6 does. Tentative until DAD passes,
 * like a SLAAC address.
 */
int install_v6_manual_address(const char *ifname, const struct in6_addr *addr,
    uint8_t prefix_len);

/*
 * Build the SLAAC unicast address from `info`'s prefix + an EUI-64
 * derived from `ifname`'s MAC and install it via SIOCAIFADDR_IN6.
 * Returns 0 on success. The address is still tentative: DAD runs in
 * the kernel, and the caller waits for it before adding the default
 * route via info->router_lladdr (install_v6_default_route) and
 * publishing.
 *
 * `out_addr` receives the computed SLAAC address for the caller's
 * DAD wait and SCDynamicStore publish.
 */
int apply_ra_lease(const char *ifname, const struct ra_info *info,
    struct in6_addr *out_addr);

/*
 * Upper bound, in milliseconds, on how long DAD for a new address on
 * `ifname` can take: the up-to-1s initial delay plus
 * net.inet6.ip6.dad_count probes RetransTimer apart, plus 500 ms.
 */
unsigned v6_dad_budget_ms(const char *ifname);

/*
 * Remove `addr` from `ifname` (SIOCDIFADDR_IN6) — a SLAAC address DAD
 * found duplicated. Returns 0 on success.
 */
int remove_v6_address(const char *ifname, const struct in6_addr *addr);

/*
 * Add / delete the ::/0 default route via the link-local `gateway`
 * on `ifindex`. Adding a route that already exists and deleting one
 * that is already gone both return 0. ra_router.c uses these to keep
 * ::/0 in step with the advertising router's lifetime.
 */
int install_v6_default_route(const struct in6_addr *gateway,
    unsigned ifindex);
int remove_v6_default_route(const struct in6_addr *gateway,
    unsigned ifindex);

#endif /* APPLY_LEASE_V6_H */
