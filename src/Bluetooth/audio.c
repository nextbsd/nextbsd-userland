/*
 * audio.c — the audio policy: the A2DP source's mixer, profile routing and
 * State:/Bluetooth/Device/<addr>/Audio. See audio.h.
 *
 * Everything in g_devs is touched on the audio queue only. What the main loop
 * publishes and the Mach thread reports is a copy, g_snap, kept under its own
 * mutex, so neither of them ever waits for the audio queue.
 */
#include "audio.h"
#include "a2dp_sink.h"
#include "avrcp.h"
#include "bluetoothd.h"
#include "btstate.h"
#include "btstore.h"
#include "child.h"
#include "connections.h"
#include "hfp.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * virtual_oss's control device ioctl (usr.sbin/virtual_oss/virtual_oss.h,
 * which is not installed): the argument is a command line, parsed as if it
 * had been given at startup. This is what virtual_oss_cmd(8) sends.
 */
#define	VOSS_OPTIONS_MAX	1024
#define	VOSS_ADD_OPTIONS	_IOWR('O', 33, char [VOSS_OPTIONS_MAX])

#define	BASENAME_CLONE		"hw.snd.basename_clone"
#define	PIDFILE			"/var/run/bluetoothd.audio.pids"

#define	DEV_MAX			16	/* linked devices tracked */
#define	SNAP_MAX		(DEV_MAX + AUDIO_MAX)
#define	RUN_OK_MS		60000	/* a mixer that ran this long resets
					   the backoff */

struct adev {
	bool			used;
	struct bt_addr		peer;
	char			addr[BT_ADDR_STRLEN];	/* display form */
	char			node[32];	/* the controller */
	uint32_t		gen;		/* matches its timers */
	bool			link;		/* ACL up */

	/* A2DP source: the mixer. */
	struct child		*mixer;
	int			slot;		/* dsp.bt<slot>; -1: none */
	bool			default_out;	/* -d dsp */
	int			saved_clone;	/* basename_clone before it */
	bool			start_pending;
	uint64_t		started_ms;
	int			backoff_ms;
	audio_route_t		route;

	/* HFP: where the mixer points while on HFP, and its codec. */
	char			hfp_dev[32];	/* "/dev/btsco0"; "": /dev/null */
	char			hfp_codec[8];	/* "CVSD", "mSBC" */

	/* AVRCP, the sink and HFP, as their modules report them. */
	bool			hfp;		/* service level connection */
	bool			hfp_audio;	/* SCO up */
	int			battery;	/* percent; -1 unknown */
	bool			avrcp;
	int			volume;
	bool			sink;
	char			sink_codec[8];
};

struct snap {
	bool			used;
	bool			dirty;
	bool			gone;		/* remove its key */
	struct audio_status	st;
};

static dispatch_queue_t	g_q;
static dispatch_queue_t	g_ctlq;		/* retargets, in order */
static pthread_once_t	g_q_once = PTHREAD_ONCE_INIT;
static struct adev	g_devs[DEV_MAX];
static bool		g_slots[AUDIO_MAX];
static uint32_t		g_gen;
static bool		g_shutdown;

static pthread_mutex_t	g_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct snap	g_snap[SNAP_MAX];

static void	consider(struct adev *d);

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[audio] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static bool
same(const struct bt_addr *a, const struct bt_addr *b)
{
	return (memcmp(a->b, b->b, BT_ADDR_LEN) == 0);
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void
q_init(void)
{
	g_q = dispatch_queue_create("org.nextbsd.bluetoothd.audio", NULL);
	g_ctlq = dispatch_queue_create("org.nextbsd.bluetoothd.audio.ctl",
	    NULL);
}

dispatch_queue_t
audio_queue(void)
{
	(void)pthread_once(&g_q_once, q_init);
	return (g_q);
}

/* ---- the published copy ------------------------------------------------- */

static void
fill_status(const struct adev *d, struct audio_status *st)
{
	(void)memset(st, 0, sizeof(*st));
	st->peer = d->peer;
	st->volume = d->volume;
	st->avrcp = d->avrcp;
	st->hfp = d->hfp;
	st->hfp_audio = d->hfp_audio;
	st->battery = d->battery;
	if (d->mixer != NULL) {
		st->role = AUDIO_ROLE_SOURCE;
		st->route = d->route;
		st->active = (d->route == AUDIO_ROUTE_A2DP) || d->hfp_audio;
		st->default_out = d->default_out;
		(void)snprintf(st->node, sizeof(st->node), "dsp.bt%d",
		    d->slot);
		(void)strlcpy(st->codec, d->route == AUDIO_ROUTE_HFP ?
		    d->hfp_codec : "SBC", sizeof(st->codec));
	} else if (d->sink) {
		st->role = AUDIO_ROLE_SINK;
		st->route = AUDIO_ROUTE_A2DP;
		st->active = true;
		(void)strlcpy(st->codec, d->sink_codec, sizeof(st->codec));
	}
}

/* Does `d` have anything worth publishing? */
static bool
has_audio(const struct adev *d)
{
	return (d->mixer != NULL || d->sink || d->avrcp || d->hfp);
}

/* Record what `d` looks like now; the main loop publishes it. */
static void
snap_update(const struct adev *d)
{
	struct audio_status st;
	bool show = d->used && has_audio(d);
	int i, slot = -1;

	if (show)
		fill_status(d, &st);
	(void)pthread_mutex_lock(&g_snap_lock);
	for (i = 0; i < SNAP_MAX; i++) {
		if (g_snap[i].used && same(&g_snap[i].st.peer, &d->peer)) {
			slot = i;
			break;
		}
		if (!g_snap[i].used && slot < 0)
			slot = i;
	}
	if (slot >= 0 && (show || g_snap[slot].used)) {
		if (show) {
			g_snap[slot].st = st;
			g_snap[slot].gone = false;
		} else
			g_snap[slot].gone = true;
		g_snap[slot].used = true;
		g_snap[slot].dirty = true;
	}
	(void)pthread_mutex_unlock(&g_snap_lock);
	bluetoothd_wake();
}

void
audio_publish(void)
{
	struct snap work[SNAP_MAX];
	char key[BT_ADDR_STRLEN], path[160];
	int i, n = 0;

	(void)pthread_mutex_lock(&g_snap_lock);
	for (i = 0; i < SNAP_MAX; i++) {
		if (!g_snap[i].used || !g_snap[i].dirty)
			continue;
		work[n++] = g_snap[i];
		g_snap[i].dirty = false;
		if (g_snap[i].gone)
			g_snap[i].used = false;
	}
	(void)pthread_mutex_unlock(&g_snap_lock);

	for (i = 0; i < n; i++) {
		(void)bt_addr_ntoa(&work[i].st.peer, BT_ADDR_FMT_KEY, key,
		    sizeof(key));
		(void)snprintf(path, sizeof(path), "%s%s/Audio",
		    BT_STATE_DEVICE_PREFIX, key);
		if (work[i].gone)
			(void)bt_state_remove(path);
		else
			(void)bt_state_publish_audio(path, &work[i].st);
	}
	if (n > 0)
		bt_state_notify(BT_NOTIFY_AUDIO);
}

bool
audio_status(const struct bt_addr *peer, struct audio_status *out)
{
	bool found = false;
	int i;

	(void)pthread_mutex_lock(&g_snap_lock);
	for (i = 0; i < SNAP_MAX && !found; i++) {
		if (g_snap[i].used && !g_snap[i].gone &&
		    same(&g_snap[i].st.peer, peer)) {
			*out = g_snap[i].st;
			found = true;
		}
	}
	(void)pthread_mutex_unlock(&g_snap_lock);
	return (found);
}

bool
audio_status_at(int idx, struct audio_status *out)
{
	int i, n = 0;
	bool found = false;

	(void)pthread_mutex_lock(&g_snap_lock);
	for (i = 0; i < SNAP_MAX && !found; i++) {
		if (!g_snap[i].used || g_snap[i].gone)
			continue;
		if (n++ == idx) {
			*out = g_snap[i].st;
			found = true;
		}
	}
	(void)pthread_mutex_unlock(&g_snap_lock);
	return (found);
}

/* ---- the device table (audio queue) ------------------------------------- */

static struct adev *
dev_find(const struct bt_addr *peer)
{
	int i;

	for (i = 0; i < DEV_MAX; i++) {
		if (g_devs[i].used && same(&g_devs[i].peer, peer))
			return (&g_devs[i]);
	}
	return (NULL);
}

static struct adev *
dev_add(const struct bt_addr *peer)
{
	struct adev *d = dev_find(peer);
	int i;

	for (i = 0; d == NULL && i < DEV_MAX; i++) {
		if (!g_devs[i].used)
			d = &g_devs[i];
	}
	if (d == NULL || d->used)
		return (d);
	(void)memset(d, 0, sizeof(*d));
	d->used = true;
	d->peer = *peer;
	d->peer.type = BT_ADDR_PUBLIC;
	(void)bt_addr_ntoa(&d->peer, BT_ADDR_FMT_DISPLAY, d->addr,
	    sizeof(d->addr));
	d->slot = -1;
	d->volume = -1;
	d->battery = -1;
	d->gen = ++g_gen;
	d->route = AUDIO_ROUTE_A2DP;
	d->backoff_ms = AUDIO_RESTART_MIN_MS;
	return (d);
}

/* Forget `d` once nothing is left of it. */
static void
dev_gc(struct adev *d)
{
	if (d->link || d->mixer != NULL || d->sink || d->avrcp || d->hfp ||
	    d->start_pending)
		return;
	d->used = false;
	snap_update(d);		/* removes its key */
}

/* Tell the policy layer a profile is up or down over the link. */
static void
report(const struct adev *d, uint32_t profile, bool up)
{
	bluetoothd_lock();
	if (!up || conn_state_locked(&d->peer) == bt_device_connected_e)
		conn_profile_locked(&d->peer, profile, up);
	bluetoothd_unlock();
}

/*
 * Should we play to `peer`: it is paired, its bond says it is an A2DP sink,
 * and its Audio switch is on.
 */
static bool
wants_source(const struct bt_addr *peer)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct bt_addr p = *peer;
	bool yes;

	if (b == NULL)
		return (false);
	p.type = BT_ADDR_PUBLIC;
	yes = bt_store_bond_find(&p, false, b) &&
	    (b->profiles & BT_PROFILE_A2DP_SINK) != 0 && !b->no_audio;
	free(b);
	return (yes);
}

bool
audio_peer_is_sink(const struct bt_addr *peer)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct bt_addr p = *peer;
	bool yes;

	if (b == NULL)
		return (false);
	p.type = BT_ADDR_PUBLIC;
	yes = bt_store_bond_find(&p, false, b) &&
	    (b->profiles & BT_PROFILE_A2DP_SINK) != 0;
	free(b);
	return (yes);
}

/* ---- the pid file: mixers a crashed daemon left behind ------------------- */

static void
mixers_record(void)
{
	FILE *f;
	int i;

	f = fopen(PIDFILE ".tmp", "w");
	if (f == NULL)
		return;
	for (i = 0; i < DEV_MAX; i++) {
		if (g_devs[i].used && g_devs[i].mixer != NULL)
			(void)fprintf(f, "%d\n", (int)child_pid(g_devs[i].mixer));
	}
	if (fclose(f) == 0)
		(void)rename(PIDFILE ".tmp", PIDFILE);
}

/* Is `pid` a running virtual_oss? */
static bool
is_mixer(pid_t pid)
{
	struct kinfo_proc kp;
	size_t len = sizeof(kp);
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };

	if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len != sizeof(kp))
		return (false);
	return (strcmp(kp.ki_comm, "virtual_oss") == 0);
}

void
audio_init(void)
{
	FILE *f;
	int pid, n = 0;

	(void)audio_queue();
	f = fopen(PIDFILE, "r");
	if (f == NULL)
		return;
	while (fscanf(f, "%d", &pid) == 1) {
		if (pid > 1 && is_mixer((pid_t)pid)) {
			(void)kill((pid_t)pid, SIGTERM);
			n++;
		}
	}
	(void)fclose(f);
	(void)unlink(PIDFILE);
	if (n > 0)
		xlog("stopped %d mixer(s) left by an earlier bluetoothd", n);
}

/* ---- the mixer (audio queue) --------------------------------------------- */

static int
clone_get(void)
{
	int v = -1;
	size_t len = sizeof(v);

	if (sysctlbyname(BASENAME_CLONE, &v, &len, NULL, 0) != 0)
		return (-1);
	return (v);
}

/* Does any mixer own /dev/dsp? */
static bool
default_taken(void)
{
	int i;

	for (i = 0; i < DEV_MAX; i++) {
		if (g_devs[i].used && g_devs[i].mixer != NULL &&
		    g_devs[i].default_out)
			return (true);
	}
	return (false);
}

static void	mixer_exited(void *ctx, struct child *c, int status);
static void	avrcp_check(void *arg);

struct tick {
	struct bt_addr	peer;
	uint32_t	gen;
};

static void
tick_after(struct adev *d, int ms, dispatch_function_t fn)
{
	struct tick *t = malloc(sizeof(*t));

	if (t == NULL)
		return;
	t->peer = d->peer;
	t->gen = d->gen;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)ms * 1000000), audio_queue(), t, fn);
}

static struct adev *
tick_dev(struct tick *t)
{
	struct adev *d = dev_find(&t->peer);

	if (d != NULL && d->gen != t->gen)
		d = NULL;
	free(t);
	return (d);
}

static void
mixer_start(struct adev *d)
{
	struct bt_prefs prefs;
	char dsp[32], ctl[40], bt[64];
	char *argv[32];
	int i, n = 0;

	for (i = 0; i < AUDIO_MAX; i++) {
		if (!g_slots[i])
			break;
	}
	if (i == AUDIO_MAX) {
		xlog("%s: %d headsets already play; not starting another",
		    d->addr, AUDIO_MAX);
		return;
	}
	d->slot = i;
	(void)snprintf(dsp, sizeof(dsp), "dsp.bt%d", d->slot);
	(void)snprintf(ctl, sizeof(ctl), "dsp.bt%d.ctl", d->slot);
	(void)snprintf(bt, sizeof(bt), "/dev/bluetooth/%s", d->addr);

	bluetoothd_lock();
	bluetoothd_prefs_locked(&prefs);
	bluetoothd_unlock();
	d->saved_clone = clone_get();
	d->default_out = prefs.autoswitch_audio && !default_taken() &&
	    d->saved_clone >= 0;

	argv[n++] = __DECONST(char *, "virtual_oss");
	argv[n++] = __DECONST(char *, "-S");		/* resample clients */
	argv[n++] = __DECONST(char *, "-C");
	argv[n++] = __DECONST(char *, "2");
	argv[n++] = __DECONST(char *, "-c");
	argv[n++] = __DECONST(char *, "2");
	argv[n++] = __DECONST(char *, "-r");
	argv[n++] = __DECONST(char *, "48000");
	argv[n++] = __DECONST(char *, "-b");
	argv[n++] = __DECONST(char *, "16");
	argv[n++] = __DECONST(char *, "-s");
	argv[n++] = __DECONST(char *, "768");	/* 16 ms */
	argv[n++] = __DECONST(char *, "-R");
	argv[n++] = __DECONST(char *, "/dev/null");
	argv[n++] = __DECONST(char *, "-P");
	argv[n++] = bt;
	argv[n++] = __DECONST(char *, "-d");
	argv[n++] = dsp;
	if (d->default_out) {
		argv[n++] = __DECONST(char *, "-d");
		argv[n++] = __DECONST(char *, "dsp");
	}
	argv[n++] = __DECONST(char *, "-t");
	argv[n++] = ctl;
	argv[n] = NULL;

	d->mixer = child_spawn(AUDIO_MIXER, argv, audio_queue(),
	    mixer_exited, d);
	if (d->mixer == NULL) {
		d->slot = -1;
		d->default_out = false;
		return;
	}
	g_slots[d->slot] = true;
	d->started_ms = now_ms();
	d->route = AUDIO_ROUTE_A2DP;
	xlog("%s: playing to it through /dev/%s%s", d->addr, dsp,
	    d->default_out ? " and /dev/dsp" : "");
	mixers_record();
	report(d, BT_PROFILE_A2DP_SINK, true);
	snap_update(d);
	if (!d->avrcp)
		tick_after(d, AUDIO_AVRCP_WAIT_MS, avrcp_check);
	/* A mixer restarted under a call starts on A2DP: HFP re-points it. */
	if (d->hfp_audio)
		hfp_mixer_started(&d->peer);
}

static void
start_timer(void *arg)
{
	struct adev *d = tick_dev(arg);

	if (d == NULL)
		return;
	d->start_pending = false;
	if (d->mixer == NULL && d->link && !g_shutdown &&
	    wants_source(&d->peer))
		mixer_start(d);
	dev_gc(d);
}

/* Put hw.snd.basename_clone back if the mixer that cleared it could not. */
static void
clone_restore(int saved)
{
	int v = saved;

	if (saved <= 0 || clone_get() != 0)
		return;
	if (sysctlbyname(BASENAME_CLONE, NULL, NULL, &v, sizeof(v)) == 0)
		xlog("%s put back to %d", BASENAME_CLONE, saved);
}

static void
mixer_exited(void *ctx, struct child *c, int status)
{
	struct adev *d = ctx;
	uint64_t ran;

	(void)status;
	if (d->mixer != c)
		return;
	d->mixer = NULL;
	if (d->slot >= 0)
		g_slots[d->slot] = false;
	d->slot = -1;
	if (d->default_out)
		clone_restore(d->saved_clone);
	d->default_out = false;
	mixers_record();
	report(d, BT_PROFILE_A2DP_SINK, false);
	snap_update(d);

	/* Still wanted: start it again, later each time. */
	if (d->link && !g_shutdown && wants_source(&d->peer)) {
		ran = now_ms() - d->started_ms;
		if (ran >= RUN_OK_MS)
			d->backoff_ms = AUDIO_RESTART_MIN_MS;
		xlog("%s: the mixer stopped; starting it again in %d ms",
		    d->addr, d->backoff_ms);
		d->start_pending = true;
		tick_after(d, d->backoff_ms, start_timer);
		d->backoff_ms *= 2;
		if (d->backoff_ms > AUDIO_RESTART_MAX_MS)
			d->backoff_ms = AUDIO_RESTART_MAX_MS;
		return;
	}
	dev_gc(d);
}

static void
mixer_stop(struct adev *d, const char *why)
{
	if (d->mixer == NULL)
		return;
	xlog("%s: stopping the mixer: %s", d->addr, why);
	child_stop(d->mixer, AUDIO_STOP_GRACE_MS);
}

/* The headset has not opened AVRCP itself: we do. */
static void
avrcp_check(void *arg)
{
	struct adev *d = tick_dev(arg);

	if (d == NULL || d->mixer == NULL || !d->link || d->avrcp)
		return;
	avrcp_connect(&d->peer);
}

/* Start or stop the mixer to match the link, the bond and the switch. */
static void
consider(struct adev *d)
{
	bool want = d->link && !g_shutdown && wants_source(&d->peer);

	if (want && d->mixer == NULL && !d->start_pending) {
		d->start_pending = true;
		d->backoff_ms = AUDIO_RESTART_MIN_MS;
		tick_after(d, AUDIO_START_DELAY_MS, start_timer);
	} else if (!want && d->mixer != NULL)
		mixer_stop(d, d->link ? "audio is off for it" :
		    "the link is down");
	dev_gc(d);
}

/* ---- from the policy layer -------------------------------------------- */

struct link_ev {
	struct bt_addr	peer;
	char		node[32];
	int		what;
};

enum { EV_UP, EV_DOWN, EV_BOND, EV_ADAPTER_DOWN };

static void
link_ev(void *arg)
{
	struct link_ev *e = arg;
	struct adev *d;
	int i;

	switch (e->what) {
	case EV_UP:
		d = dev_add(&e->peer);
		if (d == NULL)
			break;
		d->link = true;
		(void)strlcpy(d->node, e->node, sizeof(d->node));
		consider(d);
		hfp_link_up(&e->peer);
		break;
	case EV_DOWN:
		hfp_link_down(&e->peer);
		d = dev_find(&e->peer);
		if (d == NULL)
			break;
		d->link = false;
		d->gen = ++g_gen;	/* pending start and AVRCP checks die */
		d->start_pending = false;
		avrcp_disconnect(&d->peer);
		a2dp_sink_link_down(&d->peer);
		consider(d);
		break;
	case EV_BOND:
		hfp_bond_updated(&e->peer);
		d = dev_find(&e->peer);
		if (d != NULL)
			consider(d);
		break;
	case EV_ADAPTER_DOWN:
		for (i = 0; i < DEV_MAX; i++) {
			d = &g_devs[i];
			if (!d->used || !d->link ||
			    strcmp(d->node, e->node) != 0)
				continue;
			hfp_link_down(&d->peer);
			d->link = false;
			d->gen = ++g_gen;
			d->start_pending = false;
			avrcp_disconnect(&d->peer);
			a2dp_sink_link_down(&d->peer);
			consider(d);
		}
		break;
	}
	free(e);
}

static void
post(const struct bt_addr *peer, const char *node, int what)
{
	struct link_ev *e = calloc(1, sizeof(*e));

	if (e == NULL)
		return;
	if (peer != NULL)
		e->peer = *peer;
	if (node != NULL)
		(void)strlcpy(e->node, node, sizeof(e->node));
	e->what = what;
	dispatch_async_f(audio_queue(), e, link_ev);
}

void
audio_link_up_locked(const struct bt_addr *peer, const char *node)
{
	post(peer, node, EV_UP);
}

void
audio_link_down_locked(const struct bt_addr *peer)
{
	post(peer, NULL, EV_DOWN);
}

void
audio_bond_updated_locked(const struct bt_addr *peer)
{
	post(peer, NULL, EV_BOND);
}

void
audio_adapter_down_locked(const char *node)
{
	post(NULL, node, EV_ADAPTER_DOWN);
}

/* ---- from avrcp.c and a2dp_sink.c (audio queue) ------------------------- */

void
audio_avrcp_changed(const struct bt_addr *peer, bool up)
{
	struct adev *d = up ? dev_add(peer) : dev_find(peer);

	if (d == NULL || d->avrcp == up)
		return;
	d->avrcp = up;
	if (!up)
		d->volume = -1;
	report(d, BT_PROFILE_AVRCP, up);
	snap_update(d);
	if (!up)
		dev_gc(d);
}

void
audio_volume_changed(const struct bt_addr *peer, int volume)
{
	struct adev *d = dev_find(peer);

	if (d == NULL || d->volume == volume)
		return;
	d->volume = volume;
	snap_update(d);
}

void
audio_sink_changed(const struct bt_addr *peer, bool up, const char *codec)
{
	struct adev *d = up ? dev_add(peer) : dev_find(peer);

	if (d == NULL || d->sink == up)
		return;
	d->sink = up;
	(void)strlcpy(d->sink_codec, codec != NULL ? codec : "",
	    sizeof(d->sink_codec));
	report(d, BT_PROFILE_A2DP_SOURCE, up);
	snap_update(d);
	if (!up)
		dev_gc(d);
}

struct hfp_ev {
	struct bt_addr	peer;
	bool		slc;
	bool		sco;
	int		battery;
};

static void
hfp_ev(void *arg)
{
	struct hfp_ev *e = arg;
	struct adev *d = e->slc ? dev_add(&e->peer) : dev_find(&e->peer);

	if (d != NULL) {
		if (d->hfp != e->slc)
			report(d, BT_PROFILE_HFP, e->slc);
		d->hfp = e->slc;
		d->hfp_audio = e->slc && e->sco;
		d->battery = e->slc ? e->battery : -1;
		snap_update(d);
		if (!e->slc)
			dev_gc(d);
	}
	free(e);
}

void
audio_hfp_changed(const struct bt_addr *peer, bool slc, bool sco,
    int battery)
{
	struct hfp_ev *e = calloc(1, sizeof(*e));

	if (e == NULL)
		return;
	e->peer = *peer;
	e->slc = slc;
	e->sco = sco;
	e->battery = battery;
	dispatch_async_f(audio_queue(), e, hfp_ev);
}

/* ---- the RPCs ---------------------------------------------------------- */

struct call {
	struct bt_addr	peer;
	int		arg;
	bt_status_t	st;
};

static void
enabled_q(void *arg)
{
	struct call *c = arg;
	struct adev *d = dev_find(&c->peer);

	if (d != NULL)
		consider(d);
}

bt_status_t
audio_set_enabled(const struct bt_addr *peer, bool on)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct call c;
	bt_status_t st;

	if (b == NULL)
		return (bt_status_internal_error_e);
	bluetoothd_lock();
	if (!bt_store_bond_find(peer, true, b))
		st = bt_status_not_paired_e;
	else {
		b->no_audio = !on;
		st = bt_store_bond_save(b) == 0 ? bt_status_success_e :
		    bt_status_store_error_e;
	}
	bluetoothd_unlock();
	(void)memset(b, 0, sizeof(*b));
	free(b);
	if (st != bt_status_success_e)
		return (st);
	c.peer = *peer;
	dispatch_sync_f(audio_queue(), &c, enabled_q);
	return (bt_status_success_e);
}

static void
volume_q(void *arg)
{
	struct call *c = arg;
	struct adev *d = dev_find(&c->peer);

	if (d == NULL || !d->avrcp)
		c->st = bt_status_not_connected_e;
	else
		c->st = avrcp_set_volume(&c->peer, c->arg);
}

bt_status_t
audio_set_volume(const struct bt_addr *peer, int volume)
{
	struct call c;

	if (volume < 0 || volume > 127)
		return (bt_status_invalid_parameter_e);
	c.peer = *peer;
	c.arg = volume;
	dispatch_sync_f(audio_queue(), &c, volume_q);
	return (c.st);
}

void
audio_prefs_changed(void)
{
	/* AutoSwitchAudio applies when a mixer starts; the sink re-reads. */
	a2dp_sink_prefs_changed();
}

/* ---- routing (HFP's hook) ---------------------------------------------- */

struct retarget {
	char	ctl[48];
	char	opts[VOSS_OPTIONS_MAX];
	char	addr[BT_ADDR_STRLEN];
};

/*
 * Send one command line to a mixer's control device. Off the audio queue:
 * the ioctl is served by the mixer, which may be busy in a page. On a queue
 * of its own, so two switches in a row reach the mixer in order.
 */
static void
retarget_io(void *arg)
{
	struct retarget *r = arg;
	int fd;

	fd = open(r->ctl, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		xlog("%s: %s: %s", r->addr, r->ctl, strerror(errno));
	else {
		if (ioctl(fd, VOSS_ADD_OPTIONS, r->opts) != 0)
			xlog("%s: retarget failed: %s", r->addr,
			    strerror(errno));
		else if (r->opts[0] != '\0')
			xlog("%s: mixer says: %s", r->addr, r->opts);
		(void)close(fd);
	}
	free(r);
}

struct route_call {
	struct bt_addr	peer;
	audio_route_t	route;
	char		dev[32];
	char		codec[8];
	bool		force;		/* resend: the mixer restarted */
	bt_status_t	st;
};

static void
route_q(void *arg)
{
	struct route_call *c = arg;
	struct adev *d = dev_find(&c->peer);
	struct retarget *r;

	if (d == NULL || d->mixer == NULL) {
		c->st = bt_status_not_connected_e;
		return;
	}
	c->st = bt_status_success_e;
	if (!c->force && d->route == c->route &&
	    (c->route != AUDIO_ROUTE_HFP || strcmp(d->hfp_dev, c->dev) == 0))
		return;
	r = calloc(1, sizeof(*r));
	if (r == NULL) {
		c->st = bt_status_internal_error_e;
		return;
	}
	(void)snprintf(r->ctl, sizeof(r->ctl), "/dev/dsp.bt%d.ctl", d->slot);
	(void)strlcpy(r->addr, d->addr, sizeof(r->addr));
	if (c->route == AUDIO_ROUTE_A2DP)
		(void)snprintf(r->opts, sizeof(r->opts),
		    "-R /dev/null -P /dev/bluetooth/%s", d->addr);
	else if (c->dev[0] != '\0')
		/* The hands-free bridge: the mic in, the earpiece out. */
		(void)snprintf(r->opts, sizeof(r->opts), "-R %s -P %s",
		    c->dev, c->dev);
	else
		(void)strlcpy(r->opts, "-P /dev/null", sizeof(r->opts));
	d->route = c->route;
	(void)strlcpy(d->hfp_dev, c->route == AUDIO_ROUTE_HFP ? c->dev : "",
	    sizeof(d->hfp_dev));
	(void)strlcpy(d->hfp_codec, c->codec, sizeof(d->hfp_codec));
	xlog("%s: audio now over %s%s%s", d->addr,
	    d->route == AUDIO_ROUTE_A2DP ? "A2DP" : "HFP",
	    d->route == AUDIO_ROUTE_HFP && c->dev[0] != '\0' ? " via " : "",
	    d->route == AUDIO_ROUTE_HFP ? c->dev : "");
	dispatch_async_f(g_ctlq, r, retarget_io);
	snap_update(d);
}

bt_status_t
audio_route_to(const struct bt_addr *peer, audio_route_t route,
    const char *dev, const char *codec, bool force)
{
	struct route_call c;

	if (route != AUDIO_ROUTE_A2DP && route != AUDIO_ROUTE_HFP)
		return (bt_status_invalid_parameter_e);
	(void)memset(&c, 0, sizeof(c));
	c.peer = *peer;
	c.route = route;
	c.force = force;
	if (dev != NULL)
		(void)strlcpy(c.dev, dev, sizeof(c.dev));
	if (codec != NULL)
		(void)strlcpy(c.codec, codec, sizeof(c.codec));
	dispatch_sync_f(audio_queue(), &c, route_q);
	return (c.st);
}

bt_status_t
audio_route(const struct bt_addr *peer, audio_route_t route)
{
	return (audio_route_to(peer, route, NULL, NULL, false));
}

/* ---- shutdown ---------------------------------------------------------- */

static void
shutdown_q(void *arg)
{
	int i;

	(void)arg;
	g_shutdown = true;
	for (i = 0; i < DEV_MAX; i++) {
		if (g_devs[i].used && g_devs[i].mixer != NULL)
			(void)kill(child_pid(g_devs[i].mixer), SIGTERM);
		if (g_devs[i].used)
			avrcp_disconnect(&g_devs[i].peer);
	}
	a2dp_sink_shutdown();
}

void
audio_shutdown(void)
{
	/* First, and not from the audio queue: HFP syncs onto it. */
	hfp_shutdown();
	dispatch_sync_f(audio_queue(), NULL, shutdown_q);
}
