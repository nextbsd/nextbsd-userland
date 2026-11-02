/*
 * connections.c — connected devices, reconnect policy and per-device state.
 * See connections.h.
 */
#include "connections.h"
#include "adapter.h"
#include "btstate.h"
#include "btstore.h"
#include "discovery.h"
#include "hci.h"
#include "hid_host.h"
#include "pairing.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define	CONN_MAX	16	/* links at once, over every controller */
#define	AUTOQ_MAX	32	/* automatic reconnects waiting */
#define	FRESH_MAX	8	/* new bonds waiting for their SDP query */
#define	DIRTY_MAX	64	/* devices whose key needs rewriting */

struct conn {
	bool			used;
	struct bt_addr		peer;		/* LE: the identity address */
	int			transport;	/* BT_TRANSPORT_* */
	char			node[32];
	bt_device_state_t	state;	/* connecting or connected */
	int			handle;
	bool			incoming;
	bool			encrypted;
	int8_t			rssi;
	uint32_t		cod;
	uint32_t		profiles;	/* running over the link */
	uint64_t		deadline;	/* "connecting" ends by then */
};

struct autoq {
	bool		used;
	struct bt_addr	peer;
	char		node[32];
	uint64_t	when;
};

struct fresh {
	bool		used;
	struct bt_addr	peer;
	uint64_t	expires;
};

struct dirty {
	bool		used;
	struct bt_addr	peer;
};

static struct conn	g_conns[CONN_MAX];
static struct autoq	g_autoq[AUTOQ_MAX];
static struct fresh	g_fresh[FRESH_MAX];
static struct dirty	g_dirty[DIRTY_MAX];

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[conn] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static const char *
ntoa(const struct bt_addr *a, char *buf)
{
	return (bt_addr_ntoa(a, BT_ADDR_FMT_DISPLAY, buf, BT_ADDR_STRLEN));
}

/*
 * The six octets are the device: a Classic address is public, and an LE
 * link is recorded under its identity address.
 */
static bool
same(const struct bt_addr *a, const struct bt_addr *b)
{
	return (memcmp(a->b, b->b, BT_ADDR_LEN) == 0);
}

static struct conn *
find(const struct bt_addr *peer)
{
	int i;

	for (i = 0; i < CONN_MAX; i++) {
		if (g_conns[i].used && same(&g_conns[i].peer, peer))
			return (&g_conns[i]);
	}
	return (NULL);
}

static struct conn *
add(const struct bt_addr *peer, const char *node)
{
	struct conn *c = find(peer);
	int i;

	for (i = 0; c == NULL && i < CONN_MAX; i++) {
		if (!g_conns[i].used)
			c = &g_conns[i];
	}
	if (c == NULL)
		return (NULL);
	if (!c->used) {
		(void)memset(c, 0, sizeof(*c));
		c->used = true;
		c->peer = *peer;
		c->transport = BT_TRANSPORT_BREDR;
		c->handle = -1;
		c->rssi = 127;
		c->state = bt_device_disconnected_e;
	}
	if (node != NULL)
		(void)strlcpy(c->node, node, sizeof(c->node));
	return (c);
}

void
conn_device_changed_locked(const struct bt_addr *peer)
{
	int i, slot = -1;

	for (i = 0; i < DIRTY_MAX; i++) {
		if (g_dirty[i].used && same(&g_dirty[i].peer, peer))
			return;
		if (!g_dirty[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return;		/* the next change of this device catches up */
	g_dirty[slot].used = true;
	g_dirty[slot].peer = *peer;
	bluetoothd_wake();
}

static void
forget(struct conn *c)
{
	struct bt_addr peer = c->peer;

	(void)memset(c, 0, sizeof(*c));
	conn_device_changed_locked(&peer);
}

/*
 * The bond of `peer` on the heap; NULL if not paired. An address typed
 * without "/random" also finds an LE device whose identity is a static
 * random address; b->addr then has the type the bond was stored under.
 */
static struct bt_bond *
bond_get(const struct bt_addr *peer, bool with_keys)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct bt_addr p = *peer;

	if (b == NULL)
		return (NULL);
	if (bt_store_bond_find(&p, with_keys, b))
		return (b);
	if (p.type == BT_ADDR_PUBLIC) {
		p.type = BT_ADDR_RANDOM;
		if (bt_store_bond_find(&p, with_keys, b))
			return (b);
	}
	explicit_bzero(b, sizeof(*b));
	free(b);
	return (NULL);
}

static struct bt_bond *
bond_copy(const struct bt_addr *peer)
{
	return (bond_get(peer, false));
}

/*
 * The transport to reach `peer` over: LE for a random address or an LE-only
 * bond, Classic for the rest (a dual-mode device's Classic HID is preferred,
 * as pairing prefers it).
 */
static int
transport_of(const struct bt_addr *peer, const struct bt_bond *b)
{
	if (peer->type == BT_ADDR_RANDOM ||
	    (b != NULL && b->transports == BT_TRANSPORT_LE))
		return (BT_TRANSPORT_LE);
	return (BT_TRANSPORT_BREDR);
}

/* A HID device the HID host can drive: it has a descriptor, or boots. */
static bool
is_hid(const struct bt_bond *b)
{
	return ((b->profiles & BT_PROFILE_HID) != 0 &&
	    ((b->flags & BT_BOND_HID_DESC) != 0 ||
	    (b->flags & BT_BOND_HID_ATTRS) != 0));
}

static bool
powered_engine(struct bt_adapter *a, int transport)
{
	const struct bt_engine_ops *ops;

	return (a != NULL && a->powered &&
	    (a->state == bt_adapter_powered_on_e) &&
	    adapter_engine_locked(a, transport, &ops) != NULL);
}

/* The controller to reach `peer` through: its link's, else the first on. */
static struct bt_adapter *
pick(const struct bt_addr *peer, int transport)
{
	struct conn *c = find(peer);
	struct bt_adapter *a;
	int i, n = bluetoothd_adapter_count_locked();

	if (c != NULL && c->node[0] != '\0') {
		a = bluetoothd_adapter_by_node_locked(c->node);
		if (powered_engine(a, transport))
			return (a);
	}
	for (i = 0; i < n; i++) {
		a = bluetoothd_adapter_at_locked(i);
		if (powered_engine(a, transport))
			return (a);
	}
	return (NULL);
}

/* ---- link events ----------------------------------------------------------- */

/*
 * An incoming link is kept only from a paired, Trusted device, or the one
 * being paired. Anything else is dropped and logged.
 */
static bool
allowed_in(const struct bt_addr *peer)
{
	struct bt_bond *b;
	bool ok;

	if (pairing_active_locked(peer))
		return (true);
	b = bond_copy(peer);
	ok = (b != NULL && b->trusted);
	free(b);
	return (ok);
}

static void
touch_bond(const struct bt_addr *peer)
{
	struct bt_bond *b = bond_get(peer, true);

	if (b == NULL)
		return;
	b->last_connected = time(NULL);
	(void)bt_store_bond_save(b);
	explicit_bzero(b, sizeof(*b));
	free(b);
}

/*
 * An LE link (le_engine.c): the LE engine only links to bonded devices and
 * the pairing in progress, and reconnects its bonds itself, so here it is
 * only recorded and published. Its events carry no link details.
 */
static void
le_event(struct bt_adapter *a, const struct bt_event *ev)
{
	struct bt_bond *b;
	struct conn *c;

	switch (ev->kind) {
	case BT_EV_CONNECTED:
		c = add(&ev->peer, a->node);
		if (c == NULL)
			return;
		c->transport = BT_TRANSPORT_LE;
		c->state = bt_device_connected_e;
		c->handle = 0;
		c->rssi = 127;
		c->deadline = 0;
		b = bond_copy(&ev->peer);
		if (b != NULL && (b->profiles & BT_PROFILE_HOGP) != 0)
			c->profiles |= BT_PROFILE_HOGP;
		free(b);
		touch_bond(&ev->peer);
		conn_device_changed_locked(&ev->peer);
		break;
	case BT_EV_DISCONNECTED:
		c = find(&ev->peer);
		if (c != NULL)
			forget(c);
		break;
	default:
		break;
	}
}

void
conn_event_locked(struct bt_adapter *a, const struct bt_event *ev)
{
	const struct bt_engine_ops *ops;
	struct bt_engine *e;
	struct conn *c;
	char addr[BT_ADDR_STRLEN];
	int i;

	if (ev->transport == BT_TRANSPORT_LE) {
		le_event(a, ev);
		return;
	}
	if (ev->transport != BT_TRANSPORT_BREDR)
		return;
	switch (ev->kind) {
	case BT_EV_CONNECTED:
		if (ev->u.link.incoming && !allowed_in(&ev->peer)) {
			xlog("%s: rejecting an incoming connection from %s: "
			    "not paired and trusted", a->node,
			    ntoa(&ev->peer, addr));
			e = adapter_classic_locked(a, &ops);
			if (e != NULL && ops->disconnect != NULL)
				(void)ops->disconnect(e, &ev->peer);
			return;
		}
		c = add(&ev->peer, a->node);
		if (c == NULL)
			return;
		c->state = bt_device_connected_e;
		c->handle = ev->u.link.handle;
		c->incoming = ev->u.link.incoming;
		c->encrypted = ev->u.link.encrypted;
		c->rssi = ev->u.link.rssi;
		if (ev->u.link.class_of_device != 0)
			c->cod = ev->u.link.class_of_device;
		c->deadline = 0;
		for (i = 0; i < AUTOQ_MAX; i++) {
			if (g_autoq[i].used && same(&g_autoq[i].peer,
			    &ev->peer))
				g_autoq[i].used = false;
		}
		touch_bond(&ev->peer);
		conn_device_changed_locked(&ev->peer);
		break;

	case BT_EV_DISCONNECTED:
		c = find(&ev->peer);
		if (c == NULL)
			return;
		/*
		 * A failed attempt (no handle) while the HID host is still
		 * paging keeps "connecting"; its own result ends it.
		 */
		if (ev->u.link.handle < 0 && c->state ==
		    bt_device_connecting_e && hid_host_active(&ev->peer))
			return;
		forget(c);
		break;

	case BT_EV_LINK_CHANGED:
		c = find(&ev->peer);
		if (c == NULL || c->state != bt_device_connected_e)
			return;
		if (c->encrypted != ev->u.link.encrypted ||
		    c->rssi != ev->u.link.rssi) {
			c->encrypted = ev->u.link.encrypted;
			c->rssi = ev->u.link.rssi;
			conn_device_changed_locked(&ev->peer);
		}
		break;

	default:
		break;
	}
}

void
conn_profile_locked(const struct bt_addr *peer, uint32_t profile, bool up)
{
	struct conn *c = find(peer);

	if (up) {
		c = add(peer, NULL);
		if (c == NULL)
			return;
		if (c->state != bt_device_connected_e)
			c->state = bt_device_connected_e;
		c->profiles |= profile;
		c->deadline = 0;
	} else {
		if (c == NULL)
			return;
		c->profiles &= ~profile;
		/* A profile that never got a link up ends the attempt. */
		if (c->state == bt_device_connecting_e && c->handle < 0) {
			forget(c);
			return;
		}
	}
	conn_device_changed_locked(peer);
}

/* ---- connect and disconnect ------------------------------------------------ */

/*
 * Connect `peer` through controller `a`: a Classic HID device gets its HID
 * channels, another Classic device an ACL link, an LE device the LE
 * engine's connect (which brings HOGP up by itself).
 */
static bt_status_t
connect_on(struct bt_adapter *a, const struct bt_addr *peer_in)
{
	const struct bt_engine_ops *ops;
	struct bt_engine *e;
	struct bt_bond *b;
	struct bt_addr p = *peer_in;
	const struct bt_addr *peer = &p;
	struct conn *c;
	bt_status_t st;
	bool hid;
	int transport;

	b = bond_copy(peer);
	if (b != NULL)
		p = b->addr;	/* the type it was stored under */
	transport = transport_of(peer, b);
	hid = (transport == BT_TRANSPORT_BREDR && b != NULL && is_hid(b));
	free(b);
	e = adapter_engine_locked(a, transport, &ops);
	if (e == NULL || ops->connect == NULL)
		return (bt_status_operation_not_supported_e);

	c = find(peer);
	if (c != NULL && c->state == bt_device_connected_e &&
	    (!hid || (c->profiles & BT_PROFILE_HID) != 0))
		return (bt_status_success_e);

	if (hid)
		st = hid_host_connect(a->have_addr ? &a->addr : NULL, peer);
	else
		st = ops->connect(e, peer);
	if (st != bt_status_success_e)
		return (st);
	c = add(peer, a->node);
	if (c != NULL && c->state != bt_device_connected_e) {
		c->transport = transport;
		c->state = bt_device_connecting_e;
		c->deadline = bt_now_ms() + CONN_CONNECT_GUARD_MS;
		conn_device_changed_locked(peer);
	}
	bluetoothd_wake();
	return (bt_status_success_e);
}

bt_status_t
conn_connect_locked(const struct bt_addr *peer)
{
	struct bt_adapter *a;
	struct bt_bond *b;
	int transport;

	b = bond_copy(peer);
	transport = transport_of(peer, b);
	free(b);
	a = pick(peer, transport);
	if (a == NULL)
		return (bluetoothd_adapter_count_locked() == 0 ?
		    bt_status_no_adapter_e : bt_status_powered_off_e);
	return (connect_on(a, peer));
}

void
conn_acl_disconnect_locked(const struct bt_addr *peer)
{
	static const int transports[] = { BT_TRANSPORT_BREDR,
	    BT_TRANSPORT_LE };
	const struct bt_engine_ops *ops;
	struct bt_engine *e;
	struct conn *c = find(peer);
	struct bt_addr p = *peer;
	int i, t, n = bluetoothd_adapter_count_locked();

	if (c != NULL)
		p = c->peer;	/* with its type: an LE identity may be random */
	for (t = 0; t < 2; t++) {
		/* The link's own transport first. */
		int tr = (c != NULL && c->transport == BT_TRANSPORT_LE) ?
		    transports[1 - t] : transports[t];

		for (i = 0; i < n; i++) {
			struct bt_adapter *a = bluetoothd_adapter_at_locked(i);

			e = a != NULL ? adapter_engine_locked(a, tr, &ops) :
			    NULL;
			if (e != NULL && ops->disconnect != NULL &&
			    ops->disconnect(e, &p) == bt_status_success_e)
				return;
		}
	}
}

bt_status_t
conn_disconnect_locked(const struct bt_addr *peer)
{
	struct conn *c = find(peer);
	int i;

	for (i = 0; i < AUTOQ_MAX; i++) {
		if (g_autoq[i].used && same(&g_autoq[i].peer, peer))
			g_autoq[i].used = false;
	}
	if (c == NULL && !hid_host_active(peer))
		return (bt_status_not_connected_e);
	/* The HID channels first; the link once they are closed. */
	if (!hid_host_disconnect(peer, true))
		conn_acl_disconnect_locked(peer);
	bluetoothd_wake();
	return (bt_status_success_e);
}

/* ---- status ---------------------------------------------------------------- */

bool
conn_status_locked(const struct bt_addr *peer, struct conn_status *out)
{
	struct conn *c = find(peer);

	(void)memset(out, 0, sizeof(*out));
	out->state = bt_device_disconnected_e;
	out->rssi = 127;
	if (pairing_active_locked(peer))
		out->state = bt_device_pairing_e;
	if (c == NULL)
		return (out->state != bt_device_disconnected_e);
	if (out->state != bt_device_pairing_e)
		out->state = c->state;
	(void)strlcpy(out->node, c->node, sizeof(out->node));
	out->rssi = c->rssi;
	out->profiles = c->profiles;
	out->class_of_device = c->cod;
	out->encrypted = c->encrypted;
	return (true);
}

bt_device_state_t
conn_state_locked(const struct bt_addr *peer)
{
	struct conn_status st;

	(void)conn_status_locked(peer, &st);
	return (st.state);
}

/* ---- the reconnect policy -------------------------------------------------- */

static void
enqueue(struct bt_adapter *a, const struct bt_addr *peer, uint64_t when)
{
	int i, slot = -1;

	for (i = 0; i < AUTOQ_MAX; i++) {
		if (g_autoq[i].used && same(&g_autoq[i].peer, peer))
			return;
		if (!g_autoq[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return;
	g_autoq[slot].used = true;
	g_autoq[slot].peer = *peer;
	(void)strlcpy(g_autoq[slot].node, a->node, sizeof(g_autoq[slot].node));
	g_autoq[slot].when = when;
}

void
conn_adapter_up_locked(struct bt_adapter *a)
{
	struct bt_prefs prefs;
	struct bt_bond *b;
	char addr[BT_ADDR_STRLEN];
	uint64_t when = bt_now_ms() + CONN_AUTO_FIRST_MS;
	int i, queued = 0, waiting = 0;

	bluetoothd_prefs_locked(&prefs);
	if (!prefs.autoconnect) {
		xlog("%s: AutoConnect is off; not reconnecting", a->node);
		return;
	}
	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return;
	/*
	 * Classic only: the LE engine reconnects its own bonds, from a
	 * background scan for their advertisements (le_engine.c).
	 */
	for (i = 0; bt_store_paired_at(i, b); i++) {
		if (!b->autoconnect || b->addr.type != BT_ADDR_PUBLIC ||
		    (b->transports & BT_TRANSPORT_BREDR) == 0) {
			(void)memset(b, 0, sizeof(*b));
			continue;
		}
		/*
		 * A HID device that has had its first session and says it
		 * reconnects itself is left to page us: paging it would only
		 * race its own reconnect (and wake a sleeping keyboard).
		 */
		if (is_hid(b) &&
		    (b->hid_flags & BT_HID_SESSION_ESTABLISHED) != 0 &&
		    (b->hid_flags & BT_HID_RECONNECT_INITIATE) != 0) {
			xlog("%s: %s reconnects by itself; listening",
			    a->node, ntoa(&b->addr, addr));
			waiting++;
		} else {
			enqueue(a, &b->addr, when);
			when += CONN_AUTO_GAP_MS;
			queued++;
		}
		(void)memset(b, 0, sizeof(*b));
	}
	free(b);
	if (queued + waiting > 0)
		xlog("%s: %d paired device(s) to reconnect, %d to wait for",
		    a->node, queued, waiting);
	bluetoothd_wake();
}

void
conn_adapter_down_locked(struct bt_adapter *a)
{
	int i;

	for (i = 0; i < AUTOQ_MAX; i++) {
		if (g_autoq[i].used && strcmp(g_autoq[i].node, a->node) == 0)
			g_autoq[i].used = false;
	}
	/* Its links are gone with it; their sockets die on their own. */
	for (i = 0; i < CONN_MAX; i++) {
		if (g_conns[i].used && strcmp(g_conns[i].node, a->node) == 0)
			forget(&g_conns[i]);
	}
}

void
conn_paired_locked(const struct bt_addr *peer)
{
	int i, slot = -1;

	for (i = 0; i < FRESH_MAX; i++) {
		if (g_fresh[i].used && same(&g_fresh[i].peer, peer)) {
			slot = i;
			break;
		}
		if (!g_fresh[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		slot = 0;	/* the oldest goes */
	g_fresh[slot].used = true;
	g_fresh[slot].peer = *peer;
	g_fresh[slot].expires = bt_now_ms() + 120000;
	conn_device_changed_locked(peer);
}

void
conn_bond_refreshed_locked(const struct bt_addr *peer)
{
	struct bt_bond *b;
	char addr[BT_ADDR_STRLEN];
	bool fresh = false;
	int i;

	for (i = 0; i < FRESH_MAX; i++) {
		if (g_fresh[i].used && same(&g_fresh[i].peer, peer)) {
			g_fresh[i].used = false;
			fresh = true;
		}
	}
	conn_device_changed_locked(peer);
	hid_host_bond_updated(peer);
	if (!fresh)
		return;
	/* Its first HID session is ours to open (HID Profile, §5.4.2). */
	b = bond_copy(peer);
	if (b != NULL && is_hid(b) && !hid_host_active(peer)) {
		xlog("%s: newly paired HID device; connecting",
		    ntoa(peer, addr));
		(void)conn_connect_locked(peer);
	}
	free(b);
}

void
conn_publish_paired_locked(void)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	int i;

	if (b == NULL)
		return;
	for (i = 0; bt_store_paired_at(i, b); i++)
		conn_device_changed_locked(&b->addr);
	free(b);
}

static uint64_t
earlier(uint64_t t, uint64_t u)
{
	if (u == 0)
		return (t);
	return (t == 0 || u < t ? u : t);
}

uint64_t
conn_next_timer_locked(void)
{
	uint64_t t = 0;
	int i;

	for (i = 0; i < AUTOQ_MAX; i++) {
		if (g_autoq[i].used)
			t = earlier(t, g_autoq[i].when);
	}
	for (i = 0; i < CONN_MAX; i++) {
		if (g_conns[i].used &&
		    g_conns[i].state == bt_device_connecting_e)
			t = earlier(t, g_conns[i].deadline);
	}
	for (i = 0; i < FRESH_MAX; i++) {
		if (g_fresh[i].used)
			t = earlier(t, g_fresh[i].expires);
	}
	return (t);
}

void
conn_timers_locked(uint64_t now)
{
	char addr[BT_ADDR_STRLEN];
	int i;

	for (i = 0; i < AUTOQ_MAX; i++) {
		struct autoq *q = &g_autoq[i];
		struct bt_adapter *a;
		bt_status_t st;

		if (!q->used || now < q->when)
			continue;
		q->used = false;
		a = bluetoothd_adapter_by_node_locked(q->node);
		if (!powered_engine(a, BT_TRANSPORT_BREDR))
			continue;
		st = connect_on(a, &q->peer);
		xlog("%s: reconnecting %s%s", q->node, ntoa(&q->peer, addr),
		    st == bt_status_success_e ? "" : " failed to start");
	}
	for (i = 0; i < CONN_MAX; i++) {
		struct conn *c = &g_conns[i];

		if (c->used && c->state == bt_device_connecting_e &&
		    c->deadline != 0 && now >= c->deadline)
			forget(c);
	}
	for (i = 0; i < FRESH_MAX; i++) {
		if (g_fresh[i].used && now >= g_fresh[i].expires)
			g_fresh[i].used = false;
	}
}

/* ---- publication ----------------------------------------------------------- */

void
conn_publish(void)
{
	struct work {
		struct bt_addr		peer;
		struct conn		c;	/* .used: connected or connecting */
		char			name[BT_NAME_MAX + 1];
	} *work;
	struct bt_device_summary sum;
	struct bt_bond *b;
	char key[BT_ADDR_STRLEN], path[128], node[32];
	int i, n = 0;

	work = calloc(DIRTY_MAX, sizeof(*work));
	b = calloc(1, sizeof(*b));
	if (work == NULL || b == NULL) {
		free(work);
		free(b);
		return;
	}

	bluetoothd_lock();
	for (i = 0; i < DIRTY_MAX; i++) {
		struct conn *c;
		struct bt_adapter *a;

		if (!g_dirty[i].used)
			continue;
		g_dirty[i].used = false;
		work[n].peer = g_dirty[i].peer;
		c = find(&work[n].peer);
		if (c != NULL) {
			work[n].c = *c;
			a = bluetoothd_adapter_by_node_locked(c->node);
			if (a != NULL)
				(void)disc_name(a, &work[n].peer,
				    work[n].name, sizeof(work[n].name));
		}
		n++;
	}
	bluetoothd_unlock();

	for (i = 0; i < n; i++) {
		bool paired;

		(void)memset(b, 0, sizeof(*b));
		paired = bt_store_bond_find(&work[i].peer, false, b);
		(void)bt_addr_ntoa(&work[i].peer, BT_ADDR_FMT_KEY, key,
		    sizeof(key));
		(void)snprintf(path, sizeof(path), "%s%s",
		    BT_STATE_DEVICE_PREFIX, key);
		if (!paired && !work[i].c.used) {
			(void)bt_state_remove(path);
			continue;
		}
		(void)memset(&sum, 0, sizeof(sum));
		sum.addr = &work[i].peer;
		sum.name = (paired && b->name[0] != '\0') ? b->name :
		    work[i].name;
		sum.class_of_device = (paired && b->class_of_device != 0) ?
		    b->class_of_device : work[i].c.cod;
		(void)strlcpy(node, work[i].c.node, sizeof(node));
		sum.adapter = node;
		sum.paired = paired;
		sum.connected = work[i].c.used &&
		    work[i].c.state == bt_device_connected_e;
		sum.connecting = work[i].c.used &&
		    work[i].c.state == bt_device_connecting_e;
		sum.profiles = sum.connected ? work[i].c.profiles : 0;
		sum.rssi = sum.connected ? work[i].c.rssi : 127;
		(void)bt_state_publish_device(path, &sum);
	}
	free(b);
	free(work);
	if (n > 0)
		bt_state_notify(BT_NOTIFY_DEVICE);
}
