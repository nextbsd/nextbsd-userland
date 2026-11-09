/*
 * dhcpv6_socket.h — the DHCPv6 client socket (#424).
 *
 * One UDP socket on port 546 for the whole system, as Apple's bootp
 * DHCPv6Socket.c has it: each client holds a handle for its interface,
 * and a reader demultiplexes arriving packets by the receive interface
 * (ipi6_ifindex). The socket exists only while some client holds a handle.
 */
#ifndef _IPCFG_DHCPV6_SOCKET_H_
#define _IPCFG_DHCPV6_SOCKET_H_

#include <stddef.h>
#include <stdint.h>

struct dhcpv6_sock;

/* A handle for the interface with index `ifindex`. Returns NULL on failure. */
struct dhcpv6_sock *dhcpv6_socket_create(const char *ifname, unsigned ifindex);

/* Give up the handle and set `*sock` to NULL. */
void dhcpv6_socket_release(struct dhcpv6_sock **sock);

/*
 * Send `len` bytes to All_DHCP_Relay_Agents_and_Servers on the handle's
 * interface. Returns 0, or an errno value on failure.
 */
int dhcpv6_socket_transmit(struct dhcpv6_sock *sock, const void *pkt, int len);

/*
 * The next packet for this interface: copies up to `size` bytes into
 * `buf` and returns its length, or 0 if none arrived within `timeout_ms`.
 */
int dhcpv6_socket_receive(struct dhcpv6_sock *sock, void *buf, size_t size,
    unsigned timeout_ms);

#endif /* _IPCFG_DHCPV6_SOCKET_H_ */
