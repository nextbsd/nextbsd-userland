/*
 * ra_listen.h — IPv6 Router Advertisement listener for iter 7a.
 *
 * Single-shot: send one ICMPv6 Router Solicitation to ff02::2 on
 * `ifname`, wait up to `timeout_ms` for a Router Advertisement, parse
 * the first Prefix Information Option whose on-link + autonomous
 * flags are set, and return it.
 *
 * ra_open() / ra_recv() expose the same socket and validation for the
 * long-lived listener in ra_router.c, which keeps hearing RAs after
 * the first one so it can track each router's lifetime.
 *
 * RFC 4861 §4.2 (RA format) + RFC 4862 §5.5.3 (SLAAC processing).
 */
#ifndef RA_LISTEN_H
#define RA_LISTEN_H

#include <netinet/in.h>
#include <stdint.h>

struct ra_info {
	struct in6_addr	prefix;			/* /N from PIO */
	uint8_t		prefix_len;
	uint32_t	valid_lifetime;		/* seconds, RFC 4861 */
	uint32_t	preferred_lifetime;	/* seconds */
	struct in6_addr	router_lladdr;		/* fe80::… source of the RA */
	uint32_t	router_scope_id;	/* ifindex for the LL gateway */
	uint16_t	router_lifetime;	/* seconds; 0 = not a default
						 * router (RFC 4861 §4.2) */
};

/*
 * Acquire SLAAC parameters on `ifname`. Returns 0 on success (out
 * populated), 1 on no-RA-within-timeout (out untouched), -1 on
 * fatal error.
 */
int ra_acquire(const char *ifname, unsigned timeout_ms, struct ra_info *out);

/*
 * Open the RA receive socket for `ifname` (same filter + hop-limit
 * setup ra_acquire uses; 1s receive timeout). Returns the socket, or
 * -1 on error.
 */
int ra_open(const char *ifname);

/*
 * Receive one RA on `sock`. Returns 1 when a valid RA arrived on
 * `ifindex` (router fields of `out` populated; prefix fields only
 * when it carried an eligible PIO, else prefix_len is 0), 0 on
 * timeout or a rejected packet, -1 on a fatal error or shutdown.
 */
int ra_recv(int sock, unsigned ifindex, struct ra_info *out);

#endif /* RA_LISTEN_H */
