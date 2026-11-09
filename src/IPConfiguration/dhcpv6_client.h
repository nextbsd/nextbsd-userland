/*
 * dhcpv6_client.h — the DHCPv6 client on one interface (#424).
 *
 * Apple's bootp IPConfiguration.bproj/DHCPv6Client.c in the local thread
 * style: one thread per client, sharing the system's one DHCPv6 socket.
 * This is the stateless half: an Information-Request, retried with
 * Apple's constants until a reply or until the client is stopped.
 */
#ifndef _IPCFG_DHCPV6_CLIENT_H_
#define _IPCFG_DHCPV6_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dns_config.h"

/* The largest DHCPv6 packet kept or received. */
#define	DHCPV6_PACKET_MAX	1500

struct dhcpv6_client;

/* A reply was saved. Called on the client's own thread, with no locks held. */
typedef void (*dhcpv6_client_notify_t)(void *arg,
    struct dhcpv6_client *client);

/* Start the client on `ifname`; `notify` hears each saved reply. */
struct dhcpv6_client *dhcpv6_client_start(const char *ifname, unsigned ifindex,
    dhcpv6_client_notify_t notify, void *arg);

/* Stop the client, wait for its thread, free it, and set `*client` to NULL. */
void dhcpv6_client_stop(struct dhcpv6_client **client);

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
