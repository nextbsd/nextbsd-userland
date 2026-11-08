/*
 * apply_lease.c — install / remove a bound DHCPv4 lease.
 *
 * apply_lease() has two sub-steps, each independently fallible:
 *   1. SIOCAIFADDR to add the lease's address + netmask + derived
 *      broadcast to the interface (this is the FreeBSD-native
 *      "add an alias" path; the kernel also auto-installs the
 *      connected /N route to the LAN).
 *   2. PF_ROUTE RTM_ADD for the default route via lease->router.
 *
 * deconfigure_lease() undoes exactly those two (#39). It is best-effort
 * and idempotent by design: teardown races link-down, and when an
 * interface's link drops the kernel may already have torn the address
 * and route down for us. So EADDRNOTAVAIL / ESRCH / ENXIO mean "already
 * gone", which is the state we wanted, not a failure.
 *
 * DNS lives in resolv_conf_sync(), driven off the *primary* interface
 * rather than the calling one — see apply_lease.h.
 */
#include "apply_lease.h"
#include "arp_probe.h"
#include "dns_config.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_var.h>

#include <arpa/inet.h>

#include <SystemConfiguration/SCPrivate.h>	/* _SC_NOTIFY_NETWORK_CHANGE */

#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[apply] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static int
apply_address(const char *ifname, const struct dhcp_lease *lease)
{
	struct in_aliasreq req;
	int sock, rc;

	(void)memset(&req, 0, sizeof(req));
	(void)strlcpy(req.ifra_name, ifname, sizeof(req.ifra_name));

	req.ifra_addr.sin_family = AF_INET;
	req.ifra_addr.sin_len = sizeof(struct sockaddr_in);
	req.ifra_addr.sin_addr = lease->addr;

	req.ifra_mask.sin_family = AF_INET;
	req.ifra_mask.sin_len = sizeof(struct sockaddr_in);
	req.ifra_mask.sin_addr = lease->netmask;

	req.ifra_broadaddr.sin_family = AF_INET;
	req.ifra_broadaddr.sin_len = sizeof(struct sockaddr_in);
	/* Broadcast = (addr & netmask) | ~netmask. */
	req.ifra_broadaddr.sin_addr.s_addr =
	    (lease->addr.s_addr & lease->netmask.s_addr) |
	    ~lease->netmask.s_addr;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		xlog("socket(AF_INET) failed: %s", strerror(errno));
		return (-1);
	}
	rc = ioctl(sock, SIOCAIFADDR, &req);
	if (rc != 0)
		xlog("SIOCAIFADDR(%s) failed: %s", ifname,
		    strerror(errno));
	(void)close(sock);
	return (rc);
}

/*
 * Remove the lease address from the interface (SIOCDIFADDR). The
 * kernel drops the connected /N route with it.
 *
 * EADDRNOTAVAIL means the address is already off the interface — the
 * link went down and the kernel cleaned up before we got here. That is
 * the state we were trying to reach, so it is success, not failure.
 */
static int
remove_address(const char *ifname, const struct dhcp_lease *lease)
{
	struct ifreq ifr;
	struct sockaddr_in *sin;
	int sock, rc;

	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

	sin = (struct sockaddr_in *)(void *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	sin->sin_len = sizeof(*sin);
	sin->sin_addr = lease->addr;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		xlog("socket(AF_INET) failed: %s", strerror(errno));
		return (-1);
	}
	rc = ioctl(sock, SIOCDIFADDR, &ifr);
	if (rc != 0) {
		if (errno == EADDRNOTAVAIL || errno == ENXIO) {
			rc = 0;		/* already gone — fine */
		} else {
			xlog("SIOCDIFADDR(%s) failed: %s", ifname,
			    strerror(errno));
		}
	}
	(void)close(sock);
	return (rc);
}

/*
 * Add or delete the default route via lease->router on the PF_ROUTE
 * socket. `rtm_type` is RTM_ADD or RTM_DELETE — the message shape is
 * identical, which is why both paths share one function.
 *
 * Benign outcomes, treated as success:
 *   RTM_ADD    + EEXIST  — the route is already there.
 *   RTM_DELETE + ESRCH   — there is no such route to remove.
 *   either     + ENETUNREACH/ENXIO — the interface is already gone.
 */
static int
default_route(const struct dhcp_lease *lease, int rtm_type)
{
	struct {
		struct rt_msghdr	hdr;
		struct sockaddr_in	dst;
		struct sockaddr_in	gw;
		struct sockaddr_in	mask;
	} msg;
	int sock;
	ssize_t n;

	/*
	 * No router — a Manual service configured without one (#58), or a
	 * lease without option 3 — means no default route to add or take
	 * back.
	 */
	if (lease->router.s_addr == INADDR_ANY)
		return (0);

	(void)memset(&msg, 0, sizeof(msg));
	msg.hdr.rtm_msglen = sizeof(msg);
	msg.hdr.rtm_version = RTM_VERSION;
	msg.hdr.rtm_type = rtm_type;
	msg.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
	msg.hdr.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
	msg.hdr.rtm_pid = (int32_t)getpid();
	msg.hdr.rtm_seq = 1;

	msg.dst.sin_family = AF_INET;
	msg.dst.sin_len = sizeof(msg.dst);
	msg.dst.sin_addr.s_addr = 0;	/* 0.0.0.0 = default */

	msg.gw.sin_family = AF_INET;
	msg.gw.sin_len = sizeof(msg.gw);
	msg.gw.sin_addr = lease->router;

	msg.mask.sin_family = AF_INET;
	msg.mask.sin_len = sizeof(msg.mask);
	msg.mask.sin_addr.s_addr = 0;	/* /0 */

	sock = socket(AF_ROUTE, SOCK_RAW, 0);
	if (sock < 0) {
		xlog("socket(AF_ROUTE) failed: %s", strerror(errno));
		return (-1);
	}
	n = write(sock, &msg, sizeof(msg));
	if (n != (ssize_t)sizeof(msg)) {
		int e = errno;

		(void)close(sock);
		if ((rtm_type == RTM_ADD && e == EEXIST) ||
		    (rtm_type == RTM_DELETE && e == ESRCH) ||
		    e == ENETUNREACH || e == ENXIO)
			return (0);
		xlog("write(PF_ROUTE, %s default) failed: %s",
		    rtm_type == RTM_ADD ? "RTM_ADD" : "RTM_DELETE",
		    strerror(e));
		return (-1);
	}
	(void)close(sock);
	return (0);
}

/* Would `s` survive as one token on a resolv.conf line? */
static bool
resolv_token_ok(const char *s)
{
	if (*s == '\0')
		return (false);
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		if (c <= 0x20 || c >= 0x7f)
			return (false);
	}
	return (true);
}

/* "<keyword> a b c\n", skipping unusable tokens. Returns the count kept. */
static unsigned
resolv_print_list(FILE *fp, const char *keyword, const char *list,
    size_t stride, unsigned count)
{
	unsigned i, kept = 0;

	for (i = 0; i < count; i++) {
		const char *s = list + i * stride;

		if (!resolv_token_ok(s))
			continue;
		if (kept++ == 0)
			(void)fputs(keyword, fp);
		(void)fprintf(fp, " %s", s);
	}
	if (kept > 0)
		(void)fputc('\n', fp);
	return (kept);
}

/*
 * A link-local server needs its scope to be usable: libc parses
 * nameservers through getaddrinfo(AI_NUMERICHOST). Setup's already have
 * one; State's do not, since dnsinfo's if_index carries it on Darwin.
 */
static void
resolv_print_server(FILE *fp, const char *server, const char *ifname)
{
	struct in6_addr v6;

	if (strchr(server, '%') == NULL &&
	    inet_pton(AF_INET6, server, &v6) == 1 &&
	    (IN6_IS_ADDR_LINKLOCAL(&v6) || IN6_IS_ADDR_MC_LINKLOCAL(&v6)))
		(void)fprintf(fp, "nameserver %s%%%s\n", server, ifname);
	else
		(void)fprintf(fp, "nameserver %s\n", server);
}

static void
resolv_post_change(void)
{
	/* New resolvers: tell notify(3) watchers, as IPMonitor does. */
	if (notify_post(_SC_NOTIFY_NETWORK_CHANGE) != NOTIFY_STATUS_OK)
		xlog("resolv.conf: notify_post(" _SC_NOTIFY_NETWORK_CHANGE
		    ") failed");
}

/*
 * Atomically replace /etc/resolv.conf with the merged DNS of the
 * *primary* service (#41, #425), or remove it when the primary has none.
 *
 * Per-interface writers once meant the last NIC to bind owned global
 * DNS, and a bare fopen("w") could expose a truncated file (#40), so the
 * caller passes only the primary's DNS and the write goes through a
 * fsync'd temporary. Darwin's set_dns() writes the same lines to
 * /var/run/resolv.conf; here libc and mDNSResponder read /etc directly.
 * A rewrite or removal posts network_change (#339).
 *
 * All of `dns`'s servers are written even past libc's limit of three,
 * as set_dns() does; libc reads the ones it can use.
 */
void
resolv_conf_sync(const char *ifname, const struct ipconfig_dns *dns)
{
	static const char path[] = "/etc/resolv.conf";
	static const char tmpl[] = "/etc/.resolv.conf.XXXXXX";
	char tmp[sizeof(tmpl)];
	unsigned i;
	FILE *fp;
	int fd;

	if (dns == NULL) {
		if (unlink(path) == 0) {
			xlog("resolv.conf: primary has no DNS — removed");
			resolv_post_change();
		} else if (errno != ENOENT) {
			xlog("resolv.conf: unlink failed: %s",
			    strerror(errno));
		}
		return;
	}

	(void)memcpy(tmp, tmpl, sizeof(tmpl));
	fd = mkstemp(tmp);
	if (fd < 0) {
		xlog("mkstemp(%s) failed: %s", tmpl, strerror(errno));
		return;
	}
	fp = fdopen(fd, "w");
	if (fp == NULL) {
		xlog("fdopen failed: %s", strerror(errno));
		(void)close(fd);
		(void)unlink(tmp);
		return;
	}

	(void)fprintf(fp, "#\n# NextBSD Notice\n#\n"
	    "# This file is automatically generated.\n#\n");
	if (resolv_print_list(fp, "search", dns->search[0],
	    IPCFG_DNS_NAME_LEN, dns->search_count) == 0 &&
	    resolv_token_ok(dns->domain))
		(void)fprintf(fp, "domain %s\n", dns->domain);
	for (i = 0; i < dns->server_count; i++)
		resolv_print_server(fp, dns->servers[i], ifname);
	(void)resolv_print_list(fp, "sortlist", dns->sortlist[0],
	    IPCFG_DNS_SORT_LEN, dns->sort_count);

	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
		xlog("resolv.conf: flush/fsync failed: %s", strerror(errno));
		(void)fclose(fp);
		(void)unlink(tmp);
		return;
	}
	(void)fclose(fp);

	/* 0644 — mkstemp made it 0600, and resolv.conf is world-readable. */
	if (chmod(tmp, 0644) != 0)
		xlog("resolv.conf: chmod failed: %s (continuing)",
		    strerror(errno));

	if (rename(tmp, path) != 0) {
		xlog("resolv.conf: rename failed: %s", strerror(errno));
		(void)unlink(tmp);
		return;
	}
	xlog("resolv.conf: %u nameserver(s) from primary %s",
	    dns->server_count, ifname);
	resolv_post_change();
}

int
apply_lease(const char *ifname, const struct dhcp_lease *lease)
{
	int rc = 0;

	if (apply_address(ifname, lease) != 0)
		rc = -1;
	else
		/* RFC 5227 §2.3 gratuitous-ARP announce. Best-effort —
		 * the address is already on the interface, the announce
		 * is purely a peer ARP-cache update. iter 6 does not
		 * gate apply_lease on it. */
		(void)arp_announce(ifname, lease->addr);
	if (default_route(lease, RTM_ADD) != 0)
		rc = -1;
	/* DNS is not per-interface — the caller runs dns_config_update()
	 * once the binding is in bound_state and primary is settled. */
	return (rc);
}

int
deconfigure_lease(const char *ifname, const struct dhcp_lease *lease)
{
	int rc = 0;

	/*
	 * Route first, then address. The other order works too, but
	 * removing the address drops the connected route the default
	 * route's gateway is reached through, and some kernels then
	 * refuse the RTM_DELETE with ESRCH — which we would have to
	 * treat as benign anyway. Doing it this way keeps the common
	 * path free of "expected" errors.
	 */
	if (default_route(lease, RTM_DELETE) != 0)
		rc = -1;
	if (remove_address(ifname, lease) != 0)
		rc = -1;

	xlog("deconfigured %s%s", ifname, rc == 0 ? "" : " (with errors)");
	return (rc);
}
