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
 * the same path as a lease), or NONE (left entirely alone).
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
 *
 * Nor is a SLAAC address published while it is still tentative (#407):
 * ::/0 and State:/Network/Service/<UUID>/IPv6 wait until configd's
 * State:/Network/Interface/<if>/IPv6 shows DAD has passed, with #342's
 * DAD interval as the bound in case that change never comes.
 *
 * renew requests (#572) — `scutil --renew` posts the notify-only
 * RefreshConfiguration key, as on Darwin, and the link watch hands it to
 * on_link_refresh(): a bound DHCP service renews now, and a service with
 * IPv6 sends a Router Solicitation (bootp's link_refresh, dhcp.c
 * IFEventID_renew_e, rtadv.c IFEventID_renew_e). A Manual service keeps
 * its IPv4 as it is, since manual.c has no renew case either.
 *
 * service IDs (#568) — a service publishes under its SCPreferences service
 * ID, as Darwin's does, so its Setup: and State: keys line up. `ipconfig
 * set` services are <Method>-<if>, as bootp's IFState_service_add() names
 * one with no service ID. Worker slots stay keyed by interface, so a
 * service whose ID changes unwinds and starts again.
 *
 * IPv6 methods (#423) — IPv4 and IPv6 are now two services on an interface,
 * as on Darwin. The workers here run IPv4 only. v6_service.c runs IPv6 under
 * the service's IPv6 ConfigMethod: Automatic, Manual, LinkLocal or off.
 * `ipconfig set` changes one family at a time. An IPv6 service starts after
 * the interface's first IPv4 result and stops with the link, but otherwise
 * does not care what IPv4 is doing.
 *
 * link-local and DHCP retry (#426) — a LinkLocal service is an RFC 3927
 * address (linklocal.c) through the same bound path as Manual. A DHCP service
 * with no lease no longer exits: it runs Darwin's ladder and tries INIT again
 * every minute, and from its fifth DISCOVER holds a link-local address that a
 * lease replaces (dhcp.c's linklocal_service_change()). Link-local bindings
 * stand aside in the primary election, and they are never defended: another
 * claim on one means picking a new address, as linklocal.c does.
 *
 * INFORM, BOOTP and Failover (#569) — INFORM is a Manual address that asks a
 * DHCP server for the rest, and keeps the address with no answer. BOOTP is a
 * single request with no lease, retried two minutes after giving up. Failover
 * is a Manual address with no router that starts at a random moment and can
 * expire. Darwin defends none of them: another claim makes BOOTP fail and
 * Failover start over, and INFORM only logs it.
 *
 * services, not links (#570) — ipconfigd configures what the network
 * services in Setup:/Network/Service/* say, and nothing else: an interface
 * no service names is left alone, down if it was down. Services come from
 * PreferencesMonitor's flatten of the current set (ipconfig_prefs.c), which
 * also carries ServiceOrder and PrimaryRank, the inputs to the primary
 * election (bound_state.c). Link events still start and stop a running
 * service, and `ipconfig set` still works. Each link type allows what
 * Darwin's config_method_start() allows (method_allowed_on_link()).
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
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
#include "dns_config.h"
#include "dns_configuration.h"
#include "nwi_configuration.h"
#include "ipconfig_method.h"
#include "ipconfig_prefs.h"
#include "ipconfigd.h"
#include "lease_loop.h"
#include "linklocal.h"
#include "mach_service.h"
#include "sc_link_watch.h"
#include "sc_publish.h"
#include "v6_service.h"

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
	volatile sig_atomic_t	renew;		/* --renew: renew the lease now */
	uint32_t		lease_cap_secs;
	bool			ready;		/* bound or given up: startup */
	struct ipconfig_method_info method;	/* what this worker runs (#58) */
	char			service_id[SC_PUBLISH_SERVICE_ID_LEN];
						/* empty: no service (#568) */
	bool			restart;	/* method changed: rerun on exit */

	/*
	 * Address conflicts (#349). `conflict` is set with `stop` when an
	 * ARP collision makes us give the IPv4 address up; the worker then
	 * declines / waits and runs its method again instead of exiting.
	 * The rest is ServiceDefendIPv4Address's bookkeeping (Apple's
	 * ServiceIPv4 ip_assigned_time / ip_conflict_*).
	 */
	bool			conflict;
	volatile sig_atomic_t	ll_conflict;	/* the fallback's address: #426 */
	time_t			v4_assigned;	/* monotonic secs at bind */
	time_t			conflict_time;
	int			conflict_count;
};

/*
 * Darwin's defaults (bootp ipconfigd.c): defend an address at most
 * DEFEND_IP_ADDRESS_COUNT times, never in its first
 * DEFEND_IP_ADDRESS_INTERVAL_SECS, with the count starting over after that
 * long without a conflict; a Manual address given up is retried after
 * MANUAL_CONFLICT_RETRY_INTERVAL_SECS. DHCP_DECLINE_WAIT_SECS is RFC 2131
 * §3.1.5's minimum wait between DHCPDECLINE and a fresh DISCOVER.
 */
#define	DEFEND_IP_ADDRESS_INTERVAL_SECS		10
#define	DEFEND_IP_ADDRESS_COUNT			3
#define	MANUAL_CONFLICT_RETRY_INTERVAL_SECS	300
#define	DHCP_DECLINE_WAIT_SECS			10

/*
 * Darwin's defaults (#426): DHCPAllocateLinkLocalAtRetryCount, and dhcp.c's
 * INIT_RETRY_INTERVAL_SECS.
 */
#define	DHCP_ALLOCATE_LINKLOCAL_AT_RETRY_COUNT	4
#define	DHCP_INIT_RETRY_INTERVAL_SECS		60

/* bootp.c's RETRY_INTERVAL_SECS and failover.c's retry after a conflict. */
#define	BOOTP_RETRY_INTERVAL_SECS		(2 * 60)
#define	FAILOVER_CONFLICT_RETRY_SECS		10

static pthread_mutex_t	g_workers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dhcp_worker g_workers[BOUND_MAX_IF];

/*
 * `ipconfig set` methods (#58/#59), which win over SCPreferences until
 * the next `set` or an ipconfigd restart — in memory only, like Apple's.
 * A NONE here is what holds an interface the administrator switched
 * off, so a link flap does not quietly re-DHCP it. Per family (#423): a
 * `set` of one family leaves the other family's method where it was.
 * Guarded by g_workers_lock.
 */
static struct {
	char				ifname[IFNAMSIZ];
	bool				have_v4;
	struct ipconfig_method_info	info;
	bool				have_v6;
	struct ipconfig_v6_method_info	v6;
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

	/* Until PreferencesMonitor has spoken, "no services" means nothing. */
	if (!ipconfig_prefs_loaded())
		return;
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

/*
 * `ifname`'s renew-now flag, for the lease loop. The slot outlives the
 * worker thread calling this, so the pointer stays good for the loop.
 */
static volatile sig_atomic_t *
worker_renew_flag(const char *ifname)
{
	struct dhcp_worker *w;
	volatile sig_atomic_t *flag = NULL;

	pthread_mutex_lock(&g_workers_lock);
	if ((w = worker_find_locked(ifname)) != NULL)
		flag = &w->renew;
	pthread_mutex_unlock(&g_workers_lock);
	return (flag);
}

/* Take `ifname`'s pending fallback-address conflict, if any (#426). */
static bool
worker_take_ll_conflict(const char *ifname)
{
	struct dhcp_worker *w;
	bool conflict = false;

	pthread_mutex_lock(&g_workers_lock);
	if ((w = worker_find_locked(ifname)) != NULL && w->ll_conflict) {
		w->ll_conflict = 0;
		conflict = true;
	}
	pthread_mutex_unlock(&g_workers_lock);
	return (conflict);
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
 * on `ifname`: record it, publish it, elect the primary, open the door for
 * the IPv6 service, park, then tear it all down. Shared by the DHCP, Manual
 * and LinkLocal services (#58, #426), which differ only in how they get
 * `lease` and in whether parking means a renewal loop. `saved` is the ACK for
 * a DHCP service and NULL otherwise (#430). Runs on the interface's worker
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
service_run_bound(const char *ifname, struct dhcp_lease *lease,
    const struct saved_pkt *saved, uint32_t lease_cap_secs,
    volatile sig_atomic_t *stop, ipconfig_method_t method)
{
	bool dhcp = (method == ipconfig_method_dhcp_e);
	struct sc_publish *pub = NULL;
	char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN];
	char r[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];

	(void)inet_ntop(AF_INET, &lease->addr, a, sizeof(a));
	(void)inet_ntop(AF_INET, &lease->netmask, m, sizeof(m));
	(void)inet_ntop(AF_INET, &lease->router, r, sizeof(r));
	(void)inet_ntop(AF_INET, &lease->server, s, sizeof(s));
	if (dhcp)
		xlog("bound: iface=%s addr=%s netmask=%s router=%s "
		    "server=%s lease=%us",
		    ifname, a, m, r, s, (unsigned)lease->lease_time);
	else
		xlog("configured: iface=%s method=%s addr=%s netmask=%s "
		    "router=%s", ifname, ipconfig_method_string(method), a, m,
		    r);
	/*
	 * Make the lease visible to the Mach service worker before the
	 * BOUND marker fires — so any client racing IPCFG-RPC-OK against
	 * IPCFG-BOUND-OK sees the address it expects.
	 */
	if (method == ipconfig_method_linklocal_e)
		bound_state_set_linklocal(ifname, lease);
	else
		bound_state_set(ifname, lease);
	if (saved != NULL)
		bound_state_set_packet(ifname, saved);
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
	/* What an INFORM or BOOTP reply carried; its marker is DHCP's alone. */
	if (pub != NULL && !dhcp && saved != NULL)
		(void)sc_publish_dhcp(pub, ifname, lease);
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
	 * (#41), then merge this service's DNS and render the global DNS
	 * from whoever won (#425). Doing DNS off the primary rather than
	 * off `lease` is what stops a wlan0 binding from stomping a wired
	 * em0's resolvers.
	 */
	if (pub != NULL)
		(void)sc_publish_update_primary(pub);
	dns_config_update(ifname);

	/* IPv4 is configured: this service no longer holds up `waitall`. */
	startup_mark_ready(ifname);

	/*
	 * The first IPv4 result on this interface: its IPv6 service may start
	 * (#423). It starts here and not before because CI reads IPCFG-RA
	 * after IPCFG-BOUND.
	 *
	 * The boot test also reads IPCFG-RA before IPCFG-RENEW, in log order.
	 * IPv4 and IPv6 used to share a thread, so a lease loop only started
	 * once IPv6 had answered; hold it for that first answer here. This is
	 * only the renewal clock, once per bind and bounded: the address and
	 * readiness above have already happened, and IPv6 has no other say.
	 */
	v6_service_v4_result(ifname);
	if (dhcp)
		v6_service_wait_first_result(ifname, stop);

	/*
	 * Parks here for the life of the lease. Another method has nothing to
	 * renew; it holds until the link drops, its method changes, or the
	 * daemon exits. A Failover address can also run out of time, with
	 * lease_time carrying FailoverAddressTimeout (#569).
	 */
	if (dhcp) {
		(void)lease_loop_run(ifname, lease, pub, lease_cap_secs,
		    worker_renew_flag(ifname), stop);
	} else {
		time_t expires = 0;

		if (method == ipconfig_method_failover_e &&
		    lease->lease_time != 0)
			expires = monotonic_secs() + lease->lease_time;
		while (!got_term && !(stop != NULL && *stop) &&
		    (expires == 0 || monotonic_secs() < expires))
			(void)sleep(1);
	}

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
		    : "link down, method changed, address in use or expired");
		(void)deconfigure_lease(ifname, lease);
		if (pub != NULL)
			(void)sc_publish_remove(pub, ifname);
		bound_state_clear(ifname);
		if (pub != NULL)
			(void)sc_publish_update_primary(pub);
		/* IPv4 only: IPv6 has its own service (#423). */
		dns_config_update(ifname);
	}

	if (pub != NULL)
		sc_publish_close(pub);
}

/*
 * The link-local address a DHCP service holds while it has no lease (#426):
 * dhcp.c's linklocal_service_change(LINKLOCAL_ALLOCATE), from the worker
 * thread instead of a child service. Published under the DHCP service's own
 * ID, with no marker and no router, and flagged in bound_state so it stands
 * aside in the election.
 */
struct dhcp_fallback {
	const char		*ifname;
	volatile sig_atomic_t	*stop;
	bool			no_server;	/* reached try 5 */
	bool			installed;
	struct dhcp_lease	lease;
	struct linklocal_probe	probe;
};

static void
fallback_install(struct dhcp_fallback *f, struct in_addr addr)
{
	struct sc_publish *pub;
	char a[INET_ADDRSTRLEN];

	linklocal_lease(&f->lease, addr);
	if (apply_lease(f->ifname, &f->lease) != 0) {
		xlog("apply_lease(%s) failed for link-local fallback",
		    f->ifname);
		return;
	}
	bound_state_set_linklocal(f->ifname, &f->lease);
	worker_v4_assigned(f->ifname);
	f->installed = true;
	(void)inet_ntop(AF_INET, &addr, a, sizeof(a));
	xlog("%s: no DHCP server — link-local fallback %s", f->ifname, a);

	pub = sc_publish_open("ipconfigd");
	if (pub != NULL) {
		(void)sc_publish_ipv4(pub, f->ifname, &f->lease);
		(void)sc_publish_update_primary(pub);
		sc_publish_close(pub);
	}
	dns_config_update(f->ifname);
}

static void
fallback_remove(struct dhcp_fallback *f)
{
	struct sc_publish *pub;

	(void)deconfigure_lease(f->ifname, &f->lease);
	pub = sc_publish_open("ipconfigd");
	if (pub != NULL)
		(void)sc_publish_remove(pub, f->ifname);
	bound_state_clear(f->ifname);
	if (pub != NULL) {
		(void)sc_publish_update_primary(pub);
		sc_publish_close(pub);
	}
	dns_config_update(f->ifname);
	f->installed = false;
}

/*
 * One round of the fallback on the worker thread: give up an address another
 * host claimed (no defence, as linklocal.c's arp_collision case), then probe
 * one candidate if the service needs one and has none. A probe blocks for a
 * few seconds and is the only thing here that does.
 */
static void
fallback_step(struct dhcp_fallback *f)
{
	struct in_addr addr;

	if (worker_take_ll_conflict(f->ifname) && f->installed) {
		xlog("%s: link-local fallback address in use by another host",
		    f->ifname);
		fallback_remove(f);
		linklocal_forget(f->ifname);
		linklocal_probe_init(&f->probe, f->ifname);
	}
	if (!f->no_server || f->installed || got_term ||
	    (f->stop != NULL && *f->stop))
		return;
	if (linklocal_probe_next(&f->probe, f->ifname, &addr) == 1 &&
	    !got_term && !(f->stop != NULL && *f->stop))
		fallback_install(f, addr);
}

/*
 * dhcp_lease_acquire's on_try. From dhcp.c's INIT: at try 5 (about 15 s) a
 * service with no server allocates a link-local address, and that is also
 * IPv4's first result as far as the IPv6 service goes (#423).
 */
static void
dhcp_on_try(int try, void *arg)
{
	struct dhcp_fallback *f = arg;

	if (try >= DHCP_ALLOCATE_LINKLOCAL_AT_RETRY_COUNT + 1 &&
	    !f->no_server) {
		f->no_server = true;
		v6_service_v4_result(f->ifname);
	}
	fallback_step(f);
}

/*
 * dhcp_run_on_interface — the DHCP service: INIT → BOUND on `ifname`, then
 * service_run_bound(), until stopped. With no lease it does not exit (#426):
 * dhcp_no_server() tries INIT again after a minute, forever, and meanwhile
 * the fallback keeps a link-local address.
 *
 * IPCFG-BOUND-FAIL is logged once per acquire, however many retries that
 * acquire goes through, and never for a stop.
 */
static void
dhcp_run_on_interface(const char *ifname, uint32_t lease_cap_secs,
    volatile sig_atomic_t *stop)
{
	struct dhcp_fallback f;
	struct dhcp_lease lease;
	struct saved_pkt saved;
	volatile sig_atomic_t *renew;
	bool reported = false;

	(void)memset(&f, 0, sizeof(f));
	f.ifname = ifname;
	f.stop = stop;
	linklocal_probe_init(&f.probe, ifname);
	renew = worker_renew_flag(ifname);

	xlog("selected interface for DHCPv4: %s", ifname);
	for (;;) {
		int r;
		time_t until;

		r = dhcp_lease_acquire(ifname, &lease, &saved, stop,
		    dhcp_on_try, &f);

		/*
		 * dhcp_lease_acquire gives up on `stop`, but a stop can still
		 * land between the ACK and here. Do not install a lease we are
		 * about to have to rip back out. Nor is a stop a failure, so
		 * no marker either (#410, #417).
		 */
		if (got_term || (stop != NULL && *stop)) {
			if (r == DHCP_ACQUIRE_OK)
				xlog("%s: link/daemon went away during DHCP — "
				    "discarding lease", ifname);
			break;
		}

		if (r == DHCP_ACQUIRE_OK) {
			if (apply_lease(ifname, &lease) == 0) {
				/*
				 * The lease replaces the fallback (dhcp.c,
				 * LINKLOCAL_NO_ALLOCATE). service_run_bound()
				 * overwrites its bound_state and publish.
				 */
				linklocal_remove_addresses(ifname, lease.addr);
				f.installed = false;
				f.no_server = false;
				linklocal_probe_init(&f.probe, ifname);
				/* A request from before this lease is answered. */
				if (renew != NULL)
					*renew = 0;
				service_run_bound(ifname, &lease, &saved,
				    lease_cap_secs, stop,
				    ipconfig_method_dhcp_e);

				/*
				 * Gave the address up to another host (#349):
				 * it is off the interface now, so tell the
				 * server, as dhcp.c's arp_collision path does
				 * with dhcp_decline(). The worker restarts DHCP
				 * after the RFC wait.
				 */
				if (!got_term && worker_conflicted(ifname))
					(void)dhcp_decline(ifname, &lease);
				if (got_term || (stop != NULL && *stop))
					break;

				/* Expired: back to INIT, a new acquire. */
				xlog("%s: lease ended — back to INIT", ifname);
				reported = false;
				continue;
			}
			xlog("apply_lease(%s) failed", ifname);
			r = DHCP_ACQUIRE_ERROR;
		}

		/*
		 * Giving up is IPv4's result for readiness and for the IPv6
		 * service, as the worker's exit used to be; retrying does not
		 * make either wait again.
		 */
		if (!reported) {
			reported = true;
			xlog("IPCFG-BOUND-FAIL");
			startup_mark_ready(ifname);
			v6_service_v4_result(ifname);
		}

		switch (r) {
		case DHCP_ACQUIRE_NO_ACK:
			/* dhcp_select() goes straight back to INIT. */
			until = monotonic_secs();
			break;
		case DHCP_ACQUIRE_DECLINED:
			until = monotonic_secs() + DHCP_DECLINE_WAIT_SECS;
			break;
		default:
			until = monotonic_secs() + DHCP_INIT_RETRY_INTERVAL_SECS;
			break;
		}
		xlog("%s: no lease — running INIT again in %lds", ifname,
		    (long)(until - monotonic_secs()));
		while (monotonic_secs() < until && !got_term &&
		    !(stop != NULL && *stop)) {
			/* dhcp.c's renew case on an unbound service: INIT now. */
			if (renew != NULL && *renew) {
				*renew = 0;
				break;
			}
			fallback_step(&f);
			(void)sleep(1);
		}
		if (got_term || (stop != NULL && *stop))
			break;
	}

	/* As for a lease, SIGTERM leaves the address installed. */
	if (f.installed && !got_term)
		fallback_remove(&f);
}

/*
 * manual_run_on_interface — the Manual service (#58): install the
 * configured address, mask and router through the same apply / publish
 * path a DHCP lease takes, then service_run_bound(). Its DNS comes from
 * the service's Setup DNS entity through dns_config.c, not the lease (#425).
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

	(void)memset(&lease, 0, sizeof(lease));
	lease.addr = info->addr;
	lease.netmask = info->mask;
	lease.router = info->router;

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
	service_run_bound(ifname, &lease, NULL, 0, stop,
	    ipconfig_method_manual_e);
}

/* Sleep up to `secs`, a second at a time. False if stopped meanwhile. */
static bool
service_wait(unsigned secs, volatile sig_atomic_t *stop)
{
	unsigned i;

	for (i = 0; i < secs && !got_term && !(stop != NULL && *stop); i++)
		(void)sleep(1);
	return (!got_term && !(stop != NULL && *stop));
}

/*
 * A method that has not configured an address but is carrying on: it counts
 * as given up for readiness and for the IPv6 service, as a worker that
 * exited used to (service_publish_failure() on Darwin). Repeats are harmless.
 */
static void
service_gave_up(const char *ifname)
{
	startup_mark_ready(ifname);
	v6_service_v4_result(ifname);
}

/*
 * inform_run_on_interface — the INFORM service (#569), dhcp.c's inform_*():
 * Manual's probe and install, then DHCPINFORM from the address. The reply
 * can supply the router, the DNS and, if none is configured, the mask. With
 * no reply the address stays and the service is configured as it stands.
 */
static void
inform_run_on_interface(const char *ifname,
    const struct ipconfig_method_info *info, volatile sig_atomic_t *stop)
{
	struct dhcp_lease lease, reply;
	struct saved_pkt saved;
	char a[INET_ADDRSTRLEN];
	int r;

	(void)memset(&lease, 0, sizeof(lease));
	lease.addr = info->addr;
	lease.netmask = info->mask.s_addr != INADDR_ANY ? info->mask :
	    ipconfig_classful_mask(info->addr);

	(void)inet_ntop(AF_INET, &lease.addr, a, sizeof(a));
	xlog("selected interface for INFORM: %s (%s)", ifname, a);
	while (arp_probe(ifname, lease.addr) == 1) {
		xlog("%s: %s is in use by another host — trying again in %ds",
		    ifname, a, MANUAL_CONFLICT_RETRY_INTERVAL_SECS);
		service_gave_up(ifname);
		if (!service_wait(MANUAL_CONFLICT_RETRY_INTERVAL_SECS, stop))
			return;
	}
	if (got_term || (stop != NULL && *stop))
		return;
	if (apply_lease(ifname, &lease) != 0) {
		xlog("apply_lease(%s) failed for INFORM %s", ifname, a);
		return;
	}

	r = dhcp_inform(ifname, lease.addr, &reply, &saved, stop);
	if (got_term || (stop != NULL && *stop)) {
		if (!got_term)
			(void)deconfigure_lease(ifname, &lease);
		return;
	}
	if (r == DHCP_ACQUIRE_OK) {
		struct in_addr installed = lease.netmask;

		/* What the reply offers, around the address that is ours. */
		reply.addr = lease.addr;
		reply.netmask = (info->mask.s_addr == INADDR_ANY &&
		    reply.netmask.s_addr != INADDR_ANY) ? reply.netmask :
		    lease.netmask;
		if (reply.netmask.s_addr != installed.s_addr) {
			(void)deconfigure_lease(ifname, &lease);
			if (apply_lease(ifname, &reply) != 0)
				xlog("apply_lease(%s) failed for INFORM reply",
				    ifname);
		} else if (apply_lease_router(&reply) != 0) {
			xlog("%s: could not add the INFORM router", ifname);
		}
		lease = reply;
	} else {
		xlog("%s: no reply to DHCPINFORM — keeping %s", ifname, a);
	}
	service_run_bound(ifname, &lease, r == DHCP_ACQUIRE_OK ? &saved : NULL,
	    0, stop, ipconfig_method_inform_e);
}

/*
 * bootp_run_on_interface — the BOOTP service (#569), bootp.c: one exchange,
 * a probe of the address, then installed with no lease and held until the
 * link or method changes. No reply, or an address in use, tries again in two
 * minutes.
 */
static void
bootp_run_on_interface(const char *ifname, volatile sig_atomic_t *stop)
{
	struct dhcp_lease lease;
	struct saved_pkt saved;
	char a[INET_ADDRSTRLEN];

	xlog("selected interface for BOOTP: %s", ifname);
	for (;;) {
		int r;

		r = dhcp_bootp(ifname, &lease, &saved, stop);
		if (got_term || (stop != NULL && *stop))
			return;
		if (r == DHCP_ACQUIRE_OK) {
			(void)inet_ntop(AF_INET, &lease.addr, a, sizeof(a));
			if (arp_probe(ifname, lease.addr) == 1) {
				xlog("%s: BOOTP address %s is in use by "
				    "another host", ifname, a);
			} else if (got_term || (stop != NULL && *stop)) {
				return;
			} else {
				if (lease.netmask.s_addr == INADDR_ANY)
					lease.netmask =
					    ipconfig_classful_mask(lease.addr);
				if (apply_lease(ifname, &lease) == 0) {
					service_run_bound(ifname, &lease,
					    &saved, 0, stop,
					    ipconfig_method_bootp_e);
					return;
				}
				xlog("apply_lease(%s) failed for BOOTP %s",
				    ifname, a);
			}
		}
		xlog("%s: no BOOTP address — trying again in %ds", ifname,
		    BOOTP_RETRY_INTERVAL_SECS);
		service_gave_up(ifname);
		if (!service_wait(BOOTP_RETRY_INTERVAL_SECS, stop))
			return;
	}
}

/*
 * failover_run_on_interface — the Failover service (#569), failover.c: a
 * Manual address with no router, after a random 0-4 s so that two peers
 * starting together do not both win. In use: try again after 10 s. When
 * FailoverAddressTimeout runs out the address goes and the service idles
 * until the link or its method changes.
 */
static void
failover_run_on_interface(const char *ifname,
    const struct ipconfig_method_info *info, volatile sig_atomic_t *stop)
{
	struct dhcp_lease lease;
	char a[INET_ADDRSTRLEN];

	(void)memset(&lease, 0, sizeof(lease));
	lease.addr = info->addr;
	lease.netmask = info->mask;
	lease.lease_time = info->failover_timeout;

	(void)inet_ntop(AF_INET, &lease.addr, a, sizeof(a));
	xlog("selected interface for Failover: %s (%s)", ifname, a);
	for (;;) {
		struct timespec ts = { .tv_sec = 0,
		    .tv_nsec = (long)arc4random_uniform(1000000000U) };

		(void)nanosleep(&ts, NULL);
		if (!service_wait(arc4random_uniform(5), stop))
			return;
		if (arp_probe(ifname, lease.addr) != 1)
			break;
		xlog("%s: %s is in use by another host — trying again in %ds",
		    ifname, a, FAILOVER_CONFLICT_RETRY_SECS);
		service_gave_up(ifname);
		if (!service_wait(FAILOVER_CONFLICT_RETRY_SECS, stop))
			return;
	}
	if (got_term || (stop != NULL && *stop))
		return;
	if (apply_lease(ifname, &lease) != 0) {
		xlog("apply_lease(%s) failed for Failover %s", ifname, a);
		return;
	}
	service_run_bound(ifname, &lease, NULL, 0, stop,
	    ipconfig_method_failover_e);
	if (got_term || (stop != NULL && *stop))
		return;

	xlog("%s: Failover address timer fired", ifname);
	while (service_wait(1, stop))
		;
}

/*
 * linklocal_run_on_interface — the LinkLocal service (#426): an RFC 3927
 * address, then service_run_bound() as for Manual, with no router and no
 * DNS. linklocal.c probes candidates until one is free; when that has
 * gone on past its initial tries, the service counts as given up for
 * readiness and the IPv6 service, and keeps probing behind the back-off.
 */
static void
linklocal_run_on_interface(const char *ifname, volatile sig_atomic_t *stop)
{
	struct linklocal_probe lp;
	struct dhcp_lease lease;
	struct in_addr addr;
	bool gave_up = false;

	xlog("selected interface for LinkLocal: %s", ifname);
	linklocal_probe_init(&lp, ifname);
	for (;;) {
		int r;

		if (got_term || (stop != NULL && *stop))
			return;
		r = linklocal_probe_next(&lp, ifname, &addr);
		if (r == 1)
			break;
		if (lp.retry_at != 0 && !gave_up && !got_term &&
		    !(stop != NULL && *stop)) {
			gave_up = true;
			startup_mark_ready(ifname);
			v6_service_v4_result(ifname);
		}
		if (lp.retry_at > monotonic_secs())
			(void)sleep(1);
	}
	if (got_term || (stop != NULL && *stop))
		return;

	linklocal_lease(&lease, addr);
	if (apply_lease(ifname, &lease) != 0) {
		xlog("apply_lease(%s) failed for LinkLocal", ifname);
		return;
	}
	service_run_bound(ifname, &lease, NULL, 0, stop,
	    ipconfig_method_linklocal_e);
	if (!got_term && worker_conflicted(ifname))
		linklocal_forget(ifname);
}

/*
 * Worker thread body — one per interface that has linked up. Runs the
 * whole DHCP + lease + teardown lifecycle, then releases its slot so a
 * later link event on the same interface can start a fresh one. That
 * release is the other half of the re-arm: the slot, not a global flag,
 * is now what says "a run is in flight on this interface".
 */
static void	service_start(const char *ifname);

/* How long a method waits to run again after giving its address up (#349). */
static unsigned
conflict_retry_secs(ipconfig_method_t m)
{
	switch (m) {
	case ipconfig_method_manual_e:
		return (MANUAL_CONFLICT_RETRY_INTERVAL_SECS);
	case ipconfig_method_bootp_e:
		return (BOOTP_RETRY_INTERVAL_SECS);
	case ipconfig_method_linklocal_e:	/* picks a new address at once */
	case ipconfig_method_failover_e:	/* has its own random start */
		return (0);
	default:
		return (DHCP_DECLINE_WAIT_SECS);
	}
}

/*
 * A service moved here from another interface (#568): Darwin's
 * S_set_service stops it there first. The old worker is already unwinding,
 * and its teardown removes State:/Network/Service/<id>, so wait for it
 * before publishing under the same ID.
 */
static void
service_id_wait_free(const struct dhcp_worker *self)
{
	if (self->service_id[0] == '\0')
		return;
	while (!got_term && !self->stop) {
		bool busy = false;
		int i;

		pthread_mutex_lock(&g_workers_lock);
		for (i = 0; i < BOUND_MAX_IF; i++) {
			if (&g_workers[i] != self && g_workers[i].active &&
			    strcmp(g_workers[i].service_id,
			    self->service_id) == 0)
				busy = true;
		}
		pthread_mutex_unlock(&g_workers_lock);
		if (!busy)
			return;
		(void)sleep(1);
	}
}

static void *
dhcp_worker_main(void *arg)
{
	struct dhcp_worker *w = arg;
	char ifname[IFNAMSIZ];
	struct ipconfig_method_info method;
	bool restart, stopped;

	(void)strlcpy(ifname, w->ifname, sizeof(ifname));
	method = w->method;
	service_id_wait_free(w);
	/* Fix the service ID while the MAC is still readable (#409). */
	sc_publish_service_begin(ifname, w->service_id);
	for (;;) {
		bool conflict;
		unsigned wait, i;

		switch (method.method) {
		case ipconfig_method_manual_e:
			manual_run_on_interface(ifname, &method, &w->stop);
			break;
		case ipconfig_method_linklocal_e:
			linklocal_run_on_interface(ifname, &w->stop);
			break;
		case ipconfig_method_inform_e:
			inform_run_on_interface(ifname, &method, &w->stop);
			break;
		case ipconfig_method_bootp_e:
			bootp_run_on_interface(ifname, &w->stop);
			break;
		case ipconfig_method_failover_e:
			failover_run_on_interface(ifname, &method, &w->stop);
			break;
		default:
			dhcp_run_on_interface(ifname, w->lease_cap_secs,
			    &w->stop);
			break;
		}

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
			w->renew = 0;
		}
		pthread_mutex_unlock(&g_workers_lock);
		if (!conflict || got_term)
			break;
		wait = conflict_retry_secs(method.method);
		xlog("%s: address in use — running %s again in %us", ifname,
		    ipconfig_method_string(method.method), wait);
		for (i = 0; i < wait && !got_term && !w->stop; i++)
			(void)sleep(1);
		if (got_term || w->stop)
			break;
	}

	pthread_mutex_lock(&g_workers_lock);
	restart = w->restart;
	stopped = w->stop != 0;
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
	 * That is an IPv4 result too (#423), unless the worker was stopped:
	 * then the link is down, or a new method is about to produce one.
	 */
	if (!stopped && !got_term)
		v6_service_v4_result(ifname);
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

/* Darwin's ipconfig_method_string() spelling, for an `ipconfig set` service ID. */
static const char *
set_service_method_string(ipconfig_method_t m)
{
	switch (m) {
	case ipconfig_method_manual_e:
		return ("MANUAL");
	case ipconfig_method_dhcp_e:
		return ("DHCP");
	case ipconfig_method_linklocal_e:
		return ("LINKLOCAL");
	case ipconfig_method_bootp_e:
		return ("BOOTP");
	case ipconfig_method_inform_e:
		return ("INFORM");
	case ipconfig_method_failover_e:
		return ("FAILOVER");
	default:
		return ("NONE");
	}
}

/*
 * The IPv4 method `ifname` should run (#58): an `ipconfig set` method
 * first, then its network service in Setup:, and NONE when nothing configures
 * it (#570). `service_id`, if not NULL, gets the ID that service publishes
 * under (SC_PUBLISH_SERVICE_ID_LEN bytes, #568): `ipconfig set`'s
 * <Method>-<if>, the service's own ID, or <Method>-<if> if that is too long
 * to keep. It is empty for no service.
 */
static void
service_lookup(const char *ifname, struct ipconfig_method_info *out,
    char *service_id)
{
	int i;

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_set_methods[i].have_v4 &&
		    strcmp(g_set_methods[i].ifname, ifname) == 0) {
			*out = g_set_methods[i].info;
			if (service_id != NULL)
				(void)snprintf(service_id,
				    SC_PUBLISH_SERVICE_ID_LEN, "%s-%s",
				    set_service_method_string(out->method),
				    ifname);
			pthread_mutex_unlock(&g_workers_lock);
			return;
		}
	}
	pthread_mutex_unlock(&g_workers_lock);

	if (service_id != NULL)
		service_id[0] = '\0';
	if (ipconfig_prefs_lookup(ifname, out, service_id,
	    SC_PUBLISH_SERVICE_ID_LEN)) {
		if (service_id != NULL && service_id[0] == '\0')
			(void)snprintf(service_id, SC_PUBLISH_SERVICE_ID_LEN,
			    "%s-%s", set_service_method_string(out->method),
			    ifname);
		return;
	}
	(void)memset(out, 0, sizeof(*out));
	out->method = ipconfig_method_none_e;
}

static void
method_lookup(const char *ifname, struct ipconfig_method_info *out)
{
	service_lookup(ifname, out, NULL);
}

/*
 * The IPv6 method `ifname` should run (#423), and the ID its service publishes
 * under: an `ipconfig set` method first (Darwin names such a service
 * <Method>-<if>), then the service in Setup:. With no service, IPv6 is left
 * alone (#570).
 */
void
ipconfigd_v6_lookup(const char *ifname, struct ipconfig_v6_method_info *out,
    char *service_id)
{
	int i;

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_set_methods[i].have_v6 &&
		    strcmp(g_set_methods[i].ifname, ifname) == 0) {
			*out = g_set_methods[i].v6;
			(void)snprintf(service_id, SC_PUBLISH_SERVICE_ID_LEN,
			    "%s-%s", ipconfig_v6_method_string(out->method),
			    ifname);
			pthread_mutex_unlock(&g_workers_lock);
			return;
		}
	}
	pthread_mutex_unlock(&g_workers_lock);

	service_id[0] = '\0';
	if (ipconfig_prefs_lookup_v6(ifname, out, service_id,
	    SC_PUBLISH_SERVICE_ID_LEN)) {
		if (service_id[0] == '\0')
			(void)snprintf(service_id, SC_PUBLISH_SERVICE_ID_LEN,
			    "%s-%s", ipconfig_v6_method_string(out->method),
			    ifname);
		return;
	}
	(void)memset(out, 0, sizeof(*out));
	out->method = ipconfig_method_unmanaged_v6_e;
}

/* `ifname`'s interface type, from its AF_LINK address; -1 if unknown. */
static int
iface_link_type(const char *ifname)
{
	struct ifaddrs *ifa, *p;
	int type = -1;

	if (getifaddrs(&ifa) != 0)
		return (-1);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK ||
		    strcmp(p->ifa_name, ifname) != 0)
			continue;
		type = ((const struct sockaddr_dl *)(void *)p->ifa_addr)->
		    sdl_type;
		break;
	}
	freeifaddrs(ifa);
	return (type);
}

/*
 * config_method_start()'s per-type rules (bootp ipconfigd.c). Darwin's
 * VLAN, bond and bridge interfaces report IFT_ETHER once configured
 * (xnu if_vlan.c, if_bond.c, if_bridge.c); here they keep their own types,
 * so they count as Ethernet class. Anything else cannot do ARP, so manual
 * methods only; loopback does Manual only.
 */
static bool
method_allowed_on_link(const char *ifname, ipconfig_method_t method)
{
	switch (iface_link_type(ifname)) {
	case -1:		/* not there yet: the link path will say */
	case IFT_ETHER:
	case IFT_L2VLAN:
	case IFT_BRIDGE:
	case IFT_IEEE8023ADLAG:
		return (true);
	case IFT_IEEE1394:
		return (method != ipconfig_method_bootp_e);
	case IFT_LOOP:
		return (method == ipconfig_method_manual_e);
	default:
		return (method != ipconfig_method_dhcp_e &&
		    method != ipconfig_method_bootp_e &&
		    method != ipconfig_method_inform_e &&
		    method != ipconfig_method_linklocal_e);
	}
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
	char service_id[SC_PUBLISH_SERVICE_ID_LEN];
	pthread_attr_t attr;
	int i, rc;

	service_lookup(ifname, &method, service_id);
	if (!ipconfig_method_is_served(method.method))
		return (false);
	if (!method_allowed_on_link(ifname, method.method)) {
		xlog("%s: ConfigMethod %s is not possible on this interface",
		    ifname, ipconfig_method_string(method.method));
		return (false);
	}

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
	w->renew = 0;
	w->ready = false;
	w->restart = false;
	w->conflict = false;
	w->ll_conflict = 0;
	w->v4_assigned = 0;
	w->conflict_time = 0;
	w->conflict_count = 0;
	w->method = method;
	(void)strlcpy(w->service_id, service_id, sizeof(w->service_id));
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
	bool served;

	method_lookup(ifname, &method);
	served = ipconfig_method_is_served(method.method);
	/* The link still matters to an IPv6 service with no IPv4 one (#423). */
	if (!served && !v6_service_wants_link(ifname)) {
		xlog("%s: ConfigMethod %s — leaving it alone", ifname,
		    ipconfig_method_string(method.method));
		return;
	}
	if (iface_bring_up(ifname) != 0) {
		xlog("%s: could not bring up: %s", ifname, strerror(errno));
		return;
	}
	/* Loopback has no link to wait for; nothing watches lo0's. */
	if (strncmp(ifname, "lo", 2) != 0 && !link_active_in_store(ifname))
		return;
	if (!served)
		v6_service_v4_result(ifname);	/* IPv4 NONE is a result */
	else if (dhcp_worker_start(ifname, g_lease_cap_secs))
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
	char service_id[SC_PUBLISH_SERVICE_ID_LEN];
	bool unwinding = false;
	int i;

	/* IPv6's own method changing is none of IPv4's business (#423). */
	v6_service_reconfigure(ifname);

	service_lookup(ifname, &method, service_id);
	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		struct dhcp_worker *w = &g_workers[i];

		if (!w->active || strcmp(w->ifname, ifname) != 0)
			continue;
		if (!w->stop && memcmp(&w->method, &method,
		    sizeof(method)) == 0 &&
		    strcmp(w->service_id, service_id) == 0) {
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

/* An Apply changed `ifname`'s service (ipconfig_prefs.c). */
static void
on_prefs_changed(const char *ifname)
{
	service_reconfigure(ifname);
}

/*
 * SCPreferences apply changed `ifname`'s DNS entity: re-merge and rewrite
 * DNS. The worker keeps running, and so does its lease (#425).
 */
static void
on_prefs_dns_changed(const char *ifname)
{
	if (strncmp(ifname, "lo", 2) == 0)
		return;
	dns_config_update(ifname);
}

/*
 * ServiceOrder or a PrimaryRank moved (#570): the same interfaces are bound,
 * but the winner may not be. As after any bind or teardown, re-elect and
 * re-render DNS from whoever won.
 */
static void
on_prefs_order_changed(void)
{
	struct sc_publish *pub;
	char primary[IFNAMSIZ];

	pub = sc_publish_open("ipconfigd");
	if (pub != NULL) {
		(void)sc_publish_update_primary(pub);
		sc_publish_close(pub);
	}
	if (bound_state_primary(primary, sizeof(primary)))
		dns_config_update(primary);
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
	/* Darwin's set_if() reports config_method_start()'s answer. */
	if (info->method != ipconfig_method_none_e &&
	    !method_allowed_on_link(ifname, info->method))
		return (ipconfig_status_invalid_operation_e);

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
		g_set_methods[slot].have_v4 = true;
		g_set_methods[slot].info = *info;
	}
	pthread_mutex_unlock(&g_workers_lock);
	if (slot < 0)
		return (ipconfig_status_resource_unavailable_e);

	xlog("set(%s) %s", ifname, ipconfig_method_string(info->method));
	service_reconfigure(ifname);
	/* A `set` method hides the service's Setup DNS, restarted or not. */
	dns_config_update(ifname);
	return (ipconfig_status_success_e);
}

/*
 * `ipconfig set <if> {AUTOMATIC-V6,MANUAL-V6,NONE-V6}` (#423), set_if()'s
 * IPv6 half: only IPv6 changes. Serviced like ipconfigd_set_method().
 */
ipconfig_status_t
ipconfigd_set_v6_method(const char *ifname,
    const struct ipconfig_v6_method_info *info)
{
	ipconfig_status_t status;
	int i, slot = -1;

	status = set_check_ifname(ifname);
	if (status != ipconfig_status_success_e)
		return (status);
	if (info->method == ipconfig_method_stf_e)
		return (ipconfig_status_operation_not_supported_e);

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
		g_set_methods[slot].have_v6 = true;
		g_set_methods[slot].v6 = *info;
	}
	pthread_mutex_unlock(&g_workers_lock);
	if (slot < 0)
		return (ipconfig_status_resource_unavailable_e);

	xlog("set(%s) %s", ifname, ipconfig_v6_method_string(info->method));
	v6_service_reconfigure(ifname);
	return (ipconfig_status_success_e);
}

bool
ipconfigd_set_method_active(const char *ifname)
{
	bool active = false;
	int i;

	pthread_mutex_lock(&g_workers_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_set_methods[i].have_v4 &&
		    strcmp(g_set_methods[i].ifname, ifname) == 0) {
			active = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_workers_lock);
	return (active);
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
 * notices `stop` — within about a second, whether it is bound or still in
 * its DHCP exchange (#410). Clearing the binding and re-electing here as
 * well means the primary moves off a vanished interface now rather than
 * whenever its worker gets round to it. The published Service keys go too:
 * their <UUID> was cached when the service started (#409), so it still
 * resolves now that the MAC is gone. Both sides are idempotent, so the
 * double clear is harmless.
 */
static void
on_link_departed(const char *ifname)
{
	struct sc_publish *pub;
	bool had_worker, was_bound;

	v6_service_link_down(ifname);
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

	pub = sc_publish_open("ipconfigd");
	if (pub != NULL)
		(void)sc_publish_remove(pub, ifname);
	bound_state_clear(ifname);
	if (pub != NULL) {
		(void)sc_publish_update_primary(pub);
		sc_publish_close(pub);
	}
	dns_config_update(ifname);
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
	struct dhcp_worker *w;
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
		 * The IPv6 service stops with the link (#423), even when there
		 * is no IPv4 worker to unwind. Not on a stale Active:0 (#250):
		 * the store is authoritative.
		 */
		if (!link_active_in_store(ifname))
			v6_service_link_down(ifname);

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
				dns_config_update(ifname);
			}
			return;
		}

		/*
		 * Lost the link mid-exchange: a worker still in DISCOVER /
		 * REQUEST would otherwise retry into a dead link and hold the
		 * slot, swallowing the next link-up (#415). Apple sends an
		 * unbound client back to INIT on link-down; we stop the worker
		 * and mark it to restart, so its exit runs service_start(),
		 * which starts a fresh exchange if the link is back by then.
		 *
		 * Not on a stale Active:0 (#250): a late NIC's leading Active:0
		 * can be delivered after an Active:1 already started the
		 * worker. Debounce against the store value, which is
		 * authoritative, and leave the worker alone if it shows Active.
		 */
		pthread_mutex_lock(&g_workers_lock);
		w = worker_find_locked(ifname);
		pthread_mutex_unlock(&g_workers_lock);
		if (w != NULL) {
			for (i = 0; i < 2; i++) {
				struct timespec ts = { .tv_sec = 0,
				    .tv_nsec = 500 * 1000 * 1000 };	/* 500ms */

				if (link_active_in_store(ifname))
					return;
				(void)nanosleep(&ts, NULL);
			}
			if (link_active_in_store(ifname))
				return;
			pthread_mutex_lock(&g_workers_lock);
			w = worker_find_locked(ifname);
			if (w != NULL) {
				w->stop = 1;
				w->conflict = false;	/* exit, not retry */
				w->restart = true;
			}
			pthread_mutex_unlock(&g_workers_lock);
			if (w != NULL) {
				xlog("link-down(%s) — not bound; unwinding its "
				    "in-progress worker", ifname);
				return;
			}
		}

		/* Configured to be left alone (#58): not even admin-up. */
		method_lookup(ifname, &method);
		if (!ipconfig_method_is_served(method.method) &&
		    !v6_service_wants_link(ifname))
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
		return;
	}

	/*
	 * No worker to wait for: an IPv4 method of NONE or one ipconfigd does
	 * not serve is a result already, so IPv6 needn't wait (#423). A worker
	 * already running reports its own result.
	 */
	method_lookup(ifname, &method);
	if (!ipconfig_method_is_served(method.method))
		v6_service_v4_result(ifname);
}

/*
 * Link-watch renew callback (#572), on a dispatch worker: Darwin's
 * link_refresh(). A bound DHCP service renews, its lease loop noticing the
 * flag within a second; one with no lease goes back to INIT now (dhcp.c's
 * renew case). A Manual service leaves IPv4 alone. An Automatic IPv6 service
 * solicits whatever the IPv4 side is doing, as rtadv.c restarts solicitation
 * on a renew (#423).
 */
static void
on_link_refresh(const char *ifname)
{
	struct dhcp_worker *w;
	bool dhcp = false;

	pthread_mutex_lock(&g_workers_lock);
	w = worker_find_locked(ifname);
	if (w != NULL && !w->stop) {
		dhcp = w->method.method == ipconfig_method_dhcp_e;
		if (dhcp)
			w->renew = 1;
	}
	pthread_mutex_unlock(&g_workers_lock);
	xlog("%s: configuration refresh requested%s", ifname,
	    dhcp ? " — renewing or restarting DHCP" : "");
	v6_service_solicit(ifname);
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
	bool defend = false, linklocal, manual;
	int count;
	char a[INET_ADDRSTRLEN], h[3 * 16];
	int i;

	/* bootp's arp_collision(): no hardware address, or our own, is no claim. */
	if (hw_len <= 0 || is_our_hwaddr(hw, hw_len))
		return;
	if (!bound_state_get_addr(ifname, &bound) || bound != ip.s_addr)
		return;
	linklocal = bound_state_is_linklocal(ifname);

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
	if (linklocal) {
		/*
		 * linklocal.c does not defend: the address is given up and
		 * another picked, at once for a LinkLocal service and on the
		 * worker's next round for a DHCP service's fallback (#426).
		 */
		if (w->method.method == ipconfig_method_linklocal_e) {
			w->conflict = true;
			w->stop = 1;
		} else {
			w->ll_conflict = 1;
		}
		pthread_mutex_unlock(&g_workers_lock);
		xlog("%s: link-local %s in use by %s — giving it up", ifname, a,
		    h);
		return;
	}
	if (w->method.method == ipconfig_method_inform_e) {
		/* inform_thread() only reports it; the address stays. */
		pthread_mutex_unlock(&g_workers_lock);
		xlog("%s: %s in use by %s", ifname, a, h);
		return;
	}
	if (w->method.method == ipconfig_method_bootp_e ||
	    w->method.method == ipconfig_method_failover_e) {
		/* Neither defends: bootp_failed(), failover_start(). */
		w->conflict = true;
		w->stop = 1;
		pthread_mutex_unlock(&g_workers_lock);
		xlog("%s: %s in use by %s — giving it up", ifname, a, h);
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
		v6_service_on_duplicated(ifname,
		    (const struct in6_addr *)addr);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	uint32_t lease_cap_secs;

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
	/* Before any election can update it (#429). */
	dns_configuration_init();
	nwi_configuration_init();
	if (mach_service_start() != 0)
		xlog("mach_service_start failed; RPC off");

	/*
	 * Apple-shaped DHCP trigger. Rather than DHCP unconditionally at
	 * startup (which races a real NIC whose link negotiates a beat
	 * after we scan — the failure mode that left a stock-kernel box
	 * without a lease once hwregd's attach event was removed), we:
	 *
	 *   1. bring each interface that has a service administratively up
	 *      so its link starts negotiating, then
	 *   2. watch State:/Network/Interface/<if>/Link and start the
	 *      service when KernelEventMonitor reports the link Active.
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
	 * interface. Reading Setup: starts the service of each interface that
	 * has one, which brings it up: no service, no IFF_UP (#570).
	 */
	(void)ipconfig_prefs_start(on_prefs_changed, on_prefs_dns_changed,
	    startup_check, on_prefs_order_changed);

	if (sc_link_watch_start(on_link_event, on_addr_conflict,
	    on_link_refresh, lease_cap_secs) != 0)
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
