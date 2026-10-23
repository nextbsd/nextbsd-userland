/*
 * ra_router.h — IPv6 default-router lifetime tracking.
 *
 * ra_acquire hears one Router Advertisement and apply_ra_lease turns it
 * into a SLAAC address plus a ::/0 route. That route is userland-installed,
 * so nothing in the kernel ever takes it back: when the router goes away,
 * the default route stays and blackholes IPv6.
 *
 * ra_router keeps listening for RAs on the interface for the life of the
 * binding and tracks each advertising router's lifetime (RFC 4861 §6.3.4).
 * Darwin reference: bootp ipconfigd.c's router-expired handling.
 *
 *   - an RA with a non-zero router lifetime refreshes that router and
 *     re-asserts ::/0 while it is the default;
 *   - an RA with lifetime 0, or a lifetime running out, drops the router;
 *   - when the default router drops, ::/0 moves to another live router or
 *     is removed, and State:/.../IPv6 is republished to match.
 */
#ifndef RA_ROUTER_H
#define RA_ROUTER_H

#include <netinet/in.h>

#include "ra_listen.h"

struct ra_router;

/*
 * Start tracking routers on `ifname`. `ra` is the RA apply_ra_lease just
 * acted on (its router is taken to be the installed ::/0 gateway) and
 * `addr` the SLAAC address published for it; both are copied. Runs on its
 * own thread with its own configd session. Returns NULL on failure, in
 * which case the ::/0 route is left as installed.
 */
struct ra_router *ra_router_start(const char *ifname, const struct ra_info *ra,
    const struct in6_addr *addr);

/*
 * Stop tracking and wait for the thread to exit. With `remove_route` the
 * ::/0 route it manages is deleted as well (link down / lease lost); the
 * caller still owns removing the published IPv6 key. NULL is a no-op.
 */
void ra_router_stop(struct ra_router *r, int remove_route);

#endif /* RA_ROUTER_H */
