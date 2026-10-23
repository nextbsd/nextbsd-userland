/*
 * ra_router.c — keep ::/0 in step with the advertising routers' lifetimes.
 *
 * One thread per bound interface with IPv6. It owns an RA receive socket
 * (ra_open, 1s receive timeout) and a small table of routers heard on the
 * link, each with a CLOCK_MONOTONIC expiry. Every RA refreshes or drops
 * its router; every pass (one per RA or 1s timeout) expires stale entries
 * and re-elects the default:
 *
 *   - the current default stays while it is in the table, rather than
 *     flapping to whichever router spoke last (RFC 4861 §6.3.6);
 *   - otherwise the first live router takes over, or there is none.
 *
 * A change of default deletes the old ::/0, adds the new one, and
 * republishes State:/Network/Service/<UUID>/IPv6 with the new Router (or
 * without one). An RA from the current default also re-adds ::/0, so a
 * route removed behind our back comes back while the router is valid.
 *
 * The thread has its own configd session: the DHCP worker's session is in
 * use by lease_loop on the worker thread at the same time.
 */
#include "ra_router.h"

#include <sys/types.h>

#include <net/if.h>
#include <netinet/in.h>

#include <arpa/inet.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apply_lease_v6.h"
#include "sc_publish.h"

extern volatile sig_atomic_t got_term;

/* Routers remembered per link; RAs from further routers are ignored. */
#define RA_MAX_ROUTERS	4

struct ra_router_ent {
	struct in6_addr	addr;		/* link-local source of the RA */
	time_t		expires;	/* CLOCK_MONOTONIC seconds */
};

struct ra_router {
	char			ifname[IFNAMSIZ];
	unsigned		ifindex;
	struct in6_addr		addr;		/* published SLAAC address */
	uint8_t			prefix_len;
	struct ra_router_ent	rtr[RA_MAX_ROUTERS];
	int			nrtr;
	bool			have_default;	/* ::/0 installed via dflt */
	struct in6_addr		dflt;
	struct sc_publish	*pub;
	pthread_t		tid;
	volatile sig_atomic_t	stop;
};

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[rtr] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static time_t
now_sec(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec);
}

static const char *
addr_str(const struct in6_addr *a, char *buf, size_t len)
{
	if (inet_ntop(AF_INET6, a, buf, (socklen_t)len) == NULL)
		(void)strlcpy(buf, "?", len);
	return (buf);
}

static int
find_router(const struct ra_router *r, const struct in6_addr *a)
{
	int i;

	for (i = 0; i < r->nrtr; i++)
		if (IN6_ARE_ADDR_EQUAL(&r->rtr[i].addr, a))
			return (i);
	return (-1);
}

static void
drop_router(struct ra_router *r, int i)
{
	r->rtr[i] = r->rtr[r->nrtr - 1];
	r->nrtr--;
}

/*
 * Fold one RA's router lifetime into the table (RFC 4861 §6.3.4): a
 * non-zero lifetime adds or refreshes the router, zero removes it.
 */
static void
note_router(struct ra_router *r, const struct ra_info *ra, time_t now)
{
	char buf[INET6_ADDRSTRLEN];
	int i;

	i = find_router(r, &ra->router_lladdr);
	if (ra->router_lifetime == 0) {
		if (i >= 0) {
			xlog("%s: router %s withdrew (lifetime 0)", r->ifname,
			    addr_str(&ra->router_lladdr, buf, sizeof(buf)));
			drop_router(r, i);
		}
		return;
	}
	if (i < 0) {
		if (r->nrtr == RA_MAX_ROUTERS)
			return;
		i = r->nrtr++;
		r->rtr[i].addr = ra->router_lladdr;
		xlog("%s: router %s lifetime %us", r->ifname,
		    addr_str(&ra->router_lladdr, buf, sizeof(buf)),
		    (unsigned)ra->router_lifetime);
	}
	r->rtr[i].expires = now + ra->router_lifetime;
}

static void
expire_routers(struct ra_router *r, time_t now)
{
	char buf[INET6_ADDRSTRLEN];
	int i;

	for (i = r->nrtr - 1; i >= 0; i--) {
		if (r->rtr[i].expires > now)
			continue;
		xlog("%s: router %s lifetime expired", r->ifname,
		    addr_str(&r->rtr[i].addr, buf, sizeof(buf)));
		drop_router(r, i);
	}
}

static void
publish(struct ra_router *r)
{
	if (r->pub == NULL)
		return;
	if (sc_publish_ipv6(r->pub, r->ifname, &r->addr, r->prefix_len,
	    r->have_default ? &r->dflt : NULL) != 0)
		xlog("%s: republish State:/.../IPv6 failed", r->ifname);
}

/*
 * Re-elect the default router and bring ::/0 and the published Router in
 * line with it. `refresh` re-adds ::/0 for an unchanged default (an RA
 * from it just arrived).
 */
static void
select_default(struct ra_router *r, bool refresh)
{
	char buf[INET6_ADDRSTRLEN];

	if (r->have_default && find_router(r, &r->dflt) >= 0) {
		if (refresh)
			(void)install_v6_default_route(&r->dflt, r->ifindex);
		return;
	}

	if (!r->have_default && r->nrtr == 0)
		return;		/* still no router */

	if (r->have_default) {
		if (remove_v6_default_route(&r->dflt, r->ifindex) == 0)
			xlog("%s: removed ::/0 via %s", r->ifname,
			    addr_str(&r->dflt, buf, sizeof(buf)));
		r->have_default = false;
	}
	if (r->nrtr > 0) {
		r->dflt = r->rtr[0].addr;
		r->have_default = true;
		if (install_v6_default_route(&r->dflt, r->ifindex) == 0)
			xlog("%s: ::/0 via %s", r->ifname,
			    addr_str(&r->dflt, buf, sizeof(buf)));
	}
	publish(r);
}

static void *
ra_router_main(void *arg)
{
	struct ra_router *r = arg;
	struct ra_info ra;
	int sock, rc;

	sock = ra_open(r->ifname);
	if (sock < 0)
		xlog("%s: RA socket unavailable; router lifetimes still "
		    "expire but cannot be refreshed", r->ifname);

	while (!got_term && !r->stop) {
		time_t now;

		if (sock >= 0) {
			rc = ra_recv(sock, r->ifindex, &ra);
			if (rc < 0) {
				if (got_term)
					break;
				xlog("%s: RA receive failed; no further "
				    "refreshes", r->ifname);
				(void)close(sock);
				sock = -1;
			}
		} else {
			struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

			(void)nanosleep(&ts, NULL);
			rc = 0;
		}

		now = now_sec();
		if (rc == 1)
			note_router(r, &ra, now);
		expire_routers(r, now);
		select_default(r, rc == 1 && r->have_default &&
		    IN6_ARE_ADDR_EQUAL(&ra.router_lladdr, &r->dflt));
	}

	if (sock >= 0)
		(void)close(sock);
	return (NULL);
}

struct ra_router *
ra_router_start(const char *ifname, const struct ra_info *ra,
    const struct in6_addr *addr)
{
	struct ra_router *r;
	int rc;

	r = calloc(1, sizeof(*r));
	if (r == NULL)
		return (NULL);
	(void)strlcpy(r->ifname, ifname, sizeof(r->ifname));
	r->ifindex = ra->router_scope_id;
	r->addr = *addr;
	r->prefix_len = ra->prefix_len;

	/*
	 * apply_ra_lease installed ::/0 via this RA's router. Seed the
	 * table with it; a zero lifetime leaves the table empty, so the
	 * first pass takes the route straight back out.
	 */
	r->dflt = ra->router_lladdr;
	r->have_default = true;
	note_router(r, ra, now_sec());

	r->pub = sc_publish_open("ipconfigd-ra");
	if (r->pub == NULL)
		xlog("%s: no configd session; router changes will not be "
		    "republished", ifname);

	rc = pthread_create(&r->tid, NULL, ra_router_main, r);
	if (rc != 0) {
		xlog("%s: pthread_create failed: %s", ifname, strerror(rc));
		if (r->pub != NULL)
			sc_publish_close(r->pub);
		free(r);
		return (NULL);
	}
	return (r);
}

void
ra_router_stop(struct ra_router *r, int remove_route)
{
	char buf[INET6_ADDRSTRLEN];

	if (r == NULL)
		return;
	r->stop = 1;
	(void)pthread_join(r->tid, NULL);

	if (remove_route && r->have_default &&
	    remove_v6_default_route(&r->dflt, r->ifindex) == 0)
		xlog("%s: removed ::/0 via %s", r->ifname,
		    addr_str(&r->dflt, buf, sizeof(buf)));

	if (r->pub != NULL)
		sc_publish_close(r->pub);
	free(r);
}
