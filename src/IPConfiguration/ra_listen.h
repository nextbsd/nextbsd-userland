/*
 * ra_listen.h — IPv6 Router Advertisement listener for iter 7a.
 *
 * Single-shot: send one ICMPv6 Router Solicitation to ff02::2 on
 * `ifname`, wait up to `timeout_ms` for a Router Advertisement and
 * return it: the M and O flags, RDNSS, the on-link prefixes and the
 * first Prefix Information Option whose on-link + autonomous flags are
 * set, if it has one.
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
#include <stdbool.h>
#include <stdint.h>

/* Recursive DNS servers and on-link prefixes kept from one RA. */
#define	RA_MAX_RDNSS	8
#define	RA_MAX_ONLINK	8

struct ra_onlink_prefix {
	struct in6_addr	prefix;
	uint8_t		prefix_len;
};

struct ra_info {
	struct in6_addr	prefix;			/* /N from PIO; prefix_len 0 */
	uint8_t		prefix_len;		/* when no eligible PIO */
	uint32_t	valid_lifetime;		/* seconds, RFC 4861 */
	uint32_t	preferred_lifetime;	/* seconds */
	struct in6_addr	router_lladdr;		/* fe80::… source of the RA */
	uint32_t	router_scope_id;	/* ifindex for the LL gateway */
	uint16_t	router_lifetime;	/* seconds; 0 = not a default
						 * router (RFC 4861 §4.2) */
	bool		managed;		/* M: addresses via DHCPv6 */
	bool		other;			/* O: other config via DHCPv6 */
	/* RDNSS, as RTADVSocket.c find_rdnss(): the first with a lifetime. */
	struct in6_addr	rdnss[RA_MAX_RDNSS];
	unsigned	rdnss_count;
	/* Every prefix with L set, A or not; a DHCPv6 address's on-link /N. */
	struct ra_onlink_prefix onlink[RA_MAX_ONLINK];
	unsigned	onlink_count;
};

/*
 * Listen for one Router Advertisement on `ifname`. Returns 0 for any
 * valid RA (out populated; prefix_len is 0 when it carried no eligible
 * PIO, and the router fields, flags and RDNSS hold regardless), 1 on
 * no-RA-within-timeout (out untouched), -1 on fatal error.
 */
int ra_acquire(const char *ifname, unsigned timeout_ms, struct ra_info *out);

/*
 * Send one Router Solicitation on `ifname` and return without waiting
 * for an answer; whatever is tracking RAs on the interface hears the
 * reply. For `scutil --renew`, as Darwin's rtadv restarts solicitation
 * on a renew event. Returns 0 on success, -1 on error.
 */
int ra_solicit(const char *ifname);

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
