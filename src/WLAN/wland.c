/*
 * wland — NextBSD 802.11 management daemon.
 *
 *   /usr/sbin/wland
 *   launchd job    org.nextbsd.wland
 *   Mach service   org.nextbsd.wlan
 *
 * C + CoreFoundation. NOT Objective-C: there is no libobjc2 and no Foundation
 * in nextbsd-userland (zero .m files are built; the toolchains enable C/C++
 * only). The CoreWLAN-shaped ObjC API belongs ABOVE the Mach line, in Gershwin,
 * reached through the `wlan` CLI — which is also where it belongs
 * architecturally.
 *
 * ---------------------------------------------------------------------------
 * WHAT WLAND IS FOR
 *
 * Three things stand between an Intel card and an IP address on NextBSD. The
 * kernel bug (missing wlan_xauth) is fixed separately. The other two are wland's:
 *
 *   1. NOTHING CREATES THE VAP. On FreeBSD, `wlans_iwlwifi0="wlan0"` in rc.conf
 *      clones the 802.11 VAP off the PHY. NextBSD has no rc.conf and no
 *      /etc/rc.d, and never will — launchd is PID 1 and /sbin/init is superseded.
 *      So `ifconfig wlan0 create wlandev iwlwifi0` has no owner. It is wland's.
 *      (Apple's wifid never had this job: IO80211 presents en0 directly.)
 *
 *   2. NOTHING PUBLISHES AN ASSOCIATION-COMPLETE SIGNAL. net80211 reports the
 *      link UP at ASSOCIATION — before the WPA 4-way handshake — so ipconfigd
 *      would fire DHCP onto an unauthenticated port, get every packet dropped,
 *      burn its ~28s retransmit ladder, and then never retry (Link is already
 *      Active, so no new event ever comes). ipconfigd's gate now also requires
 *      AirPort{Authenticated}, and wland is the only thing that can publish it.
 *
 * That second point is why the "cheap path" of skipping wland was rejected: it
 * does not actually work. Without something publishing association-complete,
 * there is nothing for the DHCP gate to gate on, and the minimum honest shim IS
 * wland v0.
 *
 * ---------------------------------------------------------------------------
 * ARCHITECTURE
 *
 *   wland  --(wpa_ctrl UNIX socket)-->  stock wpa_supplicant 2.11 from base
 *          --(SCDynamicStore/Mach)--->  configd
 *          --(SCPreferences)---------->  known networks (plaintext PSK; no keychain)
 *          --(MIG org.nextbsd.wlan)-->  the `wlan` CLI  -->  Gershwin
 *
 * wpa_supplicant is STOCK and stays stock: not vendored, not patched, not taught
 * Mach, and not the port. It speaks net80211 ioctls downward and a UNIX control
 * socket upward, and it does not know configd exists. All Mach knowledge lives
 * here. See wpa_ctrl.h for why that matters.
 *
 * wland spawns wpa_supplicant itself rather than letting launchd do it, because
 * wpa_supplicant needs `-i wlan0` and wlan0 does not exist until wland creates
 * it. It is spawned with an EMPTY config (update_config=0, no network blocks) so
 * that wland is the sole author of every network block — two autojoin brains
 * fighting is the classic NetworkManager failure mode.
 */
#include "wland.h"
#include "airport.h"
#include "iokit_watch.h"
#include "mach_service.h"
#include "supplicant.h"
#include "vap.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t	wland_got_term;

static pthread_mutex_t	g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wlan_if	g_ifs[WLAN_MAX_PHY];
static struct airport	*g_air;

/*
 * Self-pipe that wakes the event loop: the IORegistry callbacks
 * (iokit_watch.c) and the signal handler each write a byte to [1]; the loop
 * polls [0]. Lets the loop block with no timeout while there is no radio.
 */
static int		g_wake[2] = { -1, -1 };

#define	WPA_SUPPLICANT		"/usr/sbin/wpa_supplicant"
#define	WPA_CTRL_DIR		"/var/run/wpa_supplicant"
#define	WLAND_RUN_DIR		"/var/run/wland"

/* How often to retry an autojoin scan when nothing is connected. */
#define	AUTOJOIN_INTERVAL_SEC	15

/*
 * How often to re-scan net.wlan.devices when the IORegistry cannot tell us
 * about radios (no /dev/ioregistry), and how often to retry a radio whose
 * bring-up failed. The 802.11 driver commonly finishes attaching several
 * seconds into boot — AFTER launchd has already started wland — so a one-shot
 * enumeration at startup races it. See adopt_phys().
 */
#define	PHY_RESCAN_INTERVAL_SEC	2

/* Size of a net.wlan.devices read: more than WLAN_MAX_PHY, so a radio we
 * manage is never cut off the end of the list and mistaken for gone. */
#define	PHY_LIST_MAX		(WLAN_MAX_PHY * 4)

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
	(void)fprintf(stderr, "wland %s ", tbuf);

	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void
on_signal(int sig)
{
	int saved = errno;

	wland_got_term = sig;
	/* Wake a poll() that is blocked with no timeout. */
	if (g_wake[1] >= 0)
		(void)write(g_wake[1], "", 1);
	errno = saved;
}

void
wland_lock(void)
{
	(void)pthread_mutex_lock(&g_lock);
}

void
wland_unlock(void)
{
	(void)pthread_mutex_unlock(&g_lock);
}

int
wland_if_count_locked(void)
{
	int i, n = 0;

	for (i = 0; i < WLAN_MAX_PHY; i++) {
		if (g_ifs[i].active)
			n++;
	}
	return (n);
}

struct wlan_if *
wland_if_at_locked(int idx)
{
	int i, n = 0;

	if (idx < 0)
		return (NULL);
	for (i = 0; i < WLAN_MAX_PHY; i++) {
		if (!g_ifs[i].active)
			continue;
		if (n == idx)
			return (&g_ifs[i]);
		n++;
	}
	return (NULL);
}

struct wlan_if *
wland_if_by_name_locked(const char *vap)
{
	int i;

	if (vap == NULL)
		return (NULL);
	for (i = 0; i < WLAN_MAX_PHY; i++) {
		if (g_ifs[i].active && strcmp(g_ifs[i].vap, vap) == 0)
			return (&g_ifs[i]);
	}
	return (NULL);
}

/* Slot currently managing PHY `phy` ("iwlwifi0"), or NULL. Caller holds lock. */
static struct wlan_if *
if_by_phy_locked(const char *phy)
{
	int i;

	for (i = 0; i < WLAN_MAX_PHY; i++) {
		if (g_ifs[i].active && strcmp(g_ifs[i].phy, phy) == 0)
			return (&g_ifs[i]);
	}
	return (NULL);
}

/* First unused table slot, or NULL if the table is full. Caller holds lock. */
static struct wlan_if *
if_free_slot_locked(void)
{
	int i;

	for (i = 0; i < WLAN_MAX_PHY; i++) {
		if (!g_ifs[i].active)
			return (&g_ifs[i]);
	}
	return (NULL);
}

/*
 * Write the empty wpa_supplicant config for `vap`.
 *
 * EMPTY IS THE POINT. It carries a ctrl_interface and update_config=0, and NOT
 * ONE network block. Every network is injected at runtime by wland over the
 * control socket. If this file also carried SSIDs, wpa_supplicant's own autojoin
 * would be picking networks at the same time wland's does — two brains, fighting,
 * which is exactly the bug that makes NetworkManager infuriating. update_config=0
 * additionally forbids wpa_supplicant from ever writing the file back, so it
 * cannot acquire state behind our back.
 */
static int
write_wpa_conf(const char *vap, char *path_out, size_t path_out_sz)
{
	FILE *fp;
	int fd;

	(void)snprintf(path_out, path_out_sz, "%s/wpa-%s.conf",
	    WLAND_RUN_DIR, vap);

	fd = open(path_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		xlog("open(%s): %s", path_out, strerror(errno));
		return (-1);
	}
	fp = fdopen(fd, "w");
	if (fp == NULL) {
		(void)close(fd);
		return (-1);
	}
	(void)fprintf(fp,
	    "# Generated by wland. Deliberately EMPTY of network blocks:\n"
	    "# wland is the sole author of wpa_supplicant's networks, injected\n"
	    "# at runtime over the control socket. Do not add SSIDs here — two\n"
	    "# autojoin brains will fight.\n"
	    "ctrl_interface=%s\n"
	    "update_config=0\n", WPA_CTRL_DIR);
	(void)fclose(fp);
	return (0);
}

/*
 * Spawn wpa_supplicant for `vap`. Not -B: we keep it as a child so we can reap
 * it and notice if it dies. Returns the pid, or -1.
 */
static pid_t
spawn_supplicant(const char *vap)
{
	char conf[256];
	pid_t pid;

	if (write_wpa_conf(vap, conf, sizeof(conf)) != 0)
		return (-1);

	pid = fork();
	if (pid < 0) {
		xlog("fork: %s", strerror(errno));
		return (-1);
	}
	if (pid == 0) {
		/* Child. */
		(void)execl(WPA_SUPPLICANT, "wpa_supplicant",
		    "-i", vap, "-c", conf, "-C", WPA_CTRL_DIR,
		    "-D", "bsd", (char *)NULL);
		/* execl only returns on failure. */
		(void)fprintf(stderr,
		    "wland: exec %s failed: %s\n", WPA_SUPPLICANT,
		    strerror(errno));
		_exit(127);
	}

	xlog("%s: spawned wpa_supplicant (pid %d, empty config %s)",
	    vap, (int)pid, conf);
	return (pid);
}

/*
 * Ensure the runtime directories exist. Called on every bring-up attempt, not
 * only at startup, because launchd can start wland before /var/run is populated
 * on a fresh boot; a one-shot mkdir then races and fails silently. mkdir(2)
 * returning EEXIST is exactly the state we want, so the results are ignored.
 */
static void
ensure_run_dirs(void)
{
	(void)mkdir("/var/run", 0755);		/* parent may also be late at boot */
	(void)mkdir(WLAND_RUN_DIR, 0700);
	(void)mkdir(WPA_CTRL_DIR, 0700);
}

/*
 * Bring one radio all the way up: clone the VAP, admin-up, spawn
 * wpa_supplicant, attach to its control socket.
 */
static bool
bring_up_phy(struct wlan_if *w, const char *phy)
{
	char vap[IF_NAMESIZE];
	int tries;

	/*
	 * Make sure the runtime dirs exist NOW, on every attempt — not just once
	 * at startup. On a fresh boot launchd can start wland before /var/run is
	 * ready, so the startup mkdir races and fails silently, and every
	 * write_wpa_conf() below would then fail forever with ENOENT while the VAP
	 * sits created-but-supplicant-less (ifconfig shows wlan0, but wland never
	 * marks it active, so the `wlan` CLI reports no interfaces). Re-creating
	 * here lets the next rescan retry self-heal once /var/run appears.
	 */
	ensure_run_dirs();

	if (vap_create(phy, vap, sizeof(vap)) != 0)
		return (false);

	/*
	 * The VAP must be IFF_UP before it can associate. Note this also makes
	 * configd publish a Link key for it (RTM_IFANNOUNCE on create, then
	 * RTM_IFINFO) — which is how ipconfigd learns the interface exists at
	 * all, and why wland does not need its own PF_ROUTE listener.
	 */
	(void)vap_bring_up(vap);

	w->supplicant_pid = spawn_supplicant(vap);
	if (w->supplicant_pid < 0)
		return (false);

	/*
	 * wpa_supplicant needs a moment to create its control socket. Retry
	 * rather than sleeping a fixed amount and hoping.
	 */
	for (tries = 0; tries < 50 && !wland_got_term; tries++) {
		w->supp = supplicant_attach(vap);
		if (w->supp != NULL)
			break;
		(void)usleep(100 * 1000);	/* 100ms */
	}
	if (w->supp == NULL) {
		xlog("%s: wpa_supplicant never opened its control socket",
		    vap);
		/*
		 * Reap the child we spawned before giving up. bring_up_phy is
		 * now retried from the event loop's PHY rescan, so a failure
		 * that left a live wpa_supplicant behind would leak one per
		 * retry. The VAP itself is left intact — the retry adopts it via
		 * vap_find_existing() rather than cloning a second one.
		 */
		if (w->supplicant_pid > 0) {
			(void)kill(w->supplicant_pid, SIGTERM);
			(void)waitpid(w->supplicant_pid, NULL, 0);
			w->supplicant_pid = -1;
		}
		return (false);
	}

	(void)strlcpy(w->phy, phy, sizeof(w->phy));
	(void)strlcpy(w->vap, vap, sizeof(w->vap));
	w->active = true;

	xlog("WLAN-IF-OK: %s on %s ready", vap, phy);
	return (true);
}

/* Publish the current association state for one interface. */
static void
publish_state(struct wlan_if *w)
{
	struct assoc_state st;

	if (g_air == NULL) {
		/* configd wasn't up when we started; try again now. */
		g_air = airport_open();
		if (g_air == NULL)
			return;
	}
	supplicant_get_state(w->supp, &st);
	(void)airport_publish(g_air, w->vap, &st);
}

/*
 * Autojoin: if this interface is not connected, scan and associate with the
 * strongest remembered network in range.
 */
static void
try_autojoin(struct wlan_if *w)
{
	struct assoc_state st;
	struct known_net kn;
	struct scan_entry scan[64];
	int n, i;

	supplicant_get_state(w->supp, &st);
	if (st.state == wlan_state_connected_e ||
	    st.state == wlan_state_4way_e ||
	    st.state == wlan_state_associating_e)
		return;				/* busy or already up */

	(void)supplicant_scan(w->supp);
	(void)supplicant_refresh_scan(w->supp);

	n = supplicant_scan_count(w->supp);
	if (n <= 0)
		return;
	if (n > (int)(sizeof(scan) / sizeof(scan[0])))
		n = (int)(sizeof(scan) / sizeof(scan[0]));
	for (i = 0; i < n; i++)
		(void)supplicant_scan_get(w->supp, i, &scan[i]);

	if (!known_net_pick(scan, n, &kn))
		return;				/* nothing known in range */

	(void)supplicant_connect(w->supp, kn.ssid, kn.psk);
}

/*
 * Bring up every radio in net.wlan.devices we are not already managing. Called
 * once at startup, whenever an IO80211Controller entry appears in the
 * IORegistry (iokit_watch.c), and, without /dev/ioregistry, on a timer.
 *
 * Why it has to run after startup at all: the 802.11 driver often finishes
 * attaching AFTER launchd has started wland (the iwlwifi LinuxKPI port spends
 * several seconds loading firmware), and a PHY is not an ifnet, so there is no
 * RTM_IFANNOUNCE for it. net80211 publishes IO80211Controller once the radio is
 * listed in net.wlan.devices, which is what wakes us. A USB radio hot-plugged
 * later arrives the same way. Idempotent: a radio already in the table is
 * skipped, and vap_create() adopts a VAP that already exists.
 *
 * Returns the number of listed radios that could not be brought up, which the
 * caller retries on a timer.
 */
static int
adopt_phys(void)
{
	struct wlan_phy phys[PHY_LIST_MAX];
	int nphys, i, failed = 0;

	nphys = vap_enumerate_phys(phys, PHY_LIST_MAX);
	if (nphys <= 0)
		return (0);			/* none yet, or not loaded */

	for (i = 0; i < nphys; i++) {
		struct wlan_if *w;

		wland_lock();
		if (if_by_phy_locked(phys[i].phy) != NULL) {
			wland_unlock();			/* already ours */
			continue;
		}
		w = if_free_slot_locked();
		if (w == NULL) {
			wland_unlock();
			xlog("radio table full (%d) — ignoring %s",
			    WLAN_MAX_PHY, phys[i].phy);
			continue;
		}
		xlog("found 802.11 PHY: %s", phys[i].phy);
		if (!bring_up_phy(w, phys[i].phy)) {
			xlog("%s: could not bring up (will retry)", phys[i].phy);
			failed++;
		}
		wland_unlock();
	}
	return (failed);
}

/*
 * Release every radio that has gone away: its driver kext was unloaded, or a
 * USB radio was unplugged. Called when an IO80211Controller entry is retired,
 * and with the timed rescan without /dev/ioregistry.
 *
 * A slot is gone when net.wlan.devices no longer lists its PHY, or when its
 * VAP no longer exists. net80211 destroys a radio's VAPs before it takes the
 * radio off the list and retires the entry, so the VAP check also catches a
 * radio that was unloaded and loaded again before we got here (same name,
 * new radio). The VAP is already gone, so it is not destroyed here: clear the
 * AirPort key (otherwise ipconfigd would keep a stale Authenticated:true),
 * stop and reap the wpa_supplicant, and free the slot.
 */
static void
release_departed_phys(void)
{
	struct wlan_phy phys[PHY_LIST_MAX];
	pid_t reap[WLAN_MAX_PHY];
	int nphys, nreap = 0, i, j;

	nphys = vap_enumerate_phys(phys, PHY_LIST_MAX);
	if (nphys == VAP_NO_NET80211)
		nphys = 0;			/* Wlan.kext unloaded: all gone */
	else if (nphys < 0)
		return;				/* cannot tell; keep everything */

	wland_lock();
	for (i = 0; i < WLAN_MAX_PHY; i++) {
		struct wlan_if *w = &g_ifs[i];
		bool listed = false;

		if (!w->active)
			continue;
		for (j = 0; j < nphys && !listed; j++)
			listed = (strcmp(phys[j].phy, w->phy) == 0);
		if (listed && if_nametoindex(w->vap) != 0)
			continue;

		xlog("%s: 802.11 PHY %s is gone — releasing", w->vap, w->phy);
		if (g_air != NULL)
			(void)airport_remove(g_air, w->vap);
		if (w->supplicant_pid > 0) {
			(void)kill(w->supplicant_pid, SIGTERM);
			reap[nreap++] = w->supplicant_pid;
		}
		supplicant_detach(w->supp);
		w->supp = NULL;
		w->supplicant_pid = -1;
		w->phy[0] = '\0';
		w->vap[0] = '\0';
		w->active = false;
	}
	wland_unlock();

	/* Outside the lock, so a slow exit does not stall the Mach service. */
	for (i = 0; i < nreap; i++)
		(void)waitpid(reap[i], NULL, 0);
}

/* Empty the self-pipe after a wake-up. */
static void
drain_wake_pipe(void)
{
	char buf[64];

	while (read(g_wake[0], buf, sizeof(buf)) > 0)
		;
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	time_t last_autojoin = 0;
	time_t last_rescan = 0;
	bool iokit;
	int failed, have, i;

	(void)argc;
	(void)argv;

	xlog("wland starting (VAP lifecycle + wpa_supplicant + "
	    "State:/Network/Interface/<if>/AirPort)");

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);
	/* Do not die when a wpa_supplicant we spawned exits. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* Before anything can write to it: the signal handler checks for -1. */
	if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0) {
		xlog("pipe2: %s — radios found by rescan only", strerror(errno));
		g_wake[0] = g_wake[1] = -1;
	}

	/* Best-effort at startup; bring_up_phy() re-creates these on every retry
	 * so a /var/run that is not ready this early does not wedge us forever. */
	ensure_run_dirs();

	/*
	 * The Mach service comes up FIRST, before any radio work. A client that
	 * looks us up while we are still cloning VAPs should get an answer
	 * ("zero interfaces yet"), not a bootstrap failure.
	 */
	if (mach_service_start() != 0)
		xlog("mach_service_start failed — RPC off");

	/*
	 * configd may not be up yet. There is NO launchd service ordering in
	 * this port — no StartAfter, no Requires, and the plist directory scan
	 * is not even sorted — so the rule is: open the store lazily and
	 * tolerate NULL. Never assume configd exists in main().
	 */
	g_air = airport_open();
	if (g_air == NULL)
		xlog("configd not reachable yet — will retry on first publish");

	/*
	 * Follow radios through the IORegistry: net80211 publishes an
	 * IO80211Controller entry per radio and retires it when the radio goes.
	 * Without /dev/ioregistry (or the self-pipe), fall back to rescanning
	 * net.wlan.devices every PHY_RESCAN_INTERVAL_SEC.
	 */
	iokit = (g_wake[0] >= 0 && iokit_watch_start(g_wake[1]) == 0);

	/*
	 * Bring up whatever radios exist right now. On a cold boot there usually
	 * are none yet: net80211 (Wlan.kext) is loaded with the first Wi-Fi
	 * driver kext, and the driver may still be loading firmware. Say so once.
	 */
	failed = adopt_phys();
	wland_lock();
	have = wland_if_count_locked();
	wland_unlock();
	if (have == 0 && failed == 0) {
		if (iokit)
			xlog("no 802.11 radio yet — waiting for "
			    "IO80211Controller");
		else
			xlog("no 802.11 radio yet — no /dev/ioregistry, "
			    "rescanning net.wlan.devices every %ds",
			    PHY_RESCAN_INTERVAL_SEC);
	}
	last_rescan = time(NULL);

	/*
	 * Event loop. Poll every interface's wpa_supplicant event socket; on any
	 * association-state change, republish AirPort — which is what releases
	 * ipconfigd's DHCP gate. Also poll the self-pipe, which the IORegistry
	 * callbacks and the signal handler write to.
	 *
	 * While an interface exists, a 1s tick drives autojoin retries and reaps
	 * a wpa_supplicant that died. With none, the loop blocks with no timeout
	 * until a radio appears or a signal arrives, unless it has to rescan
	 * (no /dev/ioregistry) or retry a radio whose bring-up failed.
	 */
	while (!wland_got_term) {
		struct pollfd pfd[WLAN_MAX_PHY + 1];
		struct wlan_if *w[WLAN_MAX_PHY];
		int nfd = 0, npfd, rc, timeout;
		bool matched = false, terminated = false;
		time_t now;

		wland_lock();
		for (i = 0; i < WLAN_MAX_PHY; i++) {
			int fd;

			if (!g_ifs[i].active || g_ifs[i].supp == NULL)
				continue;
			fd = supplicant_event_fd(g_ifs[i].supp);
			if (fd < 0)
				continue;
			pfd[nfd].fd = fd;
			pfd[nfd].events = POLLIN;
			pfd[nfd].revents = 0;
			w[nfd] = &g_ifs[i];
			nfd++;
		}
		have = wland_if_count_locked();
		wland_unlock();

		npfd = nfd;
		if (g_wake[0] >= 0) {
			pfd[npfd].fd = g_wake[0];
			pfd[npfd].events = POLLIN;
			pfd[npfd].revents = 0;
			npfd++;
		}

		if (have > 0)
			timeout = 1000;
		else if (!iokit || failed > 0)
			timeout = PHY_RESCAN_INTERVAL_SEC * 1000;
		else
			timeout = -1;		/* idle: wait for a radio */

		rc = poll(npfd > 0 ? pfd : NULL, (nfds_t)npfd, timeout);
		if (rc < 0 && errno != EINTR) {
			xlog("poll: %s", strerror(errno));
			break;
		}

		if (rc > 0) {
			wland_lock();
			for (i = 0; i < nfd; i++) {
				if ((pfd[i].revents & POLLIN) == 0)
					continue;
				if (supplicant_pump_events(w[i]->supp))
					publish_state(w[i]);
			}
			wland_unlock();
			if (npfd > nfd && (pfd[nfd].revents & POLLIN) != 0)
				drain_wake_pipe();
		}
		if (wland_got_term)
			break;

		/* Reap any wpa_supplicant that exited. */
		for (;;) {
			pid_t dead = waitpid(-1, NULL, WNOHANG);

			if (dead <= 0)
				break;
			wland_lock();
			for (i = 0; i < WLAN_MAX_PHY; i++) {
				if (g_ifs[i].active &&
				    g_ifs[i].supplicant_pid == dead) {
					xlog("%s: wpa_supplicant (pid %d) "
					    "exited — launchd will restart "
					    "wland", g_ifs[i].vap, (int)dead);
					g_ifs[i].supplicant_pid = -1;
				}
			}
			wland_unlock();
		}

		/*
		 * Radios that went or came. adopt_phys() and
		 * release_departed_phys() do their own locking, so they are
		 * called with no lock held. A departure is handled first, so a
		 * radio that was unloaded and loaded again is released and then
		 * adopted afresh.
		 */
		now = time(NULL);
		if (iokit)
			iokit_watch_take(&matched, &terminated);
		if (!iokit && now - last_rescan >= PHY_RESCAN_INTERVAL_SEC) {
			matched = terminated = true;	/* timed rescan */
			last_rescan = now;
		} else if (iokit && failed > 0 &&
		    now - last_rescan >= PHY_RESCAN_INTERVAL_SEC) {
			matched = true;			/* retry a bring-up */
			last_rescan = now;
		}
		if (terminated)
			release_departed_phys();
		if (matched)
			failed = adopt_phys();

		if (now - last_autojoin >= AUTOJOIN_INTERVAL_SEC) {
			last_autojoin = now;
			wland_lock();
			for (i = 0; i < WLAN_MAX_PHY; i++) {
				if (g_ifs[i].active && g_ifs[i].supp != NULL)
					try_autojoin(&g_ifs[i]);
			}
			wland_unlock();
		}
	}

	xlog("wland exiting on signal %d", (int)wland_got_term);

	/*
	 * Tear down: stop the supplicants and remove the AirPort keys. We do NOT
	 * destroy the VAPs — a wland restart adopts them (vap_find_existing),
	 * and destroying them would bounce the interface for no reason. But the
	 * AirPort key MUST go: leaving Authenticated:true behind for an
	 * interface nothing is managing any more would let ipconfigd keep
	 * DHCPing on a link with no supplicant behind it.
	 */
	wland_lock();
	for (i = 0; i < WLAN_MAX_PHY; i++) {
		if (!g_ifs[i].active)
			continue;
		if (g_air != NULL)
			(void)airport_remove(g_air, g_ifs[i].vap);
		if (g_ifs[i].supplicant_pid > 0)
			(void)kill(g_ifs[i].supplicant_pid, SIGTERM);
		supplicant_detach(g_ifs[i].supp);
		g_ifs[i].supp = NULL;
		g_ifs[i].active = false;
	}
	wland_unlock();

	airport_close(g_air);
	mach_service_join();
	return (0);
}
