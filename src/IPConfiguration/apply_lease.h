/*
 * apply_lease.h — install / remove a DHCPv4 lease on an interface.
 *
 * apply_lease() configures the interface address via SIOCAIFADDR and
 * installs the default route via the PF_ROUTE socket.
 *
 * deconfigure_lease() is its inverse, and it did not exist (#39). The
 * daemon installed an address and a default route and then never took
 * them back — not on link-down, not on lease expiry, not on interface
 * removal. Walk out of WLAN range and a dead IP plus a dead default
 * route stayed installed, silently blackholing traffic. There was no
 * SIOCDIFADDR and no RTM_DELETE anywhere in the tree.
 *
 * DNS is deliberately NOT handled per-interface. /etc/resolv.conf is
 * global state, so a per-interface writer means last-to-bind wins —
 * wlan0 would clobber the DNS of a wired em0 that outranks it.
 * resolv_conf_sync() is handed the primary service's merged DNS by
 * dns_config.c (#41, #425), which calls it whenever that changes. It
 * writes atomically (temp + fsync + rename); the old in-place fopen("w")
 * could be read half-written.
 */
#ifndef _IPCFG_APPLY_LEASE_H_
#define _IPCFG_APPLY_LEASE_H_

#include "dhcp_packet.h"

/*
 * Apply `lease` to `ifname`: SIOCAIFADDR + default route, plus an
 * RFC 5227 §2.3 gratuitous-ARP announce (best-effort). Returns 0 on
 * full success, non-zero if a sub-step failed (the failing step is
 * logged). Does not touch /etc/resolv.conf — the caller runs
 * dns_config_update() once the binding is recorded in bound_state.
 */
int	apply_lease(const char *ifname, const struct dhcp_lease *lease);

/*
 * Remove `lease` from `ifname`: SIOCDIFADDR + RTM_DELETE of the default
 * route via lease->router. Best-effort and idempotent — a missing
 * address or route is NOT an error, because teardown races link-down:
 * when an interface goes away the kernel may already have dropped both.
 * Returns 0 if the interface ends up clean, non-zero only on an
 * unexpected failure.
 */
int	deconfigure_lease(const char *ifname, const struct dhcp_lease *lease);

/*
 * Add `lease`'s default route alone, for an address already installed: an
 * INFORM service only learns its router from the reply (#569).
 */
int	apply_lease_router(const struct dhcp_lease *lease);

/*
 * Regenerate /etc/resolv.conf from `dns`, the merged DNS of the primary
 * service on `ifname`, in Apple's set_dns() order: search (or else
 * domain), then every nameserver, then sortlist. Writes atomically. With
 * `dns` NULL the primary has no DNS and the file is removed, as
 * update_dns() removes Darwin's. Either way network_change is posted.
 */
struct ipconfig_dns;
void	resolv_conf_sync(const char *ifname, const struct ipconfig_dns *dns);

#endif /* _IPCFG_APPLY_LEASE_H_ */
