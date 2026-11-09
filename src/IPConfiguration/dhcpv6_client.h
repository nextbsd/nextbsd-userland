/*
 * dhcpv6_client.h — the DHCPv6 client on one interface (#424).
 *
 * Apple's bootp IPConfiguration.bproj/DHCPv6Client.c in the local thread
 * style: one thread per client, sharing the system's one DHCPv6 socket.
 * Stateless is an Information-Request, retried with Apple's constants until
 * a reply or until the client is stopped. Stateful (#576) runs Solicit,
 * Request and Bound, then Renew and Rebind, and holds one address (IA_NA)
 * for the interface; the lease lives only in memory.
 */
#ifndef _IPCFG_DHCPV6_CLIENT_H_
#define _IPCFG_DHCPV6_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>

#include "dns_config.h"
#include "ra_listen.h"

/* The largest DHCPv6 packet kept or received. */
#define	DHCPV6_PACKET_MAX	1500

struct dhcpv6_client;

/*
 * The client's information or address changed: a reply saved, an address
 * ready or withdrawn. Called on the client's own thread with no locks held.
 */
typedef void (*dhcpv6_client_notify_t)(void *arg,
    struct dhcpv6_client *client);

/*
 * Start the client on `ifname`: stateful if `allocate`, or stateless. A
 * lease still valid from before a link bounce on this interface is
 * Confirmed. `ra` gives the on-link prefixes a leased address's length
 * comes from (copied).
 */
struct dhcpv6_client *dhcpv6_client_start(const char *ifname, unsigned ifindex,
    bool allocate, const struct ra_info *ra, dhcpv6_client_notify_t notify,
    void *arg);

/* True if the client was started `allocate`. */
bool dhcpv6_client_is_stateful(const struct dhcpv6_client *client);

/*
 * Run the start rules again on a running client (DHCPv6ClientStart): an RA
 * was sampled. `ra` is copied.
 */
void dhcpv6_client_restart(struct dhcpv6_client *client,
    const struct ra_info *ra);

/*
 * How the client stops:
 *   DHCPV6_STOP_LINK     link down or flags cleared: remove the address, keep
 *                        a still-valid lease in memory to Confirm later
 *   DHCPV6_STOP_RELEASE  service stop: Release a still-valid lease, forget it
 *   DHCPV6_STOP_KEEP     daemon exit: leave the address alone, as for IPv4
 */
typedef enum {
	DHCPV6_STOP_LINK,
	DHCPV6_STOP_RELEASE,
	DHCPV6_STOP_KEEP
} dhcpv6_stop_t;

/* Stop the client, wait for its thread, free it, and set `*client` to NULL. */
void dhcpv6_client_stop(struct dhcpv6_client **client, dhcpv6_stop_t how);

/*
 * The address the client holds, once past DAD: copied to `addr` and
 * `prefix_len`, returning true (DHCPv6ClientCopyAddresses).
 */
bool dhcpv6_client_copy_address(struct dhcpv6_client *client,
    struct in6_addr *addr, uint8_t *prefix_len);

/*
 * The kernel reported `addr` duplicated. True if it's the client's address,
 * which it will Decline.
 */
bool dhcpv6_client_on_duplicated(struct dhcpv6_client *client,
    const struct in6_addr *addr);

/*
 * The last reply saved: copies it into `buf`, sets `*len` and returns true,
 * or returns false if there is none or it doesn't fit in `size`.
 */
bool dhcpv6_client_copy_packet(struct dhcpv6_client *client, void *buf,
    size_t size, size_t *len);

/*
 * The DNS the saved reply carries: its DNS_SERVERS, and its DOMAIN_LIST if
 * it has servers, as Apple's DNSEntityCreateWithDHCPv6Info(). `*out` is
 * zeroed first. False if the reply has no servers.
 */
bool dhcpv6_client_copy_dns(struct dhcpv6_client *client,
    struct ipconfig_dns *out);

#endif /* _IPCFG_DHCPV6_CLIENT_H_ */
