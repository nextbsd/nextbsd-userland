/*
 * apply_lease_v6.h — install an IPv6 SLAAC address + default route
 * derived from a Router Advertisement.
 *
 * Iter 7a scope: one address, one default route, best-effort. No
 * lifetime tracking. The address is checked for duplicate-address
 * detection before the route is added.
 */
#ifndef APPLY_LEASE_V6_H
#define APPLY_LEASE_V6_H

#include "ra_listen.h"

/*
 * Clear ND6_IFF_IFDISABLED on `ifname` and install an EUI-64
 * link-local fe80::… address. This is the precondition for sending
 * an ND_ROUTER_SOLICIT: ff02::2 is link-local scope, so the kernel
 * needs an LL source on the egress interface. FreeBSD's per-image
 * default for `net.inet6.ip6.auto_linklocal` in this build is 0 (the
 * stack leaves IFDISABLED set on attach), so we cannot rely on the
 * kernel to auto-configure the LL. Returns 0 on success.
 */
int bring_v6_up(const char *ifname);

/*
 * Build the SLAAC unicast address from `info`'s prefix + an EUI-64
 * derived from `ifname`'s MAC, install it via SIOCAIFADDR_IN6, wait
 * for DAD, and add a default route via info->router_lladdr. Returns 0
 * on success (address installed; route is best-effort and logged
 * separately). Returns -1 if DAD finds the address duplicated; the
 * address is removed and no route is added.
 *
 * `out_addr` receives the computed SLAAC address for the caller's
 * SCDynamicStore publish; it is populated even if route install
 * fails downstream.
 */
int apply_ra_lease(const char *ifname, const struct ra_info *info,
    struct in6_addr *out_addr);

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
