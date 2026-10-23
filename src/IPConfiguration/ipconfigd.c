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
#include "bound_state.h"
#include "dhcp_discover.h"
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
};

static pthread_mutex_t	g_workers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dhcp_worker g_workers[BOUND_MAX_IF];

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
 * dhcp_run_on_interface — full DHCPv4 INIT → BOUND → publish → RA →
 * lease loop on `ifname`, then teardown. Runs on that interface's own
 * worker thread.
 *
 * Returns when the lease is lost, the link goes down (*stop), or the
 * daemon is shutting down (got_term). Every one of those paths falls
 * through to the single teardown block at the bottom — there are no
 * early returns once the interface is bound, which is what the old code
 * did on a publish failure (it left the lease applied and the interface
 * in bound_state, but never entered the lease loop, so the lease silently
 * expired and was never renewed).
 */
static void
dhcp_run_on_interface(const char *ifname, uint32_t lease_cap_secs,
    volatile sig_atomic_t *stop)
{
	struct dhcp_lease lease;
	struct sc_publish *pub = NULL;
	char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN];
	char r[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];
	struct ra_info ra;
	struct ra_router *rtr = NULL;
	int rar;

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

	(void)inet_ntop(AF_INET, &lease.addr, a, sizeof(a));
	(void)inet_ntop(AF_INET, &lease.netmask, m, sizeof(m));
	(void)inet_ntop(AF_INET, &lease.router, r, sizeof(r));
	(void)inet_ntop(AF_INET, &lease.server, s, sizeof(s));
	xlog("bound: iface=%s addr=%s netmask=%s router=%s "
	    "server=%s lease=%us",
	    ifname, a, m, r, s, (unsigned)lease.lease_time);
	/*
	 * Make the lease visible to the Mach service worker before the
	 * BOUND marker fires — so any client racing IPCFG-RPC-OK against
	 * IPCFG-BOUND-OK sees the address it expects.
	 */
	bound_state_set(ifname, &lease);
	xlog("IPCFG-BOUND-OK");

	/*
	 * Publish to configd. Failure is non-fatal: the daemon stays up and
	 * the lease is already applied at the kernel level; observers just
	 * won't see the binding. We still enter the lease loop, because a
	 * lease nobody published is a lease that still has to be renewed.
	 */
	pub = sc_publish_open("ipconfigd");
	if (pub == NULL) {
		xlog("IPCFG-STORE-FAIL: no configd session");
	} else if (sc_publish_ipv4(pub, ifname, &lease) != 0) {
		xlog("IPCFG-STORE-FAIL: set State:/.../IPv4");
	} else {
		xlog("IPCFG-STORE-OK");

		/*
		 * Issue #88: publish State:/Network/Service/<UUID>/DHCP
		 * carrying InterfaceName + LeaseStartTime, and Option_12 (host
		 * name) when the lease supplied it. SLIRP doesn't ship
		 * Option_12, so the marker proves the key/dict shape is
		 * correct; hostnamed iter 3 is the first consumer that reads
		 * it. Failure is non-fatal — the IPv4 publish already
		 * succeeded.
		 */
		if (sc_publish_dhcp(pub, ifname, &lease) != 0) {
			xlog("IPCFG-DHCP-FAIL: set State:/.../DHCP");
		} else {
			xlog("IPCFG-DHCP-OK: published /DHCP "
			    "(Option_12 %s)",
			    lease.host_name_len > 0
			        ? lease.host_name : "absent");
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
		} else {
			xlog("IPCFG-RA-MISS: apply_ra_lease failed");
		}
	} else if (rar == 1) {
		xlog("IPCFG-RA-MISS: no RA in 15s "
		    "(SLIRP may not advertise IPv6)");
	} else {
		xlog("IPCFG-RA-MISS: ra_acquire fatal");
	}

	/* Parks here for the life of the lease. */
	(void)lease_loop_run(ifname, &lease, pub, lease_cap_secs, stop);

	/*
	 * Stop the router tracker before anything below removes the IPv6
	 * key, so it cannot republish behind the teardown. Same SIGTERM
	 * rule as the IPv4 lease: keep ::/0 across a restart, otherwise
	 * take it back.
	 */
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
		xlog("%s: lease lost or link down — deconfiguring", ifname);
		(void)deconfigure_lease(ifname, &lease);
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
 * Worker thread body — one per interface that has linked up. Runs the
 * whole DHCP + lease + teardown lifecycle, then releases its slot so a
 * later link event on the same interface can start a fresh one. That
 * release is the other half of the re-arm: the slot, not a global flag,
 * is now what says "a run is in flight on this interface".
 */
static void *
dhcp_worker_main(void *arg)
{
	struct dhcp_worker *w = arg;
	char ifname[IFNAMSIZ];

	(void)strlcpy(ifname, w->ifname, sizeof(ifname));
	dhcp_run_on_interface(ifname, w->lease_cap_secs, &w->stop);

	pthread_mutex_lock(&g_workers_lock);
	w->active = false;
	w->stop = 0;
	w->ifname[0] = '\0';
	pthread_mutex_unlock(&g_workers_lock);

	xlog("%s: DHCP worker exited", ifname);
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
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_workers_lock);
	return (found);
}

/*
 * Start a DHCP worker for `ifname` unless one is already running for it.
 * Returns true if a worker was spawned.
 */
static bool
dhcp_worker_start(const char *ifname, uint32_t lease_cap_secs)
{
	struct dhcp_worker *w = NULL;
	pthread_attr_t attr;
	int i, rc;

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
 * link-watch callback — invoked on a libdispatch worker thread for every
 * change to an interface's Link entity (State:/Network/Interface/<if>/Link),
 * published by KernelEventMonitor from PF_ROUTE. `active` reflects the current
 * link state. This is the Apple-shaped trigger that replaced the hwregd attach
 * subscription.
 *
 * Three cases:
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
 */
static void
on_link_event(const char *ifname, int active, uint32_t lease_cap_secs)
{
	int i;

	/* loopback never carries a DHCP service; the watcher filters lo0 too. */
	if (strncmp(ifname, "lo", 2) == 0)
		return;

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
	if (dhcp_worker_start(ifname, lease_cap_secs))
		xlog("link-active(%s) — started DHCP worker", ifname);
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
	if (dhcp_pick_interface(ifname, sizeof(ifname)) == 0) {
		if (iface_bring_up(ifname) == 0)
			xlog("brought %s up; awaiting link-active to DHCP",
			    ifname);
		else
			xlog("could not bring %s up: %s — relying on link "
			    "watch", ifname, strerror(errno));
	} else {
		xlog("no Ethernet at startup; will DHCP when one links up");
	}

	if (sc_link_watch_start(on_link_event, lease_cap_secs) != 0)
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
