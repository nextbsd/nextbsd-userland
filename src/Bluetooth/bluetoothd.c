/*
 * bluetoothd — NextBSD Bluetooth daemon.
 *
 *   /usr/sbin/bluetoothd
 *   launchd job    org.nextbsd.bluetoothd
 *   Mach service   org.nextbsd.bluetooth
 *
 * C + CoreFoundation, no Objective-C, as wland: there is no libobjc2 or
 * Foundation in nextbsd-userland. The IOBluetooth-shaped API belongs above the
 * Mach line, in Gershwin, reached through the bluetooth(8) CLI.
 *
 * ---------------------------------------------------------------------------
 * WHAT BLUETOOTHD IS FOR
 *
 * bluetoothd and bluetooth(8) are the only Bluetooth userland on NextBSD, as
 * on macOS. The kernel stack (NgUbt, NgHCI, NgL2cap, NgBtsocket, as kexts) is
 * the transport; everything above it lives here: controller bring-up and
 * power, discovery, pairing for Classic and LE with one store, the SDP server
 * and client, the HID host feeding IOHIDUserDevice, and the audio roles. It
 * runs as root from boot, so a paired keyboard works at the login window.
 *
 * This file is the skeleton those pieces plug into:
 *
 *   - the Mach service comes up first, so `bluetooth status` answers at once;
 *   - controllers are followed through the IORegistry (iokit_watch.c), the
 *     way wland follows radios: no polling, no devd;
 *   - the pairing store and policy live in SCPreferences (btstore.c,
 *     keystore.c), and the daemon's state in the SCDynamicStore (btstate.c);
 *   - each controller gets the engines of engine.h, Classic and LE.
 *
 * With no controller it blocks with no timer and loads nothing: the upper
 * kernel stack is loaded when the first controller arrives (#207), so a
 * machine without Bluetooth pays nothing.
 *
 * ---------------------------------------------------------------------------
 * ARCHITECTURE
 *
 *   NgUbt.kext ──IOBluetoothHostController──▶ bluetoothd (iokit_watch.c)
 *   bluetoothd ──raw HCI / L2CAP sockets────▶ the kernel stack (engines)
 *              ──SCPreferences─────────────▶ pairing store + policy
 *              ──SCDynamicStore────────────▶ State:/Bluetooth
 *              ──MIG org.nextbsd.bluetooth─▶ bluetooth(8) ──▶ Gershwin
 */
#include "bluetoothd.h"
#include "btstate.h"
#include "btstore.h"
#include "engine.h"
#include "iokit_watch.h"
#include "mach_service.h"
#include "sdp_server.h"

#include <sys/types.h>

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

volatile sig_atomic_t	bluetoothd_got_term;

static pthread_mutex_t	g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct bt_adapter g_adapters[BT_MAX_ADAPTERS];
static struct bt_prefs	g_prefs;
static bool		g_registry;	/* the IOKit watch is armed */

/*
 * Self-pipe that wakes the event loop: the IORegistry callbacks
 * (iokit_watch.c) and the signal handler each write a byte to [1]; the loop
 * polls [0]. Lets the loop block with no timeout while there is no controller.
 */
static int		g_wake[2] = { -1, -1 };

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
	(void)fprintf(stderr, "bluetoothd %s ", tbuf);

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

	bluetoothd_got_term = sig;
	/* Wake a poll() that is blocked with no timeout. */
	if (g_wake[1] >= 0)
		(void)write(g_wake[1], "", 1);
	errno = saved;
}

void
bluetoothd_lock(void)
{
	(void)pthread_mutex_lock(&g_lock);
}

void
bluetoothd_unlock(void)
{
	(void)pthread_mutex_unlock(&g_lock);
}

int
bluetoothd_adapter_count_locked(void)
{
	int i, n = 0;

	for (i = 0; i < BT_MAX_ADAPTERS; i++) {
		if (g_adapters[i].active)
			n++;
	}
	return (n);
}

struct bt_adapter *
bluetoothd_adapter_at_locked(int idx)
{
	int i, n = 0;

	if (idx < 0)
		return (NULL);
	for (i = 0; i < BT_MAX_ADAPTERS; i++) {
		if (!g_adapters[i].active)
			continue;
		if (n == idx)
			return (&g_adapters[i]);
		n++;
	}
	return (NULL);
}

struct bt_adapter *
bluetoothd_adapter_by_node_locked(const char *node)
{
	int i;

	if (node == NULL)
		return (NULL);
	for (i = 0; i < BT_MAX_ADAPTERS; i++) {
		if (g_adapters[i].active &&
		    strcmp(g_adapters[i].node, node) == 0)
			return (&g_adapters[i]);
	}
	return (NULL);
}

bt_daemon_state_t
bluetoothd_state_locked(void)
{
	if (!g_registry)
		return (bt_daemon_no_ioregistry_e);
	return (bluetoothd_adapter_count_locked() > 0 ? bt_daemon_active_e
	                                              : bt_daemon_idle_e);
}

void
bluetoothd_prefs_locked(struct bt_prefs *out)
{
	*out = g_prefs;
}

static struct bt_adapter *
adapter_by_entry_locked(uint64_t id)
{
	int i;

	for (i = 0; i < BT_MAX_ADAPTERS; i++) {
		if (g_adapters[i].active && g_adapters[i].entry_id == id)
			return (&g_adapters[i]);
	}
	return (NULL);
}

/* Publish State:/Bluetooth from the current table. */
static void
publish_state(void)
{
	struct bt_daemon_summary s;
	const char *nodes[BT_MAX_ADAPTERS];
	int i;

	(void)memset(&s, 0, sizeof(s));
	s.npaired = bt_store_paired_count();

	bluetoothd_lock();
	s.state = bluetoothd_state_locked();
	for (i = 0; i < BT_MAX_ADAPTERS; i++) {
		if (g_adapters[i].active)
			nodes[s.nadapters++] = g_adapters[i].node;
	}
	s.adapters = nodes;
	s.prefs = g_prefs;
	/* Publish under the lock: `nodes` points into the table. */
	(void)bt_state_publish_daemon(&s);
	bluetoothd_unlock();
}

/*
 * A controller appeared: an IOBluetoothHostController entry matched. Record
 * it, with the power state it should come up in. Bringing it up (loading the
 * upper kernel stack, building the netgraph graph, HCI init, attaching the
 * engines) is the adapter lifecycle's job (#207); it starts from here.
 */
static void
adapter_arrived(const struct bt_iokit_event *ev)
{
	struct bt_adapter *a;
	int i;

	bluetoothd_lock();
	if (adapter_by_entry_locked(ev->entry_id) != NULL) {
		bluetoothd_unlock();
		return;				/* already ours */
	}
	a = NULL;
	for (i = 0; i < BT_MAX_ADAPTERS && a == NULL; i++) {
		if (!g_adapters[i].active)
			a = &g_adapters[i];
	}
	if (a == NULL) {
		bluetoothd_unlock();
		xlog("controller table full (%d): ignoring %s",
		    BT_MAX_ADAPTERS, ev->node);
		return;
	}

	(void)memset(a, 0, sizeof(*a));
	a->active = true;
	a->entry_id = ev->entry_id;
	(void)strlcpy(a->node, ev->node, sizeof(a->node));
	(void)strlcpy(a->transport,
	    ev->transport[0] != '\0' ? ev->transport : "USB",
	    sizeof(a->transport));
	a->vendor = ev->vendor;
	a->product = ev->product;
	a->state = bt_adapter_arrived_e;
	a->want_power = (g_prefs.power_state != 0);

	xlog("controller %s arrived (%s %04x:%04x, registry id %llu); "
	    "power %s per ControllerPowerState", a->node, a->transport,
	    a->vendor, a->product, (unsigned long long)a->entry_id,
	    a->want_power ? "on" : "off");
	bluetoothd_unlock();
}

/*
 * A controller went away: its entry was retired (unplugged, or its kext
 * unloaded). Detach its engines and free the slot. The netgraph nodes went
 * with the device.
 */
static void
adapter_departed(const struct bt_iokit_event *ev)
{
	struct bt_adapter *a;
	int i;

	bluetoothd_lock();
	a = adapter_by_entry_locked(ev->entry_id);
	if (a == NULL) {
		bluetoothd_unlock();
		return;
	}
	xlog("controller %s departed", a->node);
	for (i = 0; i < BT_ENGINE_MAX; i++) {
		if (a->engines[i] != NULL && bt_engines[i]->detach != NULL)
			bt_engines[i]->detach(a->engines[i]);
		a->engines[i] = NULL;
	}
	(void)memset(a, 0, sizeof(*a));
	bluetoothd_unlock();
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
	struct bt_iokit_event ev;
	struct sigaction sa;
	int i, j, npaired;

	(void)argc;
	(void)argv;

	xlog("bluetoothd starting");

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);
	(void)signal(SIGPIPE, SIG_IGN);

	/* Before anything can write to it: the signal handler checks for -1. */
	if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0) {
		xlog("pipe2: %s; checking for controllers every second",
		    strerror(errno));
		g_wake[0] = g_wake[1] = -1;
	}

	/* umask 077 and the preferences directory, before any store I/O. */
	bt_store_init();

	/*
	 * The Mach service comes up FIRST, before any controller work. A
	 * client that looks us up early gets an answer ("no controller yet"),
	 * not a bootstrap failure.
	 */
	if (mach_service_start() != 0)
		xlog("mach_service_start failed; RPC off");

	/* The persisted policy. A missing or corrupt file reads as defaults. */
	bluetoothd_lock();
	bt_prefs_load(&g_prefs);
	bluetoothd_unlock();
	npaired = bt_store_paired_count();
	xlog("policy: ControllerPowerState=%d DiscoverableState=%s "
	    "AutoConnect=%s; %d paired device(s)", g_prefs.power_state,
	    g_prefs.discoverable ? "on" : "off",
	    g_prefs.autoconnect ? "on" : "off", npaired);
	for (i = 0; i < BT_ENGINE_MAX && bt_engines[i] != NULL; i++)
		xlog("engine %s registered", bt_engines[i]->name);

	/*
	 * Follow controllers through the IORegistry. There is no fallback:
	 * without /dev/ioregistry no controller can be seen, so bluetoothd
	 * stays up to answer `bluetooth status` and says why.
	 */
	g_registry = (iokit_watch_start(g_wake[1]) == 0);
	if (g_registry)
		xlog("no controller yet; waiting for %s", BT_IOKIT_CLASS);
	else
		xlog("no /dev/ioregistry; controllers cannot be seen");

	/*
	 * The SDP server (sdp_server.h). The controller bring-up starts it
	 * once it has loaded the L2CAP sockets; this covers a daemon restart
	 * with the stack already loaded. Without the stack it fails quietly.
	 */
	(void)sdp_server_start();

	/*
	 * configd may not be up yet (there is no launchd service ordering);
	 * btstate.c opens the store lazily and the next change retries.
	 */
	publish_state();

	/*
	 * Event loop. Poll the self-pipe and every attached engine's event
	 * fd. With nothing attached the loop blocks with no timeout until a
	 * controller arrives or a signal does.
	 */
	while (!bluetoothd_got_term) {
		struct pollfd pfd[BT_MAX_ADAPTERS * BT_ENGINE_MAX + 1];
		struct bt_engine *pe[BT_MAX_ADAPTERS * BT_ENGINE_MAX];
		int peidx[BT_MAX_ADAPTERS * BT_ENGINE_MAX];
		int nfd = 0, npfd, rc;
		bool changed = false;

		bluetoothd_lock();
		for (i = 0; i < BT_MAX_ADAPTERS; i++) {
			if (!g_adapters[i].active)
				continue;
			for (j = 0; j < BT_ENGINE_MAX; j++) {
				struct bt_engine *e = g_adapters[i].engines[j];
				int fd;

				if (e == NULL || bt_engines[j]->event_fd == NULL)
					continue;
				fd = bt_engines[j]->event_fd(e);
				if (fd < 0)
					continue;
				pfd[nfd].fd = fd;
				pfd[nfd].events = POLLIN;
				pfd[nfd].revents = 0;
				pe[nfd] = e;
				peidx[nfd] = j;
				nfd++;
			}
		}
		bluetoothd_unlock();

		npfd = nfd;
		if (g_wake[0] >= 0) {
			pfd[npfd].fd = g_wake[0];
			pfd[npfd].events = POLLIN;
			pfd[npfd].revents = 0;
			npfd++;
		}

		rc = poll(npfd > 0 ? pfd : NULL, (nfds_t)npfd,
		    g_wake[0] >= 0 ? -1 : 1000);
		if (rc < 0 && errno != EINTR) {
			xlog("poll: %s", strerror(errno));
			break;
		}
		if (rc > 0) {
			bluetoothd_lock();
			for (i = 0; i < nfd; i++) {
				if ((pfd[i].revents & (POLLIN | POLLHUP)) == 0)
					continue;
				if (bt_engines[peidx[i]]->pump != NULL &&
				    bt_engines[peidx[i]]->pump(pe[i]))
					changed = true;
			}
			bluetoothd_unlock();
			if (npfd > nfd && (pfd[nfd].revents & POLLIN) != 0)
				drain_wake_pipe();
		}
		if (bluetoothd_got_term)
			break;

		/* Controllers that came or went, in the order they did. */
		while (iokit_watch_next(&ev)) {
			if (ev.kind == BT_IOKIT_ARRIVED)
				adapter_arrived(&ev);
			else
				adapter_departed(&ev);
			changed = true;
		}

		if (changed)
			publish_state();
	}

	xlog("bluetoothd exiting on signal %d", (int)bluetoothd_got_term);

	/* Detach every engine, and take the daemon's key out of the store. */
	bluetoothd_lock();
	for (i = 0; i < BT_MAX_ADAPTERS; i++) {
		if (!g_adapters[i].active)
			continue;
		for (j = 0; j < BT_ENGINE_MAX; j++) {
			if (g_adapters[i].engines[j] != NULL &&
			    bt_engines[j]->detach != NULL)
				bt_engines[j]->detach(g_adapters[i].engines[j]);
			g_adapters[i].engines[j] = NULL;
		}
		g_adapters[i].active = false;
	}
	bluetoothd_unlock();
	sdp_server_stop();
	(void)bt_state_remove(BT_STATE_KEY);
	bt_state_close();

	mach_service_join();
	return (0);
}
