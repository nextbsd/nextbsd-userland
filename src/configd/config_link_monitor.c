/*
 * config_link_monitor.c — in-process KernelEventMonitor (#168 follow-up, #257).
 *
 * Apple's KernelEventMonitor is a configd plugin sharing configd's event loop;
 * this port previously ran it as a standalone daemon (src/KernelEventMonitor,
 * PR #167) because a stock kernel had no EVFILT_MACHPORT and configd's single
 * mach_msg loop couldn't multiplex a Mach port and the PF_ROUTE socket. Now
 * that native EVFILT_MACHPORT delivery works (#168 Stages 0-4) the daemon is
 * redundant, so the route->Link translation moves in-process here, retiring the
 * standalone daemon + its launchd plist.
 *
 * Job: read the PF_ROUTE socket and publish Apple's canonical key
 *   State:/Network/Interface/<ifname>/Link = { Active : <bool> }
 * so ipconfigd (sc_link_watch) starts DHCP when a NIC links up.
 *
 * Two route message types matter (#42):
 *
 *   RTM_IFINFO      — an existing interface's link state changed. Republish
 *                     its Link key.
 *   RTM_IFANNOUNCE  — an interface arrived or departed. This used to be
 *                     ignored entirely, and both halves of that were bugs:
 *
 *                     * An interface created at RUNTIME was invisible until it
 *                       happened to generate its first link-state transition.
 *                       It had no Link key at all, so nothing could watch for
 *                       it. That is exactly the shape of a net80211 VAP —
 *                       `ifconfig wlan0 create wlandev iwlwifi0` — which is why
 *                       wland would otherwise have to run its own duplicate
 *                       PF_ROUTE listener just to see the interfaces it creates
 *                       itself.
 *                     * A DESTROYED interface left its Link key in the store
 *                       forever, so consumers kept seeing a dead wlan0 as a
 *                       live, Active interface. Departures now remove the key.
 *
 * Mechanism: a dedicated thread inside configd. Rather than reach into the
 * store directly (which would need a lock around config_store / config_session,
 * both written for configd's single serve thread), it publishes the way every
 * other client does — as a configd session over the MIG config.defs client
 * stubs (configUser.c, already linked for --selftest). The store write therefore
 * happens on configd's serve thread, fully serialized; this thread only reads
 * the route socket and issues RPCs to configd's own service. The value is the
 * same XML-plist byte blob SCDynamicStoreSetValue would send (configd stores it
 * opaque; ipconfigd's SCDynamicStoreCopyValue parses it back), and the key is
 * the raw UTF-8 string _SCSerializeString produces — so the published key/value
 * are byte-for-byte what an external SC client would have written.
 *
 * Because the monitor lives in configd's own address space, the session never
 * sees a "configd restarted" dead-server error (they share a lifetime), so the
 * standalone daemon's reopen/re-seed dance is unnecessary.
 *
 * Addresses (#338): RTM_NEWADDR / RTM_DELADDR rebuild the affected interface's
 *   State:/Network/Interface/<ifname>/IPv4
 *   State:/Network/Interface/<ifname>/IPv6
 * in the shape Darwin's KernelEventMonitor gives them (ev_ipv4.c / ev_ipv6.c),
 * from getifaddrs plus SIOCGIFAFLAG_IN6. See publish_ip(). Each change is
 * followed by notify_post(com.apple.system.config.network_change) (#339).
 *
 * Kernel events (#346): the primary source is now Darwin's own — a PF_SYSTEM
 * SYSPROTO_EVENT socket filtered to KEV_NETWORK_CLASS, opened and read the way
 * KernelEventMonitor's eventmon.c does, with the KEV_DL_* / KEV_INET_* /
 * KEV_INET6_* codes mapped onto the same publish functions as ev_dlil.c /
 * ev_ipv4.c / ev_ipv6.c map them. See kev_message(). If the kernel has no
 * PF_SYSTEM (socket() fails), the PF_ROUTE path runs exactly as before;
 * that fallback is quiet, a line in configd's log only.
 *
 * The kernel posts KEV_DL_LINK_ON / LINK_OFF only for a known link state, so
 * the IFF_UP fallback link_active() applies to LINK_STATE_UNKNOWN interfaces
 * needs KEV_DL_SIFFLAGS, which older kernels do not post. Until one is seen,
 * PF_ROUTE stays open beside the event socket for just that case (RTM_IFINFO
 * on a LINK_STATE_UNKNOWN interface); the first KEV_DL_SIFFLAGS closes it.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>

#include <net/if.h>
#include <net/net_kev.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>

#include <arpa/inet.h>

#include <errno.h>
#include <ifaddrs.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <notify.h>
#include <SystemConfiguration/SCPrivate.h>	/* _SC_NOTIFY_NETWORK_CHANGE */

#include "config_types.h"		/* xmlData_t, SCD_SERVER, kSCStatusOK */
#include "config_link_monitor.h"

/* config.defs MIG client stubs (configUser.c). */
extern kern_return_t configopen(mach_port_t, xmlData_t,
    mach_msg_type_number_t, xmlData_t, mach_msg_type_number_t, mach_port_t *,
    int *);
extern kern_return_t configset(mach_port_t, xmlData_t, mach_msg_type_number_t,
    xmlData_t, mach_msg_type_number_t, int, int *, int *);
extern kern_return_t configremove(mach_port_t, xmlData_t,
    mach_msg_type_number_t, int *);

/* liblaunch global — the bootstrap port configd checked its service in on. */
extern mach_port_t bootstrap_port;

/* The configd session this monitor publishes through (monitor thread only). */
static mach_port_t g_session = MACH_PORT_NULL;

static void
lmlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "configd[linkmon] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Map a (link_state, if_flags) pair to Apple's "Active" boolean — verbatim
 * from the standalone daemon. LINK_STATE_UP is yes; LINK_STATE_DOWN is no;
 * LINK_STATE_UNKNOWN (media-status-less NICs) falls back to IFF_UP.
 */
static int
link_active(int link_state, int if_flags)
{
	if (link_state == LINK_STATE_UP)
		return (1);
	if (link_state == LINK_STATE_DOWN)
		return (0);
	return ((if_flags & IFF_UP) != 0);	/* LINK_STATE_UNKNOWN */
}

/*
 * The XML-plist value byte blob for { Active : <bool> } — exactly the form
 * CFPropertyListCreateData(kCFPropertyListXMLFormat_v1_0) produces and that
 * configd stores opaque + ipconfigd's SCDynamicStoreCopyValue parses back.
 */
static const char *
link_value_xml(int active)
{
	static const char xml_true[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict>\n\t<key>Active</key>\n\t<true/>\n</dict>\n</plist>\n";
	static const char xml_false[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict>\n\t<key>Active</key>\n\t<false/>\n</dict>\n</plist>\n";

	return (active ? xml_true : xml_false);
}

/*
 * Publish State:/Network/Interface/<ifname>/Link = { Active:bool } via the
 * configd session. The key is the raw UTF-8 string (what _SCSerializeString
 * yields); the value is the XML-plist blob. Loopback never DHCPs, so skip it.
 */
static void
publish_link(const char *ifname, int active)
{
	char		key[128];
	const char	*val;
	int		keylen, vallen;
	int		newInstance = 0, status = 0;
	kern_return_t	kr;

	if (strcmp(ifname, "lo0") == 0)
		return;
	if (g_session == MACH_PORT_NULL)
		return;

	keylen = snprintf(key, sizeof(key),
	    "State:/Network/Interface/%s/Link", ifname);
	if (keylen <= 0 || (size_t)keylen >= sizeof(key))
		return;
	val = link_value_xml(active);
	vallen = (int)strlen(val);

	kr = configset(g_session, key, (mach_msg_type_number_t)keylen, val,
	    (mach_msg_type_number_t)vallen, 0, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		lmlog("KEM-LINK-FAIL: configset(%s) kr=0x%x status=%d",
		    ifname, (unsigned)kr, status);
		return;
	}
	/* KEM-LINK-OK gates in boot-test.sh: a link-state change reached the
	 * store — the signal ipconfigd's watch depends on. */
	lmlog("KEM-LINK-OK: %s Active=%d", ifname, active);
}

/*
 * Remove State:/Network/Interface/<ifname>/Link (#42).
 *
 * An interface that goes away must not leave a Link key behind. Before this,
 * destroying a VAP (`ifconfig wlan0 destroy`) left `wlan0` in the store
 * looking permanently Active, and every consumer — ipconfigd's link watch
 * included — kept believing in an interface that no longer existed.
 */
static void
unpublish_link(const char *ifname)
{
	char		key[128];
	int		keylen, status = 0;
	kern_return_t	kr;

	if (strcmp(ifname, "lo0") == 0)
		return;
	if (g_session == MACH_PORT_NULL)
		return;

	keylen = snprintf(key, sizeof(key),
	    "State:/Network/Interface/%s/Link", ifname);
	if (keylen <= 0 || (size_t)keylen >= sizeof(key))
		return;

	kr = configremove(g_session, key, (mach_msg_type_number_t)keylen,
	    &status);
	/*
	 * kSCStatusNoKey is the expected outcome for an interface that never
	 * had a Link key (it departed before we ever saw it link), so it is not
	 * worth logging as a failure.
	 */
	if (kr != KERN_SUCCESS) {
		lmlog("KEM-LINK-FAIL: configremove(%s) kr=0x%x status=%d",
		    ifname, (unsigned)kr, status);
		return;
	}
	lmlog("KEM-LINK-GONE: %s", ifname);
}

/*
 * IPv4 / IPv6 address keys (#338) — Darwin's ev_ipv4.c / ev_ipv6.c.
 *
 * Per interface and family, Darwin's KernelEventMonitor publishes one
 * dictionary of parallel arrays, slot i describing the i-th address:
 *
 *   IPv4 = { Addresses, SubnetMasks,
 *            BroadcastAddresses (IFF_BROADCAST) | DestAddresses (p2p) }
 *   IPv6 = { Addresses, PrefixLength, Flags, ScopeID, DestAddresses (p2p) }
 *
 * and removes the key once the family has no addresses left. configd has no
 * CoreFoundation, so the XML plist is written by hand, keys in the sorted
 * order CFPropertyListCreateData emits. MAX_IFADDRS keeps a value well under
 * IPKEY_VALUE_MAX (8 KiB).
 */
#define MAX_IFADDRS	32
#define IPKEY_VALUE_MAX	8192
#define PLIST_HEADER \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
	"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" " \
	"\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" \
	"<plist version=\"1.0\">\n"

struct ip_entry {
	char		addr[INET6_ADDRSTRLEN];
	char		mask[INET_ADDRSTRLEN];	/* IPv4 */
	char		brd[INET_ADDRSTRLEN];	/* IPv4, IFF_BROADCAST */
	char		dst[INET6_ADDRSTRLEN];	/* IFF_POINTOPOINT */
	int		prefixlen;		/* IPv6 */
	int		flags6;			/* IPv6 */
	uint32_t	scope_id;		/* IPv6 */
};

/* A bounded XML-plist buffer; `overflow` latches once anything is cut. */
struct xbuf {
	char	data[IPKEY_VALUE_MAX];
	size_t	len;
	int	overflow;
};

static void xb_printf(struct xbuf *, const char *, ...) __printflike(2, 3);

static void
xb_printf(struct xbuf *xb, const char *fmt, ...)
{
	va_list	ap;
	size_t	room;
	int	n;

	if (xb->overflow)
		return;
	room = sizeof(xb->data) - xb->len;
	va_start(ap, fmt);
	n = vsnprintf(xb->data + xb->len, room, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= room) {
		xb->overflow = 1;
		return;
	}
	xb->len += (size_t)n;
}

/* Field selectors for xb_array(). */
enum ip_field { F_ADDR, F_MASK, F_BRD, F_DST, F_PREFIX, F_FLAGS, F_SCOPE };

/* <key>name</key> + an <array> of field `f` across all `n` entries. */
static void
xb_array(struct xbuf *xb, const char *name, const struct ip_entry *e, int n,
    enum ip_field f)
{
	int i;

	xb_printf(xb, "\t<key>%s</key>\n\t<array>\n", name);
	for (i = 0; i < n; i++) {
		switch (f) {
		case F_ADDR:
			xb_printf(xb, "\t\t<string>%s</string>\n", e[i].addr);
			break;
		case F_MASK:
			xb_printf(xb, "\t\t<string>%s</string>\n", e[i].mask);
			break;
		case F_BRD:
			xb_printf(xb, "\t\t<string>%s</string>\n", e[i].brd);
			break;
		case F_DST:
			xb_printf(xb, "\t\t<string>%s</string>\n", e[i].dst);
			break;
		case F_PREFIX:
			xb_printf(xb, "\t\t<integer>%d</integer>\n",
			    e[i].prefixlen);
			break;
		case F_FLAGS:
			xb_printf(xb, "\t\t<integer>%d</integer>\n",
			    e[i].flags6);
			break;
		case F_SCOPE:
			xb_printf(xb, "\t\t<integer>%u</integer>\n",
			    (unsigned)e[i].scope_id);
			break;
		}
	}
	xb_printf(xb, "%s", "\t</array>\n");
}

/*
 * What this monitor last wrote to each IPv4 / IPv6 key. Darwin's
 * KernelEventMonitor only writes a key whose value actually changed (its
 * cache_SCDynamicStore* layer), so an RTM_NEWADDR that leaves an interface's
 * addresses as they were — or the startup seed — does not wake every watcher.
 * Monitor thread only.
 */
struct ip_cache {
	char	key[128];
	char	*val;		/* NULL: key not published */
	size_t	len;
};
static struct ip_cache	*g_ipcache;
static size_t		g_ipcache_n;

/* An IPv4 / IPv6 key was written since the last network_change post. */
static int		g_ip_changed;

static struct ip_cache *
ip_cache_lookup(const char *key)
{
	struct ip_cache	*c;
	size_t		i;

	for (i = 0; i < g_ipcache_n; i++)
		if (strcmp(g_ipcache[i].key, key) == 0)
			return (&g_ipcache[i]);
	c = realloc(g_ipcache, (g_ipcache_n + 1) * sizeof(*g_ipcache));
	if (c == NULL)
		return (NULL);
	g_ipcache = c;
	c = &g_ipcache[g_ipcache_n++];
	(void)strlcpy(c->key, key, sizeof(c->key));
	c->val = NULL;
	c->len = 0;
	return (c);
}

/*
 * Set (val != NULL) or remove (val == NULL) one IPv4 / IPv6 key, skipping the
 * RPC when the store already holds exactly that.
 */
static void
ip_key_update(const char *key, const char *what, const char *val, size_t len)
{
	struct ip_cache	*c;
	int		newInstance = 0, status = 0;
	kern_return_t	kr;

	c = ip_cache_lookup(key);
	if (c != NULL) {
		if (val == NULL && c->val == NULL)
			return;
		if (val != NULL && c->val != NULL && c->len == len &&
		    memcmp(c->val, val, len) == 0)
			return;
	}

	if (val != NULL) {
		kr = configset(g_session, key, (mach_msg_type_number_t)
		    strlen(key), val, (mach_msg_type_number_t)len, 0,
		    &newInstance, &status);
		if (kr != KERN_SUCCESS || status != kSCStatusOK) {
			lmlog("KEM-ADDR-FAIL: configset(%s) kr=0x%x status=%d",
			    what, (unsigned)kr, status);
			return;
		}
	} else {
		kr = configremove(g_session, key, (mach_msg_type_number_t)
		    strlen(key), &status);
		if (kr != KERN_SUCCESS) {
			lmlog("KEM-ADDR-FAIL: configremove(%s) kr=0x%x "
			    "status=%d", what, (unsigned)kr, status);
			return;
		}
	}

	if (c != NULL) {
		free(c->val);
		c->val = NULL;
		c->len = 0;
		if (val != NULL && (c->val = malloc(len)) != NULL) {
			(void)memcpy(c->val, val, len);
			c->len = len;
		}
	}
	g_ip_changed = 1;
	lmlog("KEM-ADDR: %s %s", what, val != NULL ? "updated" : "removed");
}

/*
 * Tell notify(3) watchers (syslogd, hostnamed) the network changed, as
 * Darwin's KernelEventMonitor does after an address change (eventmon.c).
 * Only posts when an IPv4 / IPv6 key actually changed, unless `force`.
 */
static void
post_network_change(int force)
{
	uint32_t status;

	if (!force && !g_ip_changed)
		return;
	g_ip_changed = 0;
	status = notify_post(_SC_NOTIFY_NETWORK_CHANGE);
	if (status != NOTIFY_STATUS_OK)
		lmlog("KEM-FAIL: notify_post(" _SC_NOTIFY_NETWORK_CHANGE
		    ") status=%u", status);
}

/* Prefix length of an IPv6 netmask (contiguous leading one bits). */
static int
prefix_len6(const struct sockaddr *sa)
{
	const struct sockaddr_in6	*mask;
	int				i, bits = 0;
	uint8_t				b;

	if (sa == NULL || sa->sa_family != AF_INET6)
		return (128);
	mask = (const struct sockaddr_in6 *)(const void *)sa;
	for (i = 0; i < 16; i++) {
		for (b = mask->sin6_addr.s6_addr[i]; b & 0x80; b <<= 1)
			bits++;
		if (mask->sin6_addr.s6_addr[i] != 0xff)
			break;
	}
	return (bits);
}

static void
ntop4(const struct sockaddr *sa, char *buf, size_t len)
{
	buf[0] = '\0';
	if (sa == NULL || sa->sa_family != AF_INET)
		return;
	(void)inet_ntop(AF_INET,
	    &((const struct sockaddr_in *)(const void *)sa)->sin_addr,
	    buf, (socklen_t)len);
}

/*
 * A link-local (or multicast link-local) address from getifaddrs carries the
 * KAME embedded scope in s6_addr16[1]. Darwin's ev_ipv6.c moves it into
 * sin6_scope_id and clears it, so Addresses holds the plain "fe80::..." form
 * and the zone travels in ScopeID.
 */
static void
unembed_scope6(struct sockaddr_in6 *sin6)
{
	uint16_t idx;

	if (!IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) &&
	    !IN6_IS_ADDR_MC_LINKLOCAL(&sin6->sin6_addr))
		return;
	idx = (uint16_t)((sin6->sin6_addr.s6_addr[2] << 8) |
	    sin6->sin6_addr.s6_addr[3]);
	if (idx == 0)
		return;
	sin6->sin6_addr.s6_addr[2] = 0;
	sin6->sin6_addr.s6_addr[3] = 0;
	if (sin6->sin6_scope_id == 0)
		sin6->sin6_scope_id = idx;
}

/* Rebuild State:/Network/Interface/<ifname>/IPv4 from `list` (ev_ipv4.c). */
static void
publish_ipv4(const struct ifaddrs *list, const char *ifname)
{
	static struct xbuf	xb;
	struct ip_entry		e[MAX_IFADDRS];
	const struct ifaddrs	*p;
	char			key[128], what[64];
	int			n = 0, nbrd = 0, ndst = 0;

	(void)memset(e, 0, sizeof(e));
	for (p = list; p != NULL && n < MAX_IFADDRS; p = p->ifa_next) {
		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET ||
		    strcmp(p->ifa_name, ifname) != 0)
			continue;
		ntop4(p->ifa_addr, e[n].addr, sizeof(e[n].addr));
		ntop4(p->ifa_netmask, e[n].mask, sizeof(e[n].mask));
		if (e[n].mask[0] == '\0')
			(void)strlcpy(e[n].mask, "0.0.0.0", sizeof(e[n].mask));
		if ((p->ifa_flags & IFF_POINTOPOINT) != 0) {
			ntop4(p->ifa_dstaddr, e[n].dst, sizeof(e[n].dst));
			if (e[n].dst[0] != '\0')
				ndst++;
		} else if ((p->ifa_flags & IFF_BROADCAST) != 0) {
			ntop4(p->ifa_broadaddr, e[n].brd, sizeof(e[n].brd));
			if (e[n].brd[0] != '\0')
				nbrd++;
		}
		n++;
	}

	(void)snprintf(key, sizeof(key),
	    "State:/Network/Interface/%s/IPv4", ifname);
	(void)snprintf(what, sizeof(what), "%s IPv4", ifname);
	if (n == 0) {
		ip_key_update(key, what, NULL, 0);
		return;
	}

	xb.len = 0;
	xb.overflow = 0;
	xb_printf(&xb, "%s", PLIST_HEADER "<dict>\n");
	xb_array(&xb, "Addresses", e, n, F_ADDR);
	/* Keep the arrays parallel: emit only when every slot has one. */
	if (nbrd == n)
		xb_array(&xb, "BroadcastAddresses", e, n, F_BRD);
	if (ndst == n)
		xb_array(&xb, "DestAddresses", e, n, F_DST);
	xb_array(&xb, "SubnetMasks", e, n, F_MASK);
	xb_printf(&xb, "%s", "</dict>\n</plist>\n");
	if (xb.overflow) {
		lmlog("KEM-ADDR-FAIL: %s value exceeds %d bytes", what,
		    IPKEY_VALUE_MAX);
		return;
	}
	ip_key_update(key, what, xb.data, xb.len);
}

/* Rebuild State:/Network/Interface/<ifname>/IPv6 from `list` (ev_ipv6.c). */
static void
publish_ipv6(const struct ifaddrs *list, const char *ifname)
{
	static struct xbuf	xb;
	struct ip_entry		e[MAX_IFADDRS];
	struct in6_ifreq	ifr6;
	struct sockaddr_in6	sin6;
	const struct ifaddrs	*p;
	char			key[128], what[64];
	int			n = 0, ndst = 0, s;

	s = socket(AF_INET6, SOCK_DGRAM, 0);
	(void)memset(e, 0, sizeof(e));
	for (p = list; p != NULL && n < MAX_IFADDRS; p = p->ifa_next) {
		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_INET6 ||
		    strcmp(p->ifa_name, ifname) != 0)
			continue;
		(void)memcpy(&sin6, p->ifa_addr, sizeof(sin6));
		unembed_scope6(&sin6);
		(void)inet_ntop(AF_INET6, &sin6.sin6_addr, e[n].addr,
		    sizeof(e[n].addr));
		e[n].scope_id = sin6.sin6_scope_id;
		e[n].prefixlen = prefix_len6(p->ifa_netmask);

		/* Per-address flags (ev_ipv6.c reads them the same way). */
		if (s >= 0) {
			(void)memset(&ifr6, 0, sizeof(ifr6));
			(void)strlcpy(ifr6.ifr_name, ifname,
			    sizeof(ifr6.ifr_name));
			ifr6.ifr_addr = sin6;
			if (ioctl(s, SIOCGIFAFLAG_IN6, &ifr6) == 0)
				e[n].flags6 = ifr6.ifr_ifru.ifru_flags6;
		}

		if ((p->ifa_flags & IFF_POINTOPOINT) != 0 &&
		    p->ifa_dstaddr != NULL &&
		    p->ifa_dstaddr->sa_family == AF_INET6) {
			(void)memcpy(&sin6, p->ifa_dstaddr, sizeof(sin6));
			unembed_scope6(&sin6);
			(void)inet_ntop(AF_INET6, &sin6.sin6_addr, e[n].dst,
			    sizeof(e[n].dst));
			ndst++;
		}
		n++;
	}
	if (s >= 0)
		(void)close(s);

	(void)snprintf(key, sizeof(key),
	    "State:/Network/Interface/%s/IPv6", ifname);
	(void)snprintf(what, sizeof(what), "%s IPv6", ifname);
	if (n == 0) {
		ip_key_update(key, what, NULL, 0);
		return;
	}

	xb.len = 0;
	xb.overflow = 0;
	xb_printf(&xb, "%s", PLIST_HEADER "<dict>\n");
	xb_array(&xb, "Addresses", e, n, F_ADDR);
	if (ndst == n)
		xb_array(&xb, "DestAddresses", e, n, F_DST);
	xb_array(&xb, "Flags", e, n, F_FLAGS);
	xb_array(&xb, "PrefixLength", e, n, F_PREFIX);
	xb_array(&xb, "ScopeID", e, n, F_SCOPE);
	xb_printf(&xb, "%s", "</dict>\n</plist>\n");
	if (xb.overflow) {
		lmlog("KEM-ADDR-FAIL: %s value exceeds %d bytes", what,
		    IPKEY_VALUE_MAX);
		return;
	}
	ip_key_update(key, what, xb.data, xb.len);
}

/*
 * Bring one interface's IPv4 and IPv6 keys in line with the kernel. The
 * reusable entry point: PF_ROUTE's RTM_NEWADDR / RTM_DELADDR call it today,
 * and a kernel-event source (#346) can call it for KEV_INET_* / KEV_INET6_*
 * without caring what woke it.
 */
static void
publish_ip(const char *ifname)
{
	struct ifaddrs *ifa;

	if (g_session == MACH_PORT_NULL)
		return;
	if (getifaddrs(&ifa) != 0) {
		lmlog("getifaddrs failed: %s", strerror(errno));
		return;
	}
	publish_ipv4(ifa, ifname);
	publish_ipv6(ifa, ifname);
	freeifaddrs(ifa);
	post_network_change(0);
}

/* A departed interface takes its IPv4 / IPv6 keys with it. */
static void
unpublish_ip(const char *ifname)
{
	char key[128], what[64];

	if (g_session == MACH_PORT_NULL)
		return;
	(void)snprintf(key, sizeof(key),
	    "State:/Network/Interface/%s/IPv4", ifname);
	(void)snprintf(what, sizeof(what), "%s IPv4", ifname);
	ip_key_update(key, what, NULL, 0);
	(void)snprintf(key, sizeof(key),
	    "State:/Network/Interface/%s/IPv6", ifname);
	(void)snprintf(what, sizeof(what), "%s IPv6", ifname);
	ip_key_update(key, what, NULL, 0);
	post_network_change(0);
}

/*
 * Publish one interface's *current* link state, looked up via getifaddrs.
 *
 * Needed for RTM_IFANNOUNCE / IFAN_ARRIVAL: unlike RTM_IFINFO, an announce
 * message carries no `struct if_data`, so there is no link state in the message
 * itself — it only tells us the interface now exists. We go and read it.
 */
static void
publish_link_current(const char *ifname)
{
	struct ifaddrs *ifa, *p;

	if (getifaddrs(&ifa) != 0) {
		lmlog("getifaddrs failed: %s", strerror(errno));
		return;
	}
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct if_data *ifd;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK ||
		    p->ifa_data == NULL ||
		    strcmp(p->ifa_name, ifname) != 0)
			continue;
		ifd = (const struct if_data *)p->ifa_data;
		publish_link(ifname,
		    link_active(ifd->ifi_link_state, (int)p->ifa_flags));
		break;
	}
	freeifaddrs(ifa);
}

/*
 * Publish the current link state of every interface (startup snapshot, so a
 * watcher already up before any RTM_IFINFO still sees the state). The AF_LINK
 * ifaddr's ifa_data is a `struct if_data *` carrying ifi_link_state. Each
 * interface's IPv4 / IPv6 keys are seeded from the same snapshot (#338),
 * then com.apple.system.config.network_change is posted once (#339).
 */
static void
publish_all(void)
{
	struct ifaddrs *ifa, *p;

	if (getifaddrs(&ifa) != 0) {
		lmlog("getifaddrs failed: %s", strerror(errno));
		return;
	}
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct if_data *ifd;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK ||
		    p->ifa_data == NULL)
			continue;
		ifd = (const struct if_data *)p->ifa_data;
		publish_link(p->ifa_name,
		    link_active(ifd->ifi_link_state, (int)p->ifa_flags));
		publish_ipv4(ifa, p->ifa_name);
		publish_ipv6(ifa, p->ifa_name);
	}
	freeifaddrs(ifa);
	/* Once after the snapshot, so early watchers re-read the state. */
	post_network_change(1);
}

/*
 * Open the configd session this monitor publishes through. Looks the configd
 * service up via the bootstrap port (configd has already checked it in by the
 * time the monitor starts) and issues configopen, retrying so a momentary race
 * with the serve loop reaching its first receive is harmless.
 */
static int
open_session(void)
{
	mach_port_t	server = MACH_PORT_NULL;
	const char	name[] = "KernelEventMonitor";
	int		status = 0;
	int		tries;

	for (tries = 0; tries < 120; tries++) {
		kern_return_t kr;

		if (server == MACH_PORT_NULL) {
			kr = bootstrap_look_up(bootstrap_port, SCD_SERVER,
			    &server);
			if (kr != KERN_SUCCESS || server == MACH_PORT_NULL) {
				(void)sleep(1);
				continue;
			}
		}
		kr = configopen(server, name,
		    (mach_msg_type_number_t)(sizeof(name) - 1), NULL, 0,
		    &g_session, &status);
		if (kr == KERN_SUCCESS && status == kSCStatusOK &&
		    g_session != MACH_PORT_NULL)
			return (0);
		(void)sleep(1);
	}
	lmlog("KEM-FAIL: could not open a configd session after 120s");
	return (-1);
}

/*
 * One PF_ROUTE message. With `companion` set the kernel-event socket is the
 * primary source and this only covers what it cannot yet: RTM_IFINFO for a
 * LINK_STATE_UNKNOWN interface, whose Active follows IFF_UP (see the comment
 * at the top of the file).
 */
static void
route_message(const char *buf, size_t n, int companion)
{
	const struct rt_msghdr		*rtm;
	const struct if_msghdr		*ifm;
	const struct if_announcemsghdr	*ifan;
	char				ifname[IFNAMSIZ];

	if (n < sizeof(struct rt_msghdr))
		return;
	rtm = (const struct rt_msghdr *)(const void *)buf;
	if (rtm->rtm_version != RTM_VERSION)
		return;

	/*
	 * Interface arrived or departed (#42). This is how a VAP cloned
	 * at runtime — `ifconfig wlan0 create wlandev iwlwifi0` — first
	 * becomes visible in the store, and how a destroyed one stops
	 * being visible. Note an announce carries no if_data, so on
	 * arrival we go read the interface's real link state.
	 */
	if (rtm->rtm_type == RTM_IFANNOUNCE) {
		if (companion || n < sizeof(struct if_announcemsghdr))
			return;
		ifan = (const struct if_announcemsghdr *)(const void *)buf;
		(void)strlcpy(ifname, ifan->ifan_name, sizeof(ifname));

		switch (ifan->ifan_what) {
		case IFAN_ARRIVAL:
			lmlog("KEM-IFAN: %s arrived", ifname);
			publish_link_current(ifname);
			break;
		case IFAN_DEPARTURE:
			lmlog("KEM-IFAN: %s departed", ifname);
			unpublish_link(ifname);
			unpublish_ip(ifname);
			break;
		default:
			break;
		}
		return;
	}

	/*
	 * An address was added to or removed from an interface (#338).
	 * The message names only the interface that changed, so rebuild
	 * that interface's IPv4 / IPv6 keys from the kernel's current
	 * list, as Darwin's ev_ipv4.c / ev_ipv6.c do. An interface that
	 * is already gone fails if_indextoname; its departure announce
	 * removes the keys.
	 */
	if (rtm->rtm_type == RTM_NEWADDR || rtm->rtm_type == RTM_DELADDR) {
		const struct ifa_msghdr *ifam;

		if (companion || n < sizeof(struct ifa_msghdr))
			return;
		ifam = (const struct ifa_msghdr *)(const void *)buf;
		if (if_indextoname(ifam->ifam_index, ifname) != NULL)
			publish_ip(ifname);
		return;
	}

	if (rtm->rtm_type != RTM_IFINFO)
		return;
	if (n < sizeof(struct if_msghdr))
		return;
	ifm = (const struct if_msghdr *)(const void *)buf;
	if (companion && ifm->ifm_data.ifi_link_state != LINK_STATE_UNKNOWN)
		return;
	if (if_indextoname(ifm->ifm_index, ifname) == NULL)
		return;

	publish_link(ifname,
	    link_active(ifm->ifm_data.ifi_link_state, ifm->ifm_flags));
}

/*
 * Open the kernel event socket, filtered to Apple network events — the
 * socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT) + SIOCSKEVFILT pair of Darwin's
 * eventmon.c. -1 when the kernel has no PF_SYSTEM; the caller then falls back
 * to PF_ROUTE.
 */
static int
kev_open(void)
{
	struct kev_request	kr;
	int			s;

	s = socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT);
	if (s < 0)
		return (-1);
	(void)memset(&kr, 0, sizeof(kr));
	kr.vendor_code = KEV_VENDOR_APPLE;
	kr.kev_class = KEV_NETWORK_CLASS;
	kr.kev_subclass = KEV_ANY_SUBCLASS;
	if (ioctl(s, SIOCSKEVFILT, &kr) != 0) {
		(void)close(s);
		return (-1);
	}
	return (s);
}

/*
 * The interface a network event names. Darwin splits the name into a driver
 * name and a unit ("em" / 0) and eventmon.c joins them with "%s%d". The
 * kernel passes a name with no trailing digits ("epair0a") whole with unit
 * 0, so when "<name>0" is not an interface but the bare name is, use that.
 */
static int
kev_ifname(const struct net_event_data *ed, char *ifname, size_t len)
{
	char	name[sizeof(ed->if_name) + 1];

	(void)memcpy(name, ed->if_name, sizeof(ed->if_name));
	name[sizeof(ed->if_name)] = '\0';
	if (name[0] == '\0')
		return (-1);
	(void)snprintf(ifname, len, "%s%u", name, (unsigned)ed->if_unit);
	if (ed->if_unit == 0 && if_nametoindex(ifname) == 0 &&
	    if_nametoindex(name) != 0)
		(void)strlcpy(ifname, name, len);
	return (0);
}

/*
 * One KEV_NETWORK_CLASS event, mapped as Darwin's KernelEventMonitor maps it
 * (eventmon.c processEvent_Apple_Network, ev_dlil.c). Every payload handled
 * here starts with a struct net_event_data naming the interface. Returns 1
 * for KEV_DL_SIFFLAGS, which tells the caller the PF_ROUTE companion is no
 * longer needed.
 */
static int
kev_message(const struct kern_event_msg *ev)
{
	const struct net_event_data	*ed;
	char				ifname[IFNAMSIZ];

	if (ev->vendor_code != KEV_VENDOR_APPLE ||
	    ev->kev_class != KEV_NETWORK_CLASS)
		return (0);
	if (ev->total_size < KEV_MSG_HEADER_SIZE + sizeof(*ed))
		return (0);
	ed = (const struct net_event_data *)(const void *)&ev->event_data[0];
	if (kev_ifname(ed, ifname, sizeof(ifname)) != 0)
		return (0);

	switch (ev->kev_subclass) {
	case KEV_DL_SUBCLASS:
		switch (ev->event_code) {
		case KEV_DL_IF_ATTACHED:
			lmlog("KEM-IFAN: %s arrived", ifname);
			publish_link_current(ifname);
			break;
		case KEV_DL_IF_DETACHED:
			lmlog("KEM-IFAN: %s departed", ifname);
			unpublish_link(ifname);
			unpublish_ip(ifname);
			break;
		case KEV_DL_LINK_ON:
		case KEV_DL_LINK_OFF:
			/* ev_dlil.c link_update_status: re-read, don't trust
			 * the event, so a quick flap settles on the truth. */
			publish_link_current(ifname);
			break;
		case KEV_DL_SIFFLAGS:
			publish_link_current(ifname);
			return (1);
		default:
			/* KEV_DL_SIFMTU and the rest: nothing published
			 * here carries them. */
			break;
		}
		break;
	case KEV_INET_SUBCLASS:
		switch (ev->event_code) {
		case KEV_INET_NEW_ADDR:
		case KEV_INET_CHANGED_ADDR:
		case KEV_INET_ADDR_DELETED:
		case KEV_INET_SIFDSTADDR:
		case KEV_INET_SIFBRDADDR:
		case KEV_INET_SIFNETMASK:
			publish_ip(ifname);
			break;
		default:
			break;
		}
		break;
	case KEV_INET6_SUBCLASS:
		switch (ev->event_code) {
		case KEV_INET6_NEW_USER_ADDR:
		case KEV_INET6_CHANGED_ADDR:
		case KEV_INET6_ADDR_DELETED:
		case KEV_INET6_NEW_LL_ADDR:
		case KEV_INET6_NEW_RTADV_ADDR:
			publish_ip(ifname);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return (0);
}

static void *
monitor_thread(void *arg)
{
	struct pollfd	pfd[2];
	int		es, rs;

	(void)arg;

	if (open_session() != 0)
		return (NULL);

	es = kev_open();
	rs = socket(PF_ROUTE, SOCK_RAW, 0);
	if (es < 0 && rs < 0) {
		lmlog("KEM-FAIL: socket(PF_ROUTE): %s", strerror(errno));
		return (NULL);
	}
	lmlog("link monitor source: %s", es >= 0 ?
	    "PF_SYSTEM kernel events" : "PF_ROUTE (no PF_SYSTEM)");

	/* Seed the store before blocking on the event sockets. */
	publish_all();

	/* poll(2) skips a negative fd, so a closed source just drops out. */
	pfd[0].fd = es;
	pfd[0].events = POLLIN;
	pfd[1].fd = rs;
	pfd[1].events = POLLIN;

	while (pfd[0].fd >= 0 || pfd[1].fd >= 0) {
		union {
			struct kern_event_msg	ev;
			char			buf[2048];
		}		u;
		size_t		off;
		ssize_t		n;

		pfd[0].revents = pfd[1].revents = 0;
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			lmlog("KEM-FAIL: poll: %s", strerror(errno));
			break;
		}

		if (pfd[0].revents != 0) {
			n = recv(pfd[0].fd, u.buf, sizeof(u.buf), 0);
			if (n <= 0 && !(n < 0 && errno == EINTR)) {
				lmlog("KEM-FAIL: recv(PF_SYSTEM): %s",
				    n < 0 ? strerror(errno) : "EOF");
				break;
			}
			/* Walk every record in the read, as eventmon.c does. */
			for (off = 0; n > 0 && off + KEV_MSG_HEADER_SIZE <=
			    (size_t)n;) {
				const struct kern_event_msg *ev;

				ev = (const struct kern_event_msg *)
				    (const void *)(u.buf + off);
				if (ev->total_size < KEV_MSG_HEADER_SIZE ||
				    off + ev->total_size > (size_t)n)
					break;
				if (kev_message(ev) && pfd[1].fd >= 0) {
					(void)close(pfd[1].fd);
					pfd[1].fd = -1;
				}
				off += ev->total_size;
			}
		}

		if (pfd[1].fd >= 0 && pfd[1].revents != 0) {
			n = read(pfd[1].fd, u.buf, sizeof(u.buf));
			if (n <= 0) {
				if (n < 0 && errno == EINTR)
					continue;
				lmlog("KEM-FAIL: read(PF_ROUTE): %s",
				    n < 0 ? strerror(errno) : "EOF");
				(void)close(pfd[1].fd);
				pfd[1].fd = -1;
				continue;
			}
			route_message(u.buf, (size_t)n, pfd[0].fd >= 0);
		}
	}

	if (pfd[0].fd >= 0)
		(void)close(pfd[0].fd);
	if (pfd[1].fd >= 0)
		(void)close(pfd[1].fd);
	return (NULL);
}

void
config_link_monitor_start(void)
{
	pthread_t	th;

	if (pthread_create(&th, NULL, monitor_thread, NULL) != 0) {
		lmlog("KEM-FAIL: pthread_create: %s", strerror(errno));
		return;
	}
	(void)pthread_detach(th);
	lmlog("link monitor started (kernel events, else PF_ROUTE -> "
	    "State:/Network/Interface/<if>/{Link,IPv4,IPv6}, in-process)");
}
