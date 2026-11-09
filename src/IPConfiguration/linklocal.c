/*
 * linklocal.c — 169.254/16 address selection (RFC 3927, #426).
 *
 * See linklocal.h. The constants and the order of events are bootp-268.1
 * IPConfiguration.bproj/linklocal.c: linklocal_allocate() probes a random
 * address, or the one the service had, and after
 * MAX_LINKLOCAL_INITIAL_TRIES addresses in use waits
 * LINKLOCAL_RETRY_TIME_SECS before each further one.
 *
 * No SIOCARPIPLL: Darwin's ioctl only opens its ARP code's link-local
 * gate, and the kernel here already broadcasts ARP replies for link-local
 * addresses.
 */
#include "linklocal.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apply_lease.h"
#include "arp_probe.h"
#include "bound_state.h"	/* BOUND_MAX_IF */

#define	LINKLOCAL_RANGE_START	0xa9fe0000U	/* 169.254.0.0 */
#define	LINKLOCAL_RANGE_END	0xa9feffffU	/* 169.254.255.255 */
#define	LINKLOCAL_FIRST_USEABLE	(LINKLOCAL_RANGE_START + 256)	/* 169.254.1.0 */
#define	LINKLOCAL_LAST_USEABLE	(LINKLOCAL_RANGE_END - 256)	/* 169.254.254.255 */
#define	LINKLOCAL_RANGE		(LINKLOCAL_LAST_USEABLE + 1 - \
				    LINKLOCAL_FIRST_USEABLE)

#define	MAX_LINKLOCAL_INITIAL_TRIES	10
#define	LINKLOCAL_RETRY_TIME_SECS	30

/*
 * The address each interface last used, linklocal_get_address()'s per-
 * interface store on Darwin: it outlives the service, so a link flap keeps
 * the address.
 */
static pthread_mutex_t	g_ll_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	char		ifname[IF_NAMESIZE];
	struct in_addr	addr;
} g_ll_addrs[BOUND_MAX_IF];

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[linklocal] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static bool
addr_is_linklocal(struct in_addr a)
{
	return ((ntohl(a.s_addr) & LINKLOCAL_MASK) == LINKLOCAL_RANGE_START);
}

static time_t
monotonic_secs(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec);
}

static void
remember(const char *ifname, struct in_addr addr)
{
	int i, slot = -1;

	(void)pthread_mutex_lock(&g_ll_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_ll_addrs[i].ifname[0] == '\0') {
			if (slot < 0)
				slot = i;
		} else if (strcmp(g_ll_addrs[i].ifname, ifname) == 0) {
			slot = i;
			break;
		}
	}
	if (slot >= 0) {
		(void)strlcpy(g_ll_addrs[slot].ifname, ifname,
		    sizeof(g_ll_addrs[slot].ifname));
		g_ll_addrs[slot].addr = addr;
	}
	(void)pthread_mutex_unlock(&g_ll_lock);
}

void
linklocal_forget(const char *ifname)
{
	int i;

	(void)pthread_mutex_lock(&g_ll_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (strcmp(g_ll_addrs[i].ifname, ifname) == 0) {
			g_ll_addrs[i].ifname[0] = '\0';
			g_ll_addrs[i].addr.s_addr = 0;
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_ll_lock);
}

/* S_find_linklocal_address(): the remembered one, else one on the interface. */
static struct in_addr
find_previous(const char *ifname)
{
	struct in_addr found = { 0 };
	struct ifaddrs *ifa, *p;
	int i;

	(void)pthread_mutex_lock(&g_ll_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (strcmp(g_ll_addrs[i].ifname, ifname) == 0) {
			found = g_ll_addrs[i].addr;
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_ll_lock);
	if (found.s_addr != 0 || getifaddrs(&ifa) != 0)
		return (found);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_in *sin;

		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET ||
		    strcmp(p->ifa_name, ifname) != 0)
			continue;
		sin = (const struct sockaddr_in *)(const void *)p->ifa_addr;
		if (addr_is_linklocal(sin->sin_addr)) {
			found = sin->sin_addr;
			break;
		}
	}
	freeifaddrs(ifa);
	return (found);
}

void
linklocal_probe_init(struct linklocal_probe *lp, const char *ifname)
{
	(void)memset(lp, 0, sizeof(*lp));
	lp->candidate = find_previous(ifname);
}

int
linklocal_probe_next(struct linklocal_probe *lp, const char *ifname,
    struct in_addr *out)
{
	struct in_addr probe = lp->candidate;
	char a[INET_ADDRSTRLEN];
	time_t now = monotonic_secs();
	int rc;

	if (lp->retry_at > now)
		return (0);
	if (probe.s_addr == 0)
		probe.s_addr = htonl(LINKLOCAL_FIRST_USEABLE +
		    arc4random_uniform(LINKLOCAL_RANGE));

	rc = arp_probe_linklocal(ifname, probe);
	if (rc < 0) {
		lp->retry_at = monotonic_secs() + LINKLOCAL_RETRY_TIME_SECS;
		return (-1);
	}
	if (rc == 0) {
		remember(ifname, probe);
		*out = probe;
		return (1);
	}

	(void)inet_ntop(AF_INET, &probe, a, sizeof(a));
	xlog("%s: %s is in use", ifname, a);
	if (lp->candidate.s_addr != 0)
		linklocal_forget(ifname);
	lp->candidate.s_addr = 0;
	if (++lp->tries >= MAX_LINKLOCAL_INITIAL_TRIES) {
		xlog("%s: %d addresses in use — waiting %ds before the next",
		    ifname, lp->tries, LINKLOCAL_RETRY_TIME_SECS);
		lp->retry_at = monotonic_secs() + LINKLOCAL_RETRY_TIME_SECS;
	}
	return (0);
}

void
linklocal_lease(struct dhcp_lease *lease, struct in_addr addr)
{
	(void)memset(lease, 0, sizeof(*lease));
	lease->addr = addr;
	lease->netmask.s_addr = htonl(LINKLOCAL_MASK);
}

void
linklocal_remove_addresses(const char *ifname, struct in_addr keep)
{
	struct ifaddrs *ifa, *p;

	if (getifaddrs(&ifa) != 0)
		return;
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_in *sin;
		struct dhcp_lease lease;

		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET ||
		    strcmp(p->ifa_name, ifname) != 0)
			continue;
		sin = (const struct sockaddr_in *)(const void *)p->ifa_addr;
		if (!addr_is_linklocal(sin->sin_addr) ||
		    sin->sin_addr.s_addr == keep.s_addr)
			continue;
		linklocal_lease(&lease, sin->sin_addr);
		(void)deconfigure_lease(ifname, &lease);
	}
	freeifaddrs(ifa);
}
