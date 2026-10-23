/*
 * ipconfigd — freebsd-launchd-mach IPConfiguration daemon.
 *
 * The Mach-IPC track port of Apple's IPConfiguration
 * (apple-oss-distributions/bootp / IPConfiguration.bproj). Apple
 * runs it as a configd plugin; we run standalone because this
 * repo's configd has no plugin loader (see [[configd-port-state]]
 * memory). Plan inventory + amputation list at
 * pkgdemon.github.io/nextbsd-ipconfiguration-plan.html.
 *
 * iter 1 — daemon skeleton. main + signal handling + getifaddrs
 * interface enumeration + bootstrap_check_in for
 * com.apple.IPConfiguration + a sleep loop that holds the service
 * port until SIGTERM. Marker IPCFG-BOOT-OK is emitted by the
 * separate `ipconfigtest` client (bootstrap_look_up against this
 * service) — same pattern hwregd / configd iter 1 use.
 *
 * iter 2 — one-shot DHCPv4 DISCOVER/OFFER probe (dhcp_discover.c).
 *
 * iter 3 — full RFC 2131 INIT → BOUND. dhcp_discover.c runs the
 * SELECTING → REQUESTING → BOUND state machine; apply_lease.c then
 * installs the result (SIOCAIFADDR, default route, /etc/resolv.conf).
 *
 * iter 4 — SCDynamicStore publish on BOUND + RFC 2131 §4.4.5
 * RENEWING/REBINDING lease loop. sc_publish.c builds the
 * State:/Network/Service/<UUID>/IPv4 dictionary; lease_loop.c
 * sleeps until T1, sends a broadcast RENEWING REQUEST, etc.
 *
 * iter 5a — raw mach_msg MIG demux + worker thread.
 * mach_service.c spawns a pthread that bootstrap_check_in's the
 * service port and runs _ipconfig_server() (MIG demux for
 * ipconfig.defs) on each request. The worker reads live state via
 * bound_state.{c,h}; each interface's DHCP worker writes it on BOUND.
 * iter 5a vendors 2 read-only routines (if_count, if_addr); the full
 * ipconfig.defs surface grows in iter 6+.
 *
 * link-state DHCP trigger — react to link-up via SCDynamicStore.
 * sc_link_watch.c watches State:/Network/Interface/<if>/Link, which
 * the standalone KernelEventMonitor daemon publishes from PF_ROUTE
 * link-state changes; when an interface goes Active the watch invokes
 * on_link_event(), which admin-ups it (link down) or starts a DHCP
 * worker (link up). This is the Apple-shaped trigger
 * (KernelEventMonitor -> SCDynamicStore -> IPConfiguration); it
 * replaced the earlier hwregd attach subscription (removed with hwregd
 * in PR #167). At startup we bring the candidate NIC IFF_UP so its link
 * negotiates, then let the watch fire DHCP — fixing the stock-kernel
 * case where a real NIC's link comes up a beat after a one-shot startup
 * scan would have given up.
 *
 * multi-interface (#37/#38/#39/#41) — the daemon was, structurally, a
 * single-interface wired-desktop daemon, and WLAN is what forced it to
 * grow up. Four bugs, all invisible while only ever one wired NIC was
 * exercised:
 *
 *   #38  exactly ONE interface could ever bind, for the daemon's whole
 *        lifetime — a global g_dhcp_started plus a global bound_state.
 *        There is now one worker slot and one lease-table entry per
 *        interface, so em0 and wlan0 bind independently.
 *   #37  the post-expiry re-arm was dead code: it cleared the in-flight
 *        flag but nothing ever cleared bound_state, so the next guard
 *        short-circuited and DHCP never ran again until restart. Workers
 *        now clear their own entry on teardown.
 *   #39  there was no deconfigure path at all — no SIOCDIFADDR, no
 *        RTM_DELETE anywhere. Walk out of WLAN range and the dead IP and
 *        dead default route stayed installed, blackholing traffic. A
 *        link-down now unwinds that interface's worker, which takes them
 *        back.
 *   #41  State:/Network/Global/IPv4 was last-binder-wins, so "primary
 *        interface" was a race between em0 and wlan0 — and both
 *        mDNSResponder and hostnamed read that key. Primary is now the
 *        highest-ranked bound interface (wired > wireless), re-elected
 *        on every bind and teardown; /etc/resolv.conf follows it.
 *
 * configuration methods (#58) — every linked interface used to be DHCP'd
 * unconditionally, so a hand-configured static address was overwritten
 * by the next link event. Each interface's service now runs the IPv4
 * ConfigMethod of the network service that names it in SCPreferences
 * (ipconfig_prefs.c), or the method `ipconfig set` gave it: DHCP, Manual
 * (static address, mask, router, DNS — installed and published through
 * the same path as a lease), or NONE (left entirely alone). An interface
 * nothing configures stays on DHCP, so an unconfigured machine behaves
 * exactly as before.
 *
 * address conflicts (#349) — configd posts the kernel's ARP-collision and
 * duplicate-address events as notify-only keys, and the link watch hands
 * them to on_addr_conflict(), as Darwin's ipconfigd routes
 * IPv4ARPCollision to IFEventID_arp_collision_e:
 *
 *   - another host claims our bound IPv4 address: defend it with a
 *     gratuitous ARP (RFC 5227 §2.4, ServiceDefendIPv4Address) unless the
 *     address is under DEFEND_IP_ADDRESS_INTERVAL_SECS old or has been
 *     defended DEFEND_IP_ADDRESS_COUNT times in a row; otherwise give it
 *     up. A DHCP service deconfigures, sends DHCPDECLINE and starts over
 *     after the RFC 2131 §3.1.5 ten seconds (dhcp.c); a Manual service
 *     deconfigures and tries again in MANUAL_CONFLICT_RETRY_INTERVAL_SECS
 *     (manual.c).
 *   - DAD finds our SLAAC address in use: take the address, its ::/0 and
 *     the IPv6 publish back (rtadv.c never publishes a duplicated
 *     address). This replaces #342's SIOCGIFAFLAG_IN6 poll — the kernel's
 *     own DAD event drives it, so a defender that appears after DAD is
 *     caught too.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>		/* inet_ntop for the BOUND log line */

#include <errno.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apply_lease.h"
#include "apply_lease_v6.h"
#include "arp_probe.h"
#include "bound_state.h"
#include "dhcp_discover.h"
#include "ipconfig_method.h"
#include "ipconfig_prefs.h"
#include "ipconfigd.h"
#include "lease_loop.h"
#include "mach_service.h"
#include "ra_listen.h"
#include "ra_router.h"
#include "sc_link_watch.h"
#include "sc_publish.h"

/*
 * Global (non-static): dhcp_discover.c's recv loop polls this
 * between BPF reads so SIGTERM/SIGHUP during the multi-second
 * retransmit ladder short-circuits the wait. Declared in
 * dhcp_discover.h.
 */
volatile sig_atomic_t got_term;

/*
 * Per-interface DHCP workers (#38).
 *
 * This was a single global `g_dhcp_started` int plus a check of
 * bound_state_any(), which together permitted exactly ONE bound
 * interface for the daemon's lifetime: once any interface was in flight
 * or bound, every later link event returned early. On a laptop with em0
 * and wlan0, whichever linked first won and the other was ignored
 * forever — so the WLAN DHCP gate could be perfectly correct and still
 * be defeated by em0 winning the race.
 *
 * It is now one slot per interface. Each slot owns a thread that runs
 * that interface's DHCP exchange and then parks in its lease loop, plus
 * a `stop` flag the link-watch sets to unwind that thread when the link
 * drops.
 *
 * The threads are deliberately real pthreads rather than more work on
 * libdispatch. dhcp_run_on_interface() blocks for the entire life of a
 * lease — hours — and it used to do that *on the libdispatch worker
 * thread that delivered the link event*. One interface merely held a
 * dispatch thread hostage; N interfaces would starve the pool and
 * deadlock the watch that feeds them. A thread per bound interface costs
 * nothing at the two-or-three interfaces any real machine has.
 */
struct dhcp_worker {
	bool			active;		/* slot in use */
	char			ifname[IFNAMSIZ];
	pthread_t		tid;
	volatile sig_atomic_t	stop;		/* link went down: unwind */
	uint32_t		lease_cap_secs;
	bool			ready;		/* bound or given up: startup */
	struct ipconfig_method_info method;	/* what this worker runs (#58) */
	bool			restart;	/* method changed: rerun on exit */

	/*
	 * Address conflicts (#349). `conflict` is set with `stop` when an
	 * ARP collision makes us give the IPv4 address up; the worker then
	 * declines / waits and runs its method again instead of exiting.
	 * The rest is ServiceDefendIPv4Address's bookkeeping (Apple's
	 * ServiceIPv4 ip_assigned_time / ip_conflict_*).
	 */
	bool			conflict;
	time_t			v4_assigned;	/* monotonic secs at bind */
	time_t			conflict_time;
	int			conflict_count;

	/*
	 * The SLAAC address, so a duplicated-address notification can find
	 * it. PENDING from just before it is installed until it is published
	 * and tracked — a duplicate reported in that window is only recorded
	 * (v6_dup), and v6_commit() acts on it — then ACTIVE, when the
	 * notification withdraws it directly.
	 */
	int			v6_state;
	bool			v6_dup;
	struct in6_addr		v6_dup_addr;
	struct in6_addr		v6_addr;
	struct in6_addr		v6_router;
	unsigned		v6_ifindex;
	struct ra_router	*v6_rtr;
};

#define	V6_NONE		0
#define	V6_PENDING	1
#define	V6_ACTIVE	2

/*
 * Darwin's defaults (bootp ipconfigd.c): defend an address at most
 * DEFEND_IP_ADDRESS_COUNT times, never in its first
 * DEFEND_IP_ADDRESS_INTERVAL_SECS, with the count starting over after that
 * long without a conflict; a Manual address given up is retried after
 * MANUAL_CONFLICT_RETRY_INTERVAL_SECS. DHCP_DECLINE_WAIT_SECS is RFC 2131
 * §3.1.5's minimum wait between DHCPDECLINE and a fresh DISCOVER.
 */
#define	DEFEND_IP_ADDRESS_INTERVAL_SECS		10
#define	DEFEND_IP_ADDRESS_COUNT			5
#define	MANUAL_CONFLICT_RETRY_INTERVAL_SECS	300
#define	DHCP_DECLINE_WAIT_SECS			10

static pthread_mutex_t	g_workers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dhcp_worker g_workers[BOUND_MAX_IF];

/*
 * `ipconfig set` methods (#58/#59), which win over SCPreferences until
 * the next `set` or an ipconfigd restart — in memory only, like Apple's.
 * A NONE here is what holds an interface the administrator switched
 * off, so a link flap does not quietly re-DHCP it. Guarded by
 * g_workers_lock.
 */
static struct {
	char				ifname[IFNAMSIZ];
	struct ipconfig_method_info	info;
} g_set_methods[BOUND_MAX_IF];

/* main()'s IPCONFIGD_FAST_LEASE cap, for workers not started by a link event. */
static uint32_t		g_lease_cap_secs;

/* Plugin:IPConfiguration published (one-shot); guarded by g_workers_lock. */
static bool		g_startup_done;

static void
on_signal(int sig)
{
	got_term = sig;
}

/*
 * Bring an interface administratively up (IFF_UP) so its link
 * negotiates — without touching addresses. ipconfigd does this at
 * startup for the candidate NIC; the resulting link-state change is
 * what KernelEventMonitor reports and our watch turns into a DHCP run.
 */
static int
iface_bring_up(const char *ifname)
{
	struct ifreq ifr;
	int s, rc = -1;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return (-1);
	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		if ((ifr.ifr_flags & IFF_UP) != 0) {
			rc = 0;
		} else {
			ifr.ifr_flags |= IFF_UP;
			if (ioctl(s, SIOCSIFFLAGS, &ifr) == 0)
				rc = 0;
		}
	}
	(void)close(s);
	return (rc);
}

static void
xlog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "ipconfigd %s ", tbuf);

	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Walk getifaddrs(3) and log each interface's BSD name + family.
 * iter 1's "real" content: the same enumeration later iters use to
 * pick which interfaces to DHCP on. The AF_LINK pass also surfaces
 * the MAC address — useful when DHCP needs it for the chaddr.
 */
static int
enumerate_interfaces(void)
{
	struct ifaddrs *ifa, *p;
	int total = 0, with_link = 0;

	if (getifaddrs(&ifa) != 0) {
		xlog("getifaddrs failed: %s", strerror(errno));
		return (-1);
	}
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const char *fam = "?";

		if (p->ifa_addr == NULL)
			continue;
		switch (p->ifa_addr->sa_family) {
		case AF_INET:	fam = "AF_INET"; break;
		case AF_INET6:	fam = "AF_INET6"; break;
		case AF_LINK:	fam = "AF_LINK"; with_link++; break;
		default:	continue;
		}
		xlog("iface: %s family=%s flags=0x%x", p->ifa_name, fam,
		    p->ifa_flags);
		total++;
	}
	freeifaddrs(ifa);
	xlog("interface scan: %d records (%d AF_LINK)", total, with_link);
	return (total);
}

/*
 * Plugin:IPConfiguration — Apple's "IPConfiguration has finished
 * starting up" key, which `ipconfig waitall` blocks on (#59). Apple's
 * all_services_ready() sets it once every configured service is ready,
 * i.e. has either configured its address or given up. Ours: once no
 * DHCP worker is still acquiring. One-shot, like Apple's; the key
 * outlives the session that set it.
 *
 * Caller must not hold g_workers_lock.
 */
static void
startup_check(void)
{
	bool all_ready = true;
	int i;

	pthread_mutex_lock(&g_workers_lock);
	if (g_startup_done) {
		pthread_mutex_unlock(&g_workers_lock);
		return;
	}
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_workers[i].active && !g_workers[i].ready)
			all_ready = false;
	}
	if (all_ready)
		g_startup_done = true;
	pthread_mutex_unlock(&g_workers_lock);

	if (all_ready) {
		if (sc_publish_startup_ready() == 0)
			xlog("startup complete: published Plugin:IPConfiguration");
		else
			xlog("startup complete: Plugin:IPConfiguration publish "
			    "failed");
	}
}

/* `ifname`'s worker has configured IPv4; re-check startup. */
static void
startup_mark_ready(const char *ifname)
{
	int i;

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_workers[i].active &&
		    strcmp(g_workers[i].ifname, ifname) == 0)
			g_workers[i].ready = true;
	}
	pthread_mutex_unlock(&g_workers_lock);
	startup_check();
}

/* The active worker for `ifname`, or NULL. Caller holds g_workers_lock. */
static struct dhcp_worker *
worker_find_locked(const char *ifname)
{
	int i;

	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_workers[i].active &&
		    strcmp(g_workers[i].ifname, ifname) == 0)
			return (&g_workers[i]);
	}
	return (NULL);
}

static time_t
monotonic_secs(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec);
}

/* `ifname` just took its IPv4 address: start the defend clock (#349). */
static void
worker_v4_assigned(const char *ifname)
{
	struct dhcp_worker *w;

	pthread_mutex_lock(&g_workers_lock);
	if ((w = worker_find_locked(ifname)) != NULL)
		w->v4_assigned = monotonic_secs();
	pthread_mutex_unlock(&g_workers_lock);
}

/* Did `ifname`'s service just unwind because its address was in use? */
static bool
worker_conflicted(const char *ifname)
{
	struct dhcp_worker *w;
	bool conflict = false;

	pthread_mutex_lock(&g_workers_lock);
	if ((w = worker_find_locked(ifname)) != NULL)
		conflict = w->conflict;
	pthread_mutex_unlock(&g_workers_lock);
	return (conflict);
}

/*
 * Take a duplicated SLAAC address back: stop the router tracker with its
 * ::/0 (or, without one, the ::/0 apply_ra_lease installed), remove the
 * address and withdraw State:/Network/Service/<UUID>/IPv6 — what #342 did
 * when its DAD poll saw IN6_IFF_DUPLICATED, and what rtadv.c amounts to by
 * never publishing a duplicated address. IPv4 is untouched.
 */
static void
v6_withdraw(const char *ifname, const struct in6_addr *addr,
    const struct in6_addr *router, unsigned ifindex, struct ra_router *rtr)
{
	struct sc_publish *pub;
	char a[INET6_ADDRSTRLEN];

	(void)inet_ntop(AF_INET6, addr, a, sizeof(a));
	xlog("%s: DAD failed: %s is a duplicate — removing it, its ::/0 and "
	    "the IPv6 publish", ifname, a);
	if (rtr != NULL)
		ra_router_stop(rtr, 1);
	else
		(void)remove_v6_default_route(router, ifindex);
	(void)remove_v6_address(ifname, addr);
	pub = sc_publish_open("ipconfigd");
	if (pub != NULL) {
		(void)sc_publish_remove_ipv6(pub, ifname);
		sc_publish_close(pub);
	}
}

/* `ifname` is about to install a SLAAC address: hold duplicates for it. */
static void
v6_pending(const char *ifname)
{
	struct dhcp_worker *w;

	pthread_mutex_lock(&g_workers_lock);
	if ((w = worker_find_locked(ifname)) != NULL) {
		w->v6_state = V6_PENDING;
		w->v6_dup = false;
	}
	pthread_mutex_unlock(&g_workers_lock);
}

/*
 * The SLAAC address `v6` (NULL: none was installed) is published and its
 * router tracker `rtr` running. Hand both to the worker table so a later
 * duplicated-address notification can withdraw them — or, if DAD already
 * reported `v6` duplicated while it was pending, withdraw them now.
 */
static void
v6_commit(const char *ifname, const struct in6_addr *v6,
    const struct ra_info *ra, struct ra_router *rtr)
{
	struct dhcp_worker *w;
	bool dup = false;

	pthread_mutex_lock(&g_workers_lock);
	if ((w = worker_find_locked(ifname)) != NULL) {
		if (v6 == NULL) {
			w->v6_state = V6_NONE;
		} else if (w->v6_dup &&
		    IN6_ARE_ADDR_EQUAL(&w->v6_dup_addr, v6)) {
			w->v6_state = V6_NONE;
			dup = true;
		} else {
			w->v6_state = V6_ACTIVE;
			w->v6_addr = *v6;
			w->v6_router = ra->router_lladdr;
			w->v6_ifindex = ra->router_scope_id;
			w->v6_rtr = rtr;
		}
		w->v6_dup = false;
	}
	pthread_mutex_unlock(&g_workers_lock);
	if (dup)
		v6_withdraw(ifname, v6, &ra->router_lladdr,
		    ra->router_scope_id, rtr);
}

/*
 * The service is unwinding: take back its router tracker for teardown.
 * NULL when there is none, or a duplicated-address notification already
 * withdrew it.
 */
static struct ra_router *
v6_end(const char *ifname)
{
	struct dhcp_worker *w;
	struct ra_router *rtr = NULL;

	pthread_mutex_lock(&g_workers_lock);
	if ((w = worker_find_locked(ifname)) != NULL) {
		if (w->v6_state == V6_ACTIVE)
			rtr = w->v6_rtr;
		w->v6_rtr = NULL;
		w->v6_state = V6_NONE;
	}
	pthread_mutex_unlock(&g_workers_lock);
	return (rtr);
}

/*
 * Read IPCONFIGD_FAST_LEASE from the environment. The env var caps
 * the effective lease time used for T1 / T2 timer math (the value
 * published to configd is unaffected). Accepts integers in [4, 86400]
 * seconds; outside that range, or unset, the cap is disabled (return
 * 0). Floor of 4 because the iter-3 retransmit ladder is 4s.
 */
static uint32_t
read_lease_cap_env(void)
{
	const char *s = getenv("IPCONFIGD_FAST_LEASE");
	char *end;
	long v;

	if (s == NULL || *s == '\0')
		return (0);
	v = strtol(s, &end, 10);
	if (end == s || *end != '\0' || v < 4 || v > 86400) {
		xlog("IPCONFIGD_FAST_LEASE='%s' ignored (need integer "
		    "in [4, 86400])", s);
		return (0);
	}
	xlog("IPCONFIGD_FAST_LEASE=%ld — capping lease for renewal "
	    "timer math (server lease still authoritative)", v);
	return ((uint32_t)v);
}

/*
 * service_run_bound — everything after an IPv4 configuration is installed
 * on `ifname`: record it, publish it, elect the primary, bring up IPv6
 * (RA/SLAAC), park, then tear it all down. Shared by the DHCP and Manual
 * services (#58), which differ only in how they get `lease` and in
 * whether parking means a renewal loop. Runs on the interface's worker
 * thread.
 *
 * Returns when the lease is lost, the link goes down or the method
 * changes (*stop), or the daemon is shutting down (got_term). Every one
 * of those paths falls through to the single teardown block at the
 * bottom — there are no early returns once the interface is bound, which
 * is what the old code did on a publish failure (it left the lease
 * applied and the interface in bound_state, but never entered the lease
 * loop, so the lease silently expired and was never renewed).
 */
static void
service_run_bound(const char *ifname, struct dhcp_lease *lease, bool dhcp,
    uint32_t lease_cap_secs, volatile sig_atomic_t *stop)
{
	struct sc_publish *pub = NULL;
	char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN];
	char r[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];
	struct ra_info ra;
	struct ra_router *rtr = NULL;
	int rar;

	(void)inet_ntop(AF_INET, &lease->addr, a, sizeof(a));
	(void)inet_ntop(AF_INET, &lease->netmask, m, sizeof(m));
	(void)inet_ntop(AF_INET, &lease->router, r, sizeof(r));
	(void)inet_ntop(AF_INET, &lease->server, s, sizeof(s));
	if (dhcp)
		xlog("bound: iface=%s addr=%s netmask=%s router=%s "
		    "server=%s lease=%us",
		    ifname, a, m, r, s, (unsigned)lease->lease_time);
	else
		xlog("configured: iface=%s method=Manual addr=%s netmask=%s "
		    "router=%s", ifname, a, m, r);
	/*
	 * Make the lease visible to the Mach service worker before the
	 * BOUND marker fires — so any client racing IPCFG-RPC-OK against
	 * IPCFG-BOUND-OK sees the address it expects.
	 */
	bound_state_set(ifname, lease);
	worker_v4_assigned(ifname);
	if (dhcp)
		xlog("IPCFG-BOUND-OK");

	/*
	 * Publish to configd. Failure is non-fatal: the daemon stays up and
	 * the lease is already applied at the kernel level; observers just
	 * won't see the binding. We still enter the lease loop, because a
	 * lease nobody published is a lease that still has to be renewed.
	 * A Manual service publishes the same IPv4 / DNS shape, so
	 * mDNSResponder and hostnamed see a primary service either way; it
	 * has no /DHCP entity.
	 */
	pub = sc_publish_open("ipconfigd");
	if (pub == NULL) {
		xlog("IPCFG-STORE-FAIL: no configd session");
	} else if (sc_publish_ipv4(pub, ifname, lease) != 0) {
		xlog("IPCFG-STORE-FAIL: set State:/.../IPv4");
	} else {
		xlog("IPCFG-STORE-OK");
	}
	if (pub != NULL && dhcp) {
		/*
		 * Issue #88: publish State:/Network/Service/<UUID>/DHCP
		 * carrying InterfaceName + LeaseStartTime, and Option_12 (host
		 * name) when the lease supplied it. SLIRP doesn't ship
		 * Option_12, so the marker proves the key/dict shape is
		 * correct; hostnamed iter 3 is the first consumer that reads
		 * it. Failure is non-fatal — the IPv4 publish already
		 * succeeded.
		 */
		if (sc_publish_dhcp(pub, ifname, lease) != 0) {
			xlog("IPCFG-DHCP-FAIL: set State:/.../DHCP");
		} else {
			xlog("IPCFG-DHCP-OK: published /DHCP "
			    "(Option_12 %s)",
			    lease->host_name_len > 0
			        ? lease->host_name : "absent");
		}
	}

	/*
	 * Re-elect the primary now that this interface is in bound_state
	 * (#41), then render /etc/resolv.conf from whoever won. Doing DNS
	 * off the primary rather than off `lease` is what stops a wlan0
	 * binding from stomping a wired em0's resolvers.
	 */
	if (pub != NULL)
		(void)sc_publish_update_primary(pub);
	resolv_conf_sync();

	/* IPv4 is configured: this service no longer holds up `waitall`. */
	startup_mark_ready(ifname);

	/*
	 * iter 7a: solicit + listen for one RA, derive a SLAAC address,
	 * install it + the v6 default route, and publish
	 * State:/.../IPv6. 15s budget — QEMU SLIRP answers within ms,
	 * so a miss means RA isn't configured; we log IPCFG-RA-MISS and
	 * continue with IPv4 only.
	 *
	 * Round-1 CI showed em0 has ND6_IFF_IFDISABLED set (this image's
	 * net.inet6.ip6.auto_linklocal is 0, so the kernel never
	 * auto-added a link-local). bring_v6_up clears IFDISABLED and
	 * installs fe80::EUI-64 so the kernel can source-select the
	 * link-local-scoped RS.
	 */
	(void)bring_v6_up(ifname);
	rar = ra_acquire(ifname, 15000, &ra);
	if (rar == 0) {
		struct in6_addr v6;

		/*
		 * DAD runs in the kernel after the install; a duplicate comes
		 * back as the IPv6DuplicatedAddress notification (#349),
		 * which v6_commit() honours if it beat us here.
		 */
		v6_pending(ifname);
		if (apply_ra_lease(ifname, &ra, &v6) == 0) {
			if (pub != NULL)
				(void)sc_publish_ipv6(pub, ifname, &v6,
				    ra.prefix_len, &ra.router_lladdr);
			xlog("IPCFG-RA-OK");
			/*
			 * Keep hearing RAs so ::/0 follows the router's
			 * lifetime instead of outliving it (#350).
			 */
			rtr = ra_router_start(ifname, &ra, &v6);
			v6_commit(ifname, &v6, &ra, rtr);
		} else {
			v6_commit(ifname, NULL, NULL, NULL);
			xlog("IPCFG-RA-MISS: apply_ra_lease failed");
		}
	} else if (rar == 1) {
		xlog("IPCFG-RA-MISS: no RA in 15s "
		    "(SLIRP may not advertise IPv6)");
	} else {
		xlog("IPCFG-RA-MISS: ra_acquire fatal");
	}

	/*
	 * Parks here for the life of the lease. A Manual configuration has
	 * nothing to renew; it holds until the link drops, its method
	 * changes, or the daemon exits.
	 */
	if (dhcp)
		(void)lease_loop_run(ifname, lease, pub, lease_cap_secs, stop);
	else
		while (!got_term && !(stop != NULL && *stop))
			(void)sleep(1);

	/*
	 * Stop the router tracker before anything below removes the IPv6
	 * key, so it cannot republish behind the teardown. Same SIGTERM
	 * rule as the IPv4 lease: keep ::/0 across a restart, otherwise
	 * take it back. The worker table owns the tracker once committed
	 * (a duplicated address may already have stopped it).
	 */
	rtr = v6_end(ifname);
	ra_router_stop(rtr, !got_term);

	/*
	 * Teardown (#37, #39).
	 *
	 * On SIGTERM we deliberately leave the address and route installed:
	 * the machine should stay on the network across an ipconfigd restart,
	 * and Apple's IPConfiguration behaves the same way. Every other exit
	 * — lease expired without renewal, or the link went down — means the
	 * configuration is now a lie, so we take it all back.
	 *
	 * Leaving it installed is exactly the bug in #39: walk out of WLAN
	 * range and the dead IP plus the dead default route stayed, silently
	 * blackholing traffic. And clearing bound_state here is what makes
	 * the re-arm real (#37) — without it, bound_state_any() kept
	 * reporting the interface bound and every future link event
	 * short-circuited, so a single lease expiry meant the daemon never
	 * DHCP'd again until restarted.
	 */
	if (got_term) {
		xlog("%s: daemon shutting down — leaving the lease installed",
		    ifname);
	} else {
		xlog("%s: %s — deconfiguring", ifname, dhcp
		    ? "lease lost, link down, method changed or address in use"
		    : "link down, method changed or address in use");
		(void)deconfigure_lease(ifname, lease);
		if (pub != NULL)
			(void)sc_publish_remove(pub, ifname);
		bound_state_clear(ifname);
		if (pub != NULL)
			(void)sc_publish_update_primary(pub);
		resolv_conf_sync();
	}

	if (pub != NULL)
		sc_publish_close(pub);
}

/*
 * dhcp_run_on_interface — DHCPv4 INIT → BOUND on `ifname`, then
 * service_run_bound().
 */
static void
dhcp_run_on_interface(const char *ifname, uint32_t lease_cap_secs,
    volatile sig_atomic_t *stop)
{
	struct dhcp_lease lease;

	xlog("selected interface for DHCPv4: %s", ifname);
	if (dhcp_lease_acquire(ifname, &lease) != 0) {
		/* dhcp_lease_acquire logged IPCFG-BOUND-FAIL on its own line */
		return;
	}

	/*
	 * The DISCOVER ladder can burn ~28s. If the link dropped or the
	 * daemon is going down in that window, do not install a lease we
	 * are about to have to rip back out.
	 */
	if (got_term || (stop != NULL && *stop)) {
		xlog("%s: link/daemon went away during DHCP — discarding lease",
		    ifname);
		return;
	}

	if (apply_lease(ifname, &lease) != 0) {
		xlog("apply_lease(%s) failed", ifname);
		xlog("IPCFG-BOUND-FAIL");
		return;
	}
	service_run_bound(ifname, &lease, true, lease_cap_secs, stop);

	/*
	 * Gave the address up to another host (#349): it is off the interface
	 * now, so tell the server, as dhcp.c's arp_collision path does with
	 * dhcp_decline(). The worker restarts DHCP after the RFC wait.
	 */
	if (!got_term && worker_conflicted(ifname))
		(void)dhcp_decline(ifname, &lease);
}

/*
 * manual_run_on_interface — the Manual service (#58): install the
 * configured address, mask, router and DNS through the same apply /
 * publish path a DHCP lease takes, then service_run_bound().
 *
 * Like Apple's manual service it probes for an address conflict first
 * (RFC 5227); on a conflict it does not configure the address, and the
 * next link-up or method change tries again.
 */
static void
manual_run_on_interface(const char *ifname,
    const struct ipconfig_method_info *info, volatile sig_atomic_t *stop)
{
	struct dhcp_lease lease;
	char a[INET_ADDRSTRLEN];
	unsigned i;

	(void)memset(&lease, 0, sizeof(lease));
	lease.addr = info->addr;
	lease.netmask = info->mask;
	lease.router = info->router;
	for (i = 0; i < info->dns_count && i < DHCP_LEASE_MAX_DNS; i++)
		lease.dns[i] = info->dns[i];
	lease.dns_count = i;

	(void)inet_ntop(AF_INET, &lease.addr, a, sizeof(a));
	xlog("selected interface for Manual: %s (%s)", ifname, a);
	if (arp_probe(ifname, lease.addr) == 1) {
		xlog("%s: %s is in use by another host — not configuring it",
		    ifname, a);
		return;
	}
	if (got_term || (stop != NULL && *stop))
		return;

	if (apply_lease(ifname, &lease) != 0) {
		xlog("apply_lease(%s) failed for Manual %s", ifname, a);
		return;
	}
	service_run_bound(ifname, &lease, false, 0, stop);
}

/*
 * Worker thread body — one per interface that has linked up. Runs the
 * whole DHCP + lease + teardown lifecycle, then releases its slot so a
 * later link event on the same interface can start a fresh one. That
 * release is the other half of the re-arm: the slot, not a global flag,
 * is now what says "a run is in flight on this interface".
 */
static void	service_start(const char *ifname);

static void *
dhcp_worker_main(void *arg)
{
	struct dhcp_worker *w = arg;
	char ifname[IFNAMSIZ];
	struct ipconfig_method_info method;
	bool restart;

	(void)strlcpy(ifname, w->ifname, sizeof(ifname));
	method = w->method;
	for (;;) {
		bool conflict;
		unsigned wait, i;

		if (method.method == ipconfig_method_manual_e)
			manual_run_on_interface(ifname, &method, &w->stop);
		else
			dhcp_run_on_interface(ifname, w->lease_cap_secs,
			    &w->stop);

		/*
		 * Unwound by an address conflict (#349) rather than a link
		 * down or method change: take the stop back and run the
		 * method again once the conflict wait is over — dhcp.c's
		 * decline-then-INIT, manual.c's retry timer.
		 */
		pthread_mutex_lock(&g_workers_lock);
		conflict = w->conflict;
		if (conflict) {
			w->conflict = false;
			w->stop = 0;
		}
		pthread_mutex_unlock(&g_workers_lock);
		if (!conflict || got_term)
			break;
		wait = method.method == ipconfig_method_manual_e
		    ? MANUAL_CONFLICT_RETRY_INTERVAL_SECS
		    : DHCP_DECLINE_WAIT_SECS;
		xlog("%s: address in use — running %s again in %us", ifname,
		    ipconfig_method_string(method.method), wait);
		for (i = 0; i < wait && !got_term && !w->stop; i++)
			(void)sleep(1);
		if (got_term || w->stop)
			break;
	}

	pthread_mutex_lock(&g_workers_lock);
	restart = w->restart;
	w->active = false;
	w->stop = 0;
	w->restart = false;
	w->ifname[0] = '\0';
	pthread_mutex_unlock(&g_workers_lock);

	xlog("%s: %s worker exited", ifname,
	    ipconfig_method_string(method.method));
	/* A worker that gave up without binding is "ready" too (Apple). */
	startup_check();
	/*
	 * Unwound because the interface's method changed (#58): the old
	 * configuration is gone, so start whatever replaced it.
	 */
	if (restart && !got_term)
		service_start(ifname);
	return (NULL);
}

/*
 * Ask the worker owning `ifname` (if any) to unwind, and report whether
 * there was one. Setting `stop` wakes it out of its lease-loop sleep; it
 * then runs its own teardown and frees its slot. We deliberately do NOT
 * join: teardown does ioctls and configd RPCs, and blocking the link-watch
 * dispatch thread on that would be exactly the mistake this refactor exists
 * to undo.
 */
static bool
dhcp_worker_stop(const char *ifname)
{
	bool found = false;
	int i;

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_workers[i].active &&
		    strcmp(g_workers[i].ifname, ifname) == 0) {
			g_workers[i].stop = 1;
			g_workers[i].conflict = false;	/* exit, not retry */
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_workers_lock);
	return (found);
}

/*
 * The IPv4 method `ifname` should run (#58): an `ipconfig set` method
 * first, then its network service in SCPreferences, and DHCP when
 * nothing configures it — which is what keeps an unconfigured machine
 * exactly as it was.
 */
static void
method_lookup(const char *ifname, struct ipconfig_method_info *out)
{
	int i;

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_set_methods[i].ifname[0] != '\0' &&
		    strcmp(g_set_methods[i].ifname, ifname) == 0) {
			*out = g_set_methods[i].info;
			pthread_mutex_unlock(&g_workers_lock);
			return;
		}
	}
	pthread_mutex_unlock(&g_workers_lock);

	if (ipconfig_prefs_lookup(ifname, out))
		return;
	(void)memset(out, 0, sizeof(*out));
	out->method = ipconfig_method_dhcp_e;
}

/*
 * Start a worker running `ifname`'s configured method unless one is
 * already running for it, or the method is one ipconfigd does not serve
 * (NONE included) — those interfaces are left alone. Returns true if a
 * worker was spawned.
 */
static bool
dhcp_worker_start(const char *ifname, uint32_t lease_cap_secs)
{
	struct dhcp_worker *w = NULL;
	struct ipconfig_method_info method;
	pthread_attr_t attr;
	int i, rc;

	method_lookup(ifname, &method);
	if (!ipconfig_method_is_served(method.method))
		return (false);

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_workers[i].active &&
		    strcmp(g_workers[i].ifname, ifname) == 0) {
			pthread_mutex_unlock(&g_workers_lock);
			return (false);		/* already running */
		}
	}
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (!g_workers[i].active) {
			w = &g_workers[i];
			break;
		}
	}
	if (w == NULL) {
		pthread_mutex_unlock(&g_workers_lock);
		xlog("%s: no free DHCP worker slot (%d in use) — ignoring",
		    ifname, BOUND_MAX_IF);
		return (false);
	}
	w->active = true;
	w->stop = 0;
	w->ready = false;
	w->restart = false;
	w->conflict = false;
	w->v4_assigned = 0;
	w->conflict_time = 0;
	w->conflict_count = 0;
	w->v6_state = V6_NONE;
	w->v6_dup = false;
	w->v6_rtr = NULL;
	w->method = method;
	w->lease_cap_secs = lease_cap_secs;
	(void)strlcpy(w->ifname, ifname, sizeof(w->ifname));
	pthread_mutex_unlock(&g_workers_lock);

	/*
	 * Detached: nothing ever joins these. The daemon exits by process
	 * teardown, and a worker's exit path is entirely self-contained.
	 */
	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	/* pthread_create returns the error number; it does not set errno. */
	rc = pthread_create(&w->tid, &attr, dhcp_worker_main, w);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		pthread_mutex_lock(&g_workers_lock);
		w->active = false;
		w->ifname[0] = '\0';
		pthread_mutex_unlock(&g_workers_lock);
		xlog("%s: pthread_create failed: %s", ifname, strerror(rc));
		return (false);
	}
	return (true);
}

/*
 * Start `ifname`'s configured service outside a link event — after its
 * method changed. Admin-up as for any interface, and start the worker
 * now if the link is already up; otherwise the link watch starts it on
 * link-active, as usual.
 */
static void
service_start(const char *ifname)
{
	struct ipconfig_method_info method;

	method_lookup(ifname, &method);
	if (!ipconfig_method_is_served(method.method)) {
		xlog("%s: ConfigMethod %s — leaving it alone", ifname,
		    ipconfig_method_string(method.method));
		return;
	}
	if (iface_bring_up(ifname) != 0) {
		xlog("%s: could not bring up: %s", ifname, strerror(errno));
		return;
	}
	if (link_active_in_store(ifname) &&
	    dhcp_worker_start(ifname, g_lease_cap_secs))
		xlog("%s: started %s worker", ifname,
		    ipconfig_method_string(method.method));
}

/*
 * `ifname`'s method may have changed — a preferences commit, or
 * `ipconfig set`. If a worker is running something else, unwind it; its
 * teardown deconfigures what it installed and its exit starts the new
 * service (the restart flag), so the old and new configurations never
 * overlap. With nothing running, start the new service directly.
 */
static void
service_reconfigure(const char *ifname)
{
	struct ipconfig_method_info method;
	bool unwinding = false;
	int i;

	method_lookup(ifname, &method);
	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		struct dhcp_worker *w = &g_workers[i];

		if (!w->active || strcmp(w->ifname, ifname) != 0)
			continue;
		if (!w->stop && memcmp(&w->method, &method,
		    sizeof(method)) == 0) {
			pthread_mutex_unlock(&g_workers_lock);
			return;		/* already running this */
		}
		w->stop = 1;
		w->restart = true;
		w->conflict = false;	/* the restart replaces any retry */
		unwinding = true;
		break;
	}
	pthread_mutex_unlock(&g_workers_lock);

	if (unwinding) {
		xlog("%s: ConfigMethod now %s — unwinding the running service",
		    ifname, ipconfig_method_string(method.method));
		return;
	}
	service_start(ifname);
}

/* SCPreferences commit changed `ifname`'s service (ipconfig_prefs.c). */
static void
on_prefs_changed(const char *ifname)
{
	if (strncmp(ifname, "lo", 2) == 0)
		return;
	service_reconfigure(ifname);
}

static ipconfig_status_t set_check_ifname(const char *ifname);

/*
 * `ipconfig set` (#59) — see ipconfigd.h. Called on the Mach service
 * thread; never blocks on a worker: unwinding one only sets its stop
 * flag, and its own thread does the teardown, exactly as for a
 * link-down.
 */
ipconfig_status_t
ipconfigd_set_method(const char *ifname,
    const struct ipconfig_method_info *info)
{
	ipconfig_status_t status;
	int i, slot = -1;

	status = set_check_ifname(ifname);
	if (status != ipconfig_status_success_e)
		return (status);
	if (!ipconfig_method_is_served(info->method) &&
	    info->method != ipconfig_method_none_e)
		return (ipconfig_status_operation_not_supported_e);
	if (info->method == ipconfig_method_manual_e &&
	    info->addr.s_addr == INADDR_ANY)
		return (ipconfig_status_invalid_parameter_e);

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_set_methods[i].ifname[0] == '\0') {
			if (slot < 0)
				slot = i;
		} else if (strcmp(g_set_methods[i].ifname, ifname) == 0) {
			slot = i;
			break;
		}
	}
	if (slot >= 0) {
		(void)strlcpy(g_set_methods[slot].ifname, ifname,
		    sizeof(g_set_methods[slot].ifname));
		g_set_methods[slot].info = *info;
	}
	pthread_mutex_unlock(&g_workers_lock);
	if (slot < 0)
		return (ipconfig_status_resource_unavailable_e);

	xlog("set(%s) %s", ifname, ipconfig_method_string(info->method));
	service_reconfigure(ifname);
	return (ipconfig_status_success_e);
}

bool
ipconfigd_get_method(const char *ifname, struct ipconfig_method_info *out)
{
	bool hit = false;
	int i;

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_workers[i].active && !g_workers[i].stop &&
		    strcmp(g_workers[i].ifname, ifname) == 0) {
			*out = g_workers[i].method;
			hit = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_workers_lock);
	return (hit);
}

/*
 * The interface departed: configd removed State:/Network/Interface/<if>/Link.
 * On Darwin removing that key ends the service (ipconfigd's
 * kSCPropNetLinkDetaching handling); here it means whatever worker owns the
 * interface — bound, or still in its DHCP exchange — must unwind, and the
 * interface must stop counting as bound.
 *
 * The worker's own teardown already deconfigures, unpublishes, clears
 * bound_state and re-elects the primary, but it only runs once the worker
 * notices `stop`: an in-flight exchange first has to finish its retransmit
 * ladder. Clearing the binding and re-electing here as well means the
 * primary moves off a vanished interface now rather than whenever its worker
 * gets round to it. Both sides are idempotent, so the double clear is
 * harmless.
 */
static void
on_link_departed(const char *ifname)
{
	struct sc_publish *pub;
	bool had_worker, was_bound;

	had_worker = dhcp_worker_stop(ifname);
	was_bound = bound_state_is_bound(ifname);
	/*
	 * Nothing to tear down. Also the path for an AirPort key that arrived
	 * before its VAP's Link key, which the watcher reports the same way.
	 */
	if (!had_worker && !was_bound)
		return;

	xlog("link-gone(%s) — interface departed; %s", ifname,
	    had_worker ? "unwinding its DHCP worker" : "no worker");
	if (!was_bound)
		return;

	bound_state_clear(ifname);
	pub = sc_publish_open("ipconfigd");
	if (pub != NULL) {
		(void)sc_publish_update_primary(pub);
		sc_publish_close(pub);
	}
	resolv_conf_sync();
}

/* `ipconfig set` (#59): the interface names a set may act on. */
static ipconfig_status_t
set_check_ifname(const char *ifname)
{
	if (ifname[0] == '\0' || if_nametoindex(ifname) == 0)
		return (ipconfig_status_interface_does_not_exist_e);
	if (strncmp(ifname, "lo", 2) == 0)
		return (ipconfig_status_invalid_operation_e);
	return (ipconfig_status_success_e);
}

/*
 * link-watch callback — invoked on a libdispatch worker thread for every
 * change to an interface's Link entity (State:/Network/Interface/<if>/Link),
 * published by KernelEventMonitor from PF_ROUTE. `active` reflects the current
 * link state. This is the Apple-shaped trigger that replaced the hwregd attach
 * subscription.
 *
 * Four cases:
 *   - Link key GONE (SC_LINK_GONE): the interface departed. Unwind its worker,
 *     clear its binding and re-elect the primary — see on_link_departed().
 *     Before this the watcher simply ignored the missing key, so a bound or
 *     in-flight worker kept its address and route on an interface that no
 *     longer existed (#341).
 *   - link DOWN on an interface we are BOUND on: the link we hold a lease over
 *     just went away (unplugged, or walked out of WLAN range). Signal that
 *     interface's worker to unwind; it deconfigures the address and default
 *     route, clears bound_state, re-elects the primary, and frees its slot.
 *     Before this existed the daemon simply kept the dead lease and its dead
 *     default route forever (#39).
 *   - link DOWN otherwise: the interface is present but its link has not come
 *     up. Bring it administratively up so its link can negotiate. This is the
 *     critical path for a NIC that ARRIVES after startup — e.g. an auto-loaded
 *     driver kext (#219), or a wlan0 VAP that wland clones at runtime:
 *     ipconfigd's startup scan never saw it, so nothing else admin-ups it, and
 *     without IFF_UP the link stays down forever and it is never DHCP'd.
 *     iface_bring_up is idempotent, so re-fires are harmless.
 *   - link UP: start a DHCP worker for this interface, unless one is already
 *     running for it. Note the guard is now PER-INTERFACE. It used to be a
 *     global "is any DHCP in flight, or is anything at all bound?", which meant
 *     the first interface to link up was the only one that could ever bind
 *     (#38) — em0 would win the race and wlan0 was ignored for the daemon's
 *     lifetime, no matter how correct the WLAN gate was.
 *
 * The two non-teardown cases apply only to an interface whose method
 * ipconfigd serves (#58). One configured NONE — or with a method we do not
 * serve — is neither admin-upped nor DHCP'd: this is what lets an
 * administrator configure an interface by hand without the daemon fighting
 * them. Link UP starts the interface's configured service, DHCP or Manual.
 */
static void
on_link_event(const char *ifname, int active, uint32_t lease_cap_secs)
{
	struct ipconfig_method_info method;
	int i;

	/* loopback never carries a DHCP service; the watcher filters lo0 too. */
	if (strncmp(ifname, "lo", 2) == 0)
		return;

	if (active == SC_LINK_GONE) {
		on_link_departed(ifname);
		return;
	}

	if (!active) {
		/*
		 * Lost the link on an interface we hold a lease over — tear it
		 * down (#39). The worker does the actual work on its own
		 * thread; we only set its stop flag.
		 */
		if (bound_state_is_bound(ifname)) {
			xlog("link-down(%s) — bound; unwinding its DHCP worker",
			    ifname);
			if (!dhcp_worker_stop(ifname)) {
				/*
				 * Bound with no worker: can't happen, since the
				 * worker owns the binding for its whole life. If
				 * it somehow does, don't leave a dead lease in
				 * the store.
				 */
				xlog("link-down(%s) — bound but no worker; "
				    "clearing state directly", ifname);
				bound_state_clear(ifname);
				resolv_conf_sync();
			}
			return;
		}

		/* Configured to be left alone (#58): not even admin-up. */
		method_lookup(ifname, &method);
		if (!ipconfig_method_is_served(method.method))
			return;

		/* Present but link down — admin-up so the link can negotiate
		 * (the late-arriving-NIC onboarding path, #219).
		 *
		 * Recovery for the lost-wakeup race (#250): KEM publishes Active:0
		 * then Active:1 in quick succession for a late NIC. configd sends a
		 * watcher wakeup only on the empty->non-empty edge, so if the
		 * Active:1 _configset is demuxed by configd's single serve thread
		 * between our wakeup and our notifychanges drain, no second wakeup
		 * is sent and our callout reads the store one instant too early
		 * (Active:0). The Active:1 value then sits in the store with no
		 * pending notification and we would wait forever. The store *value*
		 * is authoritative even when the *wakeup* is not, so poll it a few
		 * times after admin-up and recover by re-driving the active path.
		 * This also covers a configd full-queue silent send-drop and any
		 * future transport change — it does not depend on the wakeup at all. */
		if (iface_bring_up(ifname) != 0) {
			xlog("link-seen(%s) — could not bring up: %s", ifname,
			    strerror(errno));
			return;
		}
		xlog("link-seen(%s) — brought admin-up; polling link state",
		    ifname);
		for (i = 0; i < 6; i++) {
			struct timespec ts = { .tv_sec = 0,
			    .tv_nsec = 500 * 1000 * 1000 };	/* 500ms */

			(void)nanosleep(&ts, NULL);
			if (link_active_in_store(ifname)) {
				xlog("link-seen(%s) — store shows Active after "
				    "admin-up; recovering missed wakeup, DHCP",
				    ifname);
				on_link_event(ifname, 1, lease_cap_secs);
				return;
			}
		}
		xlog("link-seen(%s) — still down after admin-up poll; "
		    "awaiting Active wakeup", ifname);
		return;
	}

	/*
	 * Link up. Per-interface guard: dhcp_worker_start() is a no-op if this
	 * interface already has a worker (a duplicate fire, or the initial scan
	 * racing a real event). Other interfaces are unaffected — that is the
	 * whole point of #38.
	 */
	if (dhcp_worker_start(ifname, lease_cap_secs)) {
		method_lookup(ifname, &method);
		xlog("link-active(%s) — started %s worker", ifname,
		    ipconfig_method_string(method.method));
	}
}

/* Is `hw` the hardware address of one of our own interfaces? */
static bool
is_our_hwaddr(const uint8_t *hw, int hw_len)
{
	struct ifaddrs *ifa, *p;
	bool ours = false;

	if (getifaddrs(&ifa) != 0)
		return (false);
	for (p = ifa; p != NULL && !ours; p = p->ifa_next) {
		const struct sockaddr_dl *dl;

		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_LINK)
			continue;
		dl = (const struct sockaddr_dl *)(const void *)p->ifa_addr;
		ours = dl->sdl_alen == hw_len &&
		    memcmp(LLADDR(dl), hw, (size_t)hw_len) == 0;
	}
	freeifaddrs(ifa);
	return (ours);
}

/* IPv6DuplicatedAddress for `addr` on `ifname` (#349). */
static void
on_v6_duplicated(const char *ifname, const struct in6_addr *addr)
{
	struct dhcp_worker *w;
	struct ra_router *rtr;
	struct in6_addr router;
	unsigned ifindex;
	char a[INET6_ADDRSTRLEN];

	(void)inet_ntop(AF_INET6, addr, a, sizeof(a));
	pthread_mutex_lock(&g_workers_lock);
	w = worker_find_locked(ifname);
	if (w != NULL && w->v6_state == V6_PENDING) {
		/* Not published yet: v6_commit() checks this. */
		w->v6_dup = true;
		w->v6_dup_addr = *addr;
		pthread_mutex_unlock(&g_workers_lock);
		xlog("%s: %s reported duplicated while being installed", ifname,
		    a);
		return;
	}
	if (w == NULL || w->v6_state != V6_ACTIVE ||
	    !IN6_ARE_ADDR_EQUAL(&w->v6_addr, addr)) {
		pthread_mutex_unlock(&g_workers_lock);
		xlog("%s: %s reported duplicated — not an address we "
		    "configured; ignoring", ifname, a);
		return;
	}
	rtr = w->v6_rtr;
	router = w->v6_router;
	ifindex = w->v6_ifindex;
	w->v6_rtr = NULL;
	w->v6_state = V6_NONE;
	pthread_mutex_unlock(&g_workers_lock);
	v6_withdraw(ifname, addr, &router, ifindex, rtr);
}

/*
 * IPv4ARPCollision: a host at `hw` claims `ip` on `ifname` (#349). Only a
 * claim on the address this interface's service holds matters. Defend it,
 * or give it up — the worker's teardown deconfigures, and its retry loop
 * declines (DHCP) and runs the method again. Darwin: dhcp.c / manual.c
 * IFEventID_arp_collision_e and ServiceDefendIPv4Address.
 */
static void
on_arp_collision(const char *ifname, struct in_addr ip, const uint8_t *hw,
    int hw_len)
{
	struct dhcp_worker *w;
	uint32_t bound;
	time_t now;
	bool defend = false, manual;
	int count;
	char a[INET_ADDRSTRLEN], h[3 * 16];
	int i;

	/* bootp's arp_collision(): no hardware address, or our own, is no claim. */
	if (hw_len <= 0 || is_our_hwaddr(hw, hw_len))
		return;
	if (!bound_state_get_addr(ifname, &bound) || bound != ip.s_addr)
		return;

	(void)inet_ntop(AF_INET, &ip, a, sizeof(a));
	h[0] = '\0';
	for (i = 0; i < hw_len && i < 16; i++)
		(void)snprintf(h + 3 * i, sizeof(h) - 3 * (size_t)i, "%s%02x",
		    i == 0 ? "" : ":", hw[i]);

	now = monotonic_secs();
	pthread_mutex_lock(&g_workers_lock);
	w = worker_find_locked(ifname);
	if (w == NULL || w->stop) {
		pthread_mutex_unlock(&g_workers_lock);
		return;
	}
	manual = w->method.method == ipconfig_method_manual_e;
	if (now - w->v4_assigned > DEFEND_IP_ADDRESS_INTERVAL_SECS) {
		/* A quiet interval means the last defence held: count afresh. */
		if (w->conflict_count > 0 && now - w->conflict_time >
		    DEFEND_IP_ADDRESS_INTERVAL_SECS)
			w->conflict_count = 0;
		w->conflict_time = now;
		w->conflict_count++;
		defend = w->conflict_count <= DEFEND_IP_ADDRESS_COUNT;
	}
	count = w->conflict_count;
	pthread_mutex_unlock(&g_workers_lock);

	if (defend && arp_announce(ifname, ip) == 0) {
		xlog("%s: defending %s against %s %d (of %d)", ifname, a, h,
		    count, DEFEND_IP_ADDRESS_COUNT);
		return;
	}

	pthread_mutex_lock(&g_workers_lock);
	w = worker_find_locked(ifname);
	if (w != NULL && !w->stop) {
		w->conflict = true;
		w->stop = 1;
	} else {
		w = NULL;
	}
	pthread_mutex_unlock(&g_workers_lock);
	if (w != NULL)
		xlog("%s: %s in use by %s — giving it up (%s)", ifname, a, h,
		    manual ? "Manual: retrying later"
		    : "DHCP: declining and starting over");
}

/* Link-watch conflict callback (sc_link_watch.h); on a dispatch worker. */
static void
on_addr_conflict(const char *ifname, int af, const void *addr,
    const uint8_t *hw, int hw_len)
{
	if (af == AF_INET)
		on_arp_collision(ifname, *(const struct in_addr *)addr, hw,
		    hw_len);
	else if (af == AF_INET6)
		on_v6_duplicated(ifname, (const struct in6_addr *)addr);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	uint32_t lease_cap_secs;
	char ifname[IFNAMSIZ] = "";

	(void)argc;
	(void)argv;

	xlog("ipconfigd starting (MIG service + DHCPv4 + RFC 5227 ARP + "
	    "IPv6 RA/SLAAC + link-state DHCP trigger)");

	lease_cap_secs = read_lease_cap_env();
	g_lease_cap_secs = lease_cap_secs;

	/*
	 * sa_flags = 0 — system calls are NOT auto-restarted on
	 * signal. That's deliberate: a SIGTERM/SIGHUP during
	 * dhcp_discover.c's BPF read returns EINTR, the read loop
	 * checks got_term and bails out instead of resuming the
	 * (potentially-multi-second) wait. iter-2 had no such hook.
	 */
	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);

	if (enumerate_interfaces() < 0) {
		xlog("interface enumeration failed; continuing anyway");
		/* not fatal — the daemon still exposes its Mach service */
	}

	/*
	 * iter 5a: the Mach service thread now owns the receive right
	 * for com.apple.IPConfiguration (bootstrap_check_in lives in
	 * mach_service.c). It is spawned up-front so the service is
	 * reachable while DHCP is still in flight — RPC routines
	 * return ipconfig_status_no_server_e until the BOUND lease
	 * lands in bound_state. iter-1 IPCFG-BOOT (ipconfigtest's
	 * bootstrap_look_up) is now answered by launchd's broker, not
	 * a direct send to our checked-in port; the Mach plumbing is
	 * identical from the client side.
	 */
	bound_state_init();
	if (mach_service_start() != 0)
		xlog("mach_service_start failed; RPC off");

	/*
	 * Apple-shaped DHCP trigger. Rather than DHCP unconditionally at
	 * startup (which races a real NIC whose link negotiates a beat
	 * after we scan — the failure mode that left a stock-kernel box
	 * without a lease once hwregd's attach event was removed), we:
	 *
	 *   1. bring the candidate Ethernet administratively up so its
	 *      link starts negotiating, then
	 *   2. watch State:/Network/Interface/<if>/Link and run DHCP only
	 *      when KernelEventMonitor reports the link Active.
	 *
	 * The watch's initial scan + KernelEventMonitor's startup snapshot
	 * make this fire immediately when the link is already up (CI /
	 * SLIRP), and exactly once when it comes up later (real hardware).
	 * DHCP itself (INIT → BOUND, apply_lease, the SCDynamicStore
	 * publish, and the RENEWING/REBINDING lease loop) is unchanged —
	 * see dhcp_run_on_interface; markers IPCFG-BOUND-OK / -STORE-OK /
	 * -DHCP-OK fire from there. A failed run does NOT exit ipconfigd;
	 * the Mach service stays registered and the watch re-arms.
	 */
	/*
	 * Per-interface methods (#58), loaded before anything acts on an
	 * interface. No network preferences — the default — leaves the
	 * table empty and every interface on DHCP.
	 */
	(void)ipconfig_prefs_start(on_prefs_changed);

	if (dhcp_pick_interface(ifname, sizeof(ifname)) == 0) {
		struct ipconfig_method_info method;

		method_lookup(ifname, &method);
		if (!ipconfig_method_is_served(method.method))
			xlog("%s is configured %s; leaving it alone", ifname,
			    ipconfig_method_string(method.method));
		else if (iface_bring_up(ifname) == 0)
			xlog("brought %s up; awaiting link-active to DHCP",
			    ifname);
		else
			xlog("could not bring %s up: %s — relying on link "
			    "watch", ifname, strerror(errno));
	} else {
		xlog("no Ethernet at startup; will DHCP when one links up");
		/* Nothing to configure: don't hold up `ipconfig waitall`. */
		startup_check();
	}

	if (sc_link_watch_start(on_link_event, on_addr_conflict,
	    lease_cap_secs) != 0)
		xlog("IPCFG-BOUND-FAIL: link watch unavailable — DHCP will "
		    "not be triggered");

	/*
	 * Hold the daemon alive. DHCP runs on the link-watch's worker
	 * thread; the main thread just waits for SIGTERM. A bound lease's
	 * renewal loop also runs on that worker thread.
	 */
	while (!got_term) {
		(void)sleep(60);
	}

	xlog("ipconfigd exiting on signal %d", (int)got_term);
	mach_service_join();
	return (0);
}
