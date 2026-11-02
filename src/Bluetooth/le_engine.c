/*
 * le_engine.c — the LE engine behind engine.h (#205). See le.h for the parts.
 *
 * GAP over the shared raw HCI socket:
 *
 *   scanning      LE_Set_Scan_Parameters / LE_Set_Scan_Enable. Active while
 *                 bt_scan runs (names, appearance and services from scan
 *                 responses, merged into the same scan list as Classic);
 *                 passive in the background while a bonded device with
 *                 AutoConnect is not connected; off while a connection is
 *                 being made, which some controllers cannot do while scanning.
 *   connecting    an L2CAP socket on CID 4 (ATT) with the peer's LE address
 *                 type; the kernel issues LE_Create_Connection. A second
 *                 socket on CID 6 (SMP) shares the link. bluetoothd cancels
 *                 (LE_Create_Connection_Cancel) an attempt the device does
 *                 not answer within LE_CONNECT_MS, before ng_hci's own 60 s
 *                 timeout would leave the controller initiating.
 *   reconnecting  a bonded device that advertises (connectable, or directed
 *                 at us) is connected as soon as the background scan sees
 *                 it, its private address resolved with its IRK. Then
 *                 LE_Start_Encryption with the stored LTK, before any GATT
 *                 traffic, so HID reads never race the encryption.
 *   LTK requests  LE_Long_Term_Key_Request (subevent 0x05) is answered from
 *                 the store, as the kernel leaves it to bluetoothd.
 *   identity      bonds are keyed by the identity address the device gave
 *                 in key distribution; resolvable private addresses are
 *                 matched with each bond's IRK (ah, §2.2.2 of Vol 3 Part H).
 *
 * The engine's sockets live in one kqueue, the engine's event_fd, so the main
 * loop polls one descriptor however many links there are.
 *
 * Clean-room: written from the Core specification (v5.4) and the kernel's
 * socket interface; no BlueZ, NimBLE, Zephyr or BTstack code.
 */
#include "le.h"
#include "hci.h"
#include "hci_compat.h"

#include <sys/event.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	OGF_LC		NG_HCI_OGF_LINK_CONTROL
#define	OGF_LE		NG_HCI_OGF_LE

/* Scan timing, in 0.625 ms units. */
#define	SCAN_ACTIVE_INTERVAL	0x0030	/* 30 ms, scanning all of it */
#define	SCAN_ACTIVE_WINDOW	0x0030
#define	SCAN_PASSIVE_INTERVAL	0x0060	/* 60 ms, half of it: leaves */
#define	SCAN_PASSIVE_WINDOW	0x0030	/* air time for Wi-Fi on combo cards */

/* LE Advertising Report event types. */
#define	ADV_IND			0x00
#define	ADV_DIRECT_IND		0x01
#define	ADV_SCAN_RSP		0x04

#define	RESCAN_MS		2000	/* after a refused LE_Set_Scan_Enable */
#define	KEY_MISSING_RETRY_MS	60000	/* the device forgot us */

static uint32_t	g_prompt_id;

uint32_t
le_prompt_id(void)
{
	/* The top bit keeps them apart from the Classic engine's ids. */
	if (++g_prompt_id > 0x7fffffff)
		g_prompt_id = 1;
	return (0x80000000u | g_prompt_id);
}

void
le_log(struct bt_engine *e, const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[le] %s: ", e->node);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

void
le_raise(struct bt_engine *e, const struct bt_event *ev)
{
	if (e->host.event != NULL)
		e->host.event(e->host.ctx, e, ev);
}

static const char *
ntoa(const struct bt_addr *a, char *buf)
{
	return (bt_addr_ntoa(a, BT_ADDR_FMT_DISPLAY, buf, BT_ADDR_STRLEN));
}

static void
raise_simple(struct bt_engine *e, bt_event_kind_t kind,
    const struct bt_addr *peer)
{
	struct bt_event ev;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.transport = BT_TRANSPORT_LE;
	if (peer != NULL)
		ev.peer = *peer;
	le_raise(e, &ev);
}

static void
raise_discovery(struct bt_engine *e, bool scanning)
{
	struct bt_event ev;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_DISCOVERY;
	ev.transport = BT_TRANSPORT_LE;
	ev.u.scanning = scanning;
	le_raise(e, &ev);
}

/* ---- the kqueue -------------------------------------------------------------- */

/* udata carries the slot and its generation, so a reused slot is told apart. */
static void *
conn_cookie(struct bt_engine *e, struct le_conn *c)
{
	uintptr_t slot = (uintptr_t)(c - e->conns);

	return ((void *)(slot | ((uintptr_t)c->gen << 8)));
}

static struct le_conn *
conn_from_cookie(struct bt_engine *e, void *udata)
{
	uintptr_t u = (uintptr_t)udata;
	struct le_conn *c;

	if ((u & 0xff) >= LE_CONN_MAX)
		return (NULL);
	c = &e->conns[u & 0xff];
	if (c->state == LC_FREE || (uint32_t)(u >> 8) != c->gen)
		return (NULL);
	return (c);
}

void
le_watch(struct bt_engine *e, int fd, struct le_conn *c, bool write)
{
	struct kevent kev;

	EV_SET(&kev, fd, write ? EVFILT_WRITE : EVFILT_READ,
	    EV_ADD | (write ? EV_ONESHOT : 0), 0, 0, conn_cookie(e, c));
	if (kevent(e->kq, &kev, 1, NULL, 0, NULL) < 0)
		le_log(e, "kevent: %s", strerror(errno));
}

void
le_unwatch(struct bt_engine *e, int fd)
{
	struct kevent kev[2];

	/* Closing the descriptor drops them anyway; this is for tidiness. */
	EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	(void)kevent(e->kq, &kev[0], 1, NULL, 0, NULL);
	(void)kevent(e->kq, &kev[1], 1, NULL, 0, NULL);
}

/* ---- HCI ---------------------------------------------------------------------- */

int
le_hci_cmd(struct bt_engine *e, uint16_t ocf, const void *p, size_t len)
{
	if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LE, ocf), p, len) < 0) {
		le_log(e, "send LE command 0x%04x failed: %s",
		    HCI_OPCODE(OGF_LE, ocf), strerror(errno));
		return (-1);
	}
	return (0);
}

int
le_start_encryption(struct le_conn *c, const uint8_t ltk[16], uint16_t ediv,
    uint64_t rand)
{
	ng_hci_le_start_encryption_cp cp;
	int rc;

	if (c->handle < 0)
		return (-1);
	cp.connection_handle = htole16((uint16_t)c->handle);
	cp.random_number = htole64(rand);
	cp.encrypted_diversifier = htole16(ediv);
	(void)memcpy(cp.long_term_key, ltk, sizeof(cp.long_term_key));
	rc = le_hci_cmd(c->e, NG_HCI_OCF_LE_START_ENCRYPTION, &cp, sizeof(cp));
	explicit_bzero(&cp, sizeof(cp));
	return (rc);
}

/* The LE connection handle to `a`, from ng_hci's connection list. */
static int
handle_of(struct bt_engine *e, const struct bt_addr *a)
{
	struct ng_btsocket_hci_raw_con_list r;
	ng_hci_node_con_ep *list;
	int i, h = -1;

	list = calloc(NG_HCI_MAX_CON_NUM, sizeof(*list));
	if (list == NULL)
		return (-1);
	(void)memset(&r, 0, sizeof(r));
	r.num_connections = NG_HCI_MAX_CON_NUM;
	r.connections = list;
	if (ioctl(e->fd, SIOC_HCI_RAW_NODE_GET_CON_LIST, &r) == 0) {
		for (i = 0; i < (int)r.num_connections; i++) {
			if ((list[i].link_type == NG_HCI_LINK_LE_PUBLIC ||
			    list[i].link_type == NG_HCI_LINK_LE_RANDOM) &&
			    memcmp(&list[i].bdaddr, a->b, BT_ADDR_LEN) == 0) {
				h = list[i].con_handle;
				break;
			}
		}
	}
	free(list);
	return (h);
}

/* ---- addresses and bonds ---------------------------------------------------------- */

bool
le_rpa_match(const uint8_t irk[16], const struct bt_addr *a)
{
	uint8_t hash[3];

	/* A resolvable private address: random, top two bits 01. */
	if (a->type != BT_ADDR_RANDOM || (a->b[5] & 0xc0) != 0x40)
		return (false);
	if (le_ah(irk, a->b + 3, hash) != 0)
		return (false);
	return (memcmp(hash, a->b, 3) == 0);
}

/* The bond `a` belongs to: its identity address, or resolved by an IRK. */
static int
bond_for(struct bt_engine *e, const struct bt_addr *a)
{
	int i;

	for (i = 0; i < e->nbonds; i++) {
		if (bt_addr_equal(&e->bonds[i].identity, a))
			return (i);
	}
	for (i = 0; i < e->nbonds; i++) {
		if (e->bonds[i].has_irk && le_rpa_match(e->bonds[i].irk, a))
			return (i);
	}
	return (-1);
}

static int
bond_put(struct bt_engine *e, const struct bt_addr *identity, bool has_irk,
    const uint8_t irk[16], bool autoconnect)
{
	int i;

	for (i = 0; i < e->nbonds; i++) {
		if (bt_addr_equal(&e->bonds[i].identity, identity))
			break;
	}
	if (i == e->nbonds) {
		if (e->nbonds >= LE_BOND_MAX)
			return (-1);
		e->nbonds++;
		(void)memset(&e->bonds[i], 0, sizeof(e->bonds[i]));
	}
	e->bonds[i].identity = *identity;
	e->bonds[i].has_irk = has_irk;
	if (has_irk)
		(void)memcpy(e->bonds[i].irk, irk, 16);
	e->bonds[i].autoconnect = autoconnect;
	e->bonds[i].retry_at = 0;
	e->reconcile = true;
	return (i);
}

/* ---- links ----------------------------------------------------------------------- */

static struct le_conn *
conn_by_handle(struct bt_engine *e, int handle)
{
	int i;

	for (i = 0; i < LE_CONN_MAX; i++) {
		if (e->conns[i].state != LC_FREE &&
		    e->conns[i].handle == handle)
			return (&e->conns[i]);
	}
	return (NULL);
}

/* The link to `a`: by its air address, its identity, or the six octets. */
static struct le_conn *
conn_by_addr(struct bt_engine *e, const struct bt_addr *a)
{
	int i;

	for (i = 0; i < LE_CONN_MAX; i++) {
		struct le_conn *c = &e->conns[i];

		if (c->state == LC_FREE)
			continue;
		if (bt_addr_equal(&c->addr, a) ||
		    bt_addr_equal(&c->identity, a) ||
		    memcmp(c->addr.b, a->b, BT_ADDR_LEN) == 0)
			return (c);
	}
	return (NULL);
}

static bool
bond_connected(struct bt_engine *e, int bond)
{
	int i;

	for (i = 0; i < LE_CONN_MAX; i++) {
		if (e->conns[i].state != LC_FREE && e->conns[i].bond == bond)
			return (true);
	}
	return (false);
}

static bool
connecting(struct bt_engine *e)
{
	int i;

	for (i = 0; i < LE_CONN_MAX; i++) {
		if (e->conns[i].state == LC_CONNECTING)
			return (true);
	}
	return (false);
}

/* ---- scanning --------------------------------------------------------------------- */

static le_scan_t
scan_wanted(struct bt_engine *e)
{
	int i;

	if (!e->powered || connecting(e) || e->rescan_at != 0)
		return (LE_SCAN_OFF);
	if (e->user_scan)
		return (LE_SCAN_ACTIVE);
	for (i = 0; i < e->nbonds; i++) {
		if (e->bonds[i].autoconnect && !bond_connected(e, i))
			return (LE_SCAN_PASSIVE);
	}
	return (LE_SCAN_OFF);
}

static void
scan_enable(struct bt_engine *e, bool on)
{
	ng_hci_le_set_scan_enable_cp cp;

	cp.le_scan_enable = on ? 1 : 0;
	cp.filter_duplicates = 1;
	if (le_hci_cmd(e, NG_HCI_OCF_LE_SET_SCAN_ENABLE, &cp,
	    sizeof(cp)) == 0 && e->scan_qn < (int)sizeof(e->scan_q))
		e->scan_q[e->scan_qn++] = cp.le_scan_enable;
}

/* Make the controller scan the way the engine wants now. */
static void
scan_apply(struct bt_engine *e)
{
	ng_hci_le_set_scan_parameters_cp sp;
	le_scan_t want = scan_wanted(e);

	if (want == e->scan)
		return;
	if (e->scan != LE_SCAN_OFF)
		scan_enable(e, false);
	if (want != LE_SCAN_OFF) {
		bool active = want == LE_SCAN_ACTIVE;

		sp.le_scan_type = active ? 1 : 0;
		sp.le_scan_interval = htole16(active ? SCAN_ACTIVE_INTERVAL :
		    SCAN_PASSIVE_INTERVAL);
		sp.le_scan_window = htole16(active ? SCAN_ACTIVE_WINDOW :
		    SCAN_PASSIVE_WINDOW);
		sp.own_address_type = 0;	/* public */
		sp.scanning_filter_policy = 0;	/* all, and directed at us */
		(void)le_hci_cmd(e, NG_HCI_OCF_LE_SET_SCAN_PARAMETERS, &sp,
		    sizeof(sp));
		scan_enable(e, true);
	}
	e->scan = want;
}

/* LE_Set_Scan_Enable answered. */
static void
scan_result(struct bt_engine *e, uint8_t status)
{
	uint8_t v = 0;

	if (e->scan_qn > 0) {
		v = e->scan_q[0];
		(void)memmove(e->scan_q, e->scan_q + 1, (size_t)--e->scan_qn);
	}
	if (status == 0 || v == 0)
		return;		/* disabling something already off is fine */
	le_log(e, "LE_Set_Scan_Enable refused: 0x%02x", status);
	e->scan = LE_SCAN_OFF;
	e->rescan_at = bt_now_ms() + RESCAN_MS;
}

/* ---- connecting -------------------------------------------------------------------- */

static void	conn_release(struct le_conn *c, bt_status_t st);

static struct le_conn *
conn_new(struct bt_engine *e)
{
	int i;

	for (i = 0; i < LE_CONN_MAX; i++) {
		struct le_conn *c = &e->conns[i];
		uint32_t gen;

		if (c->state != LC_FREE)
			continue;
		gen = c->gen + 1;
		(void)memset(c, 0, sizeof(*c));
		c->gen = gen & 0x00ffffff;
		c->e = e;
		c->handle = -1;
		c->bond = -1;
		c->smp_fd = -1;
		c->battery = -1;
		att_init(&c->att);
		smp_init(c);
		return (c);
	}
	return (NULL);
}

/* An L2CAP socket on fixed channel `cid` to the link's peer; -1 on error. */
static int
l2cap_open(struct le_conn *c, uint16_t cid)
{
	struct bt_engine *e = c->e;
	struct sockaddr_l2cap sa;
	int fd, err;

	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		return (-1);

	(void)memset(&sa, 0, sizeof(sa));
	sa.l2cap_len = sizeof(sa);
	sa.l2cap_family = AF_BLUETOOTH;
	hci_addr_to_wire(&e->local, (uint8_t *)&sa.l2cap_bdaddr);
	sa.l2cap_bdaddr_type = BDADDR_LE_PUBLIC;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		goto fail;

	(void)memset(&sa, 0, sizeof(sa));
	sa.l2cap_len = sizeof(sa);
	sa.l2cap_family = AF_BLUETOOTH;
	hci_addr_to_wire(&c->addr, (uint8_t *)&sa.l2cap_bdaddr);
	sa.l2cap_cid = cid;
	sa.l2cap_bdaddr_type = bt_addr_l2cap_type(&c->addr, BT_TRANSPORT_LE);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
	    errno != EINPROGRESS)
		goto fail;
	/* Writable once connected, or once it failed (SO_ERROR says). */
	le_watch(e, fd, c, true);
	return (fd);
fail:
	err = errno;
	(void)close(fd);
	errno = err;
	return (-1);
}

static struct le_conn *
le_connect(struct bt_engine *e, const struct bt_addr *a, int bond,
    bool pairing)
{
	struct le_conn *c;
	char s[BT_ADDR_STRLEN];

	c = conn_new(e);
	if (c == NULL)
		return (NULL);
	c->state = LC_CONNECTING;
	c->addr = *a;
	c->identity = bond >= 0 ? e->bonds[bond].identity : *a;
	c->bond = bond;
	c->pairing = pairing;
	c->connect_deadline = bt_now_ms() + LE_CONNECT_MS;

	/* No scanning while initiating. */
	scan_apply(e);

	c->att.fd = l2cap_open(c, NG_L2CAP_ATT_CID);
	if (c->att.fd < 0) {
		le_log(e, "cannot connect to %s: %s", ntoa(a, s),
		    strerror(errno));
		c->pairing = false;	/* the caller reports it */
		conn_release(c, bt_status_internal_error_e);
		return (NULL);
	}
	le_log(e, "connecting to %s%s", ntoa(a, s),
	    pairing ? " to pair" : bond >= 0 ? " (bonded)" : "");
	return (c);
}

/*
 * Take the link down from our side: LE_Create_Connection_Cancel while it is
 * still being made, HCI Disconnect once it exists.
 */
void
le_conn_drop(struct le_conn *c, const char *why)
{
	struct bt_engine *e = c->e;
	char s[BT_ADDR_STRLEN];

	if (c->state == LC_FREE)
		return;
	le_log(e, "dropping %s: %s", ntoa(&c->addr, s), why);
	if (c->state == LC_CONNECTING && c->handle < 0) {
		(void)le_hci_cmd(e, NG_HCI_OCF_LE_CREATE_CONNECTION_CANCEL,
		    NULL, 0);
	} else if (c->handle >= 0) {
		ng_hci_discon_cp cp;

		cp.con_handle = htole16((uint16_t)c->handle);
		cp.reason = HCI_ERR_REMOTE_USER_TERMINATED;
		(void)hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC,
		    NG_HCI_OCF_DISCON), &cp, sizeof(cp));
	}
	conn_release(c, bt_status_pairing_failed_e);
}

/* Forget the link: its sockets, its HID device, a pairing on it. */
static void
conn_release(struct le_conn *c, bt_status_t st)
{
	struct bt_engine *e = c->e;
	struct bt_addr addr = c->addr;
	struct bt_addr ident = c->identity;
	bool linked = c->state != LC_CONNECTING;
	bool worked = c->state == LC_READY;
	bool pairing = c->pairing;
	int bond = c->bond;
	uint32_t gen = c->gen;
	uint64_t retry;

	if (c->state == LC_FREE)
		return;
	c->pairing = false;
	hogp_stop(c);
	att_close(c);
	if (c->smp_fd >= 0) {
		le_unwatch(e, c->smp_fd);
		(void)close(c->smp_fd);
		c->smp_fd = -1;
	}
	smp_init(c);		/* frees and wipes the pairing's secrets */
	(void)memset(c, 0, sizeof(*c));
	c->state = LC_FREE;
	c->gen = gen;
	c->smp_fd = -1;
	c->att.fd = -1;

	/* A link that never worked is not retried at once; one that did is. */
	if (bond >= 0 && bond < e->nbonds && !worked) {
		retry = bt_now_ms() + LE_RETRY_MS;
		if (e->bonds[bond].retry_at < retry)
			e->bonds[bond].retry_at = retry;
	}
	if (pairing) {
		struct bt_event ev;

		(void)memset(&ev, 0, sizeof(ev));
		ev.kind = BT_EV_PAIR_FAILED;
		ev.peer = addr;
		ev.transport = BT_TRANSPORT_LE;
		ev.u.status = st;
		le_raise(e, &ev);
	}
	if (linked) {
		raise_simple(e, BT_EV_DISCONNECTED, &addr);
		/*
		 * BT_EV_CONNECTED named the identity address, which differs
		 * from the one on the air for a private address: the policy
		 * layer (connections.c) keys the link by the former.
		 */
		if (!bt_addr_equal(&ident, &addr))
			raise_simple(e, BT_EV_DISCONNECTED, &ident);
	}
	e->reconcile = true;
}

/*
 * A bonded device's link is up: encrypt it with the stored LTK before any GATT
 * traffic. This does not wait for the SMP channel, which only a pairing (or a
 * Security Request) needs.
 */
static void
secure_link(struct le_conn *c)
{
	struct bt_engine *e = c->e;
	struct bt_bond *b;

	b = calloc(1, sizeof(*b));
	if (b != NULL && e->host.bond_lookup != NULL &&
	    e->host.bond_lookup(e->host.ctx, &c->identity, b) &&
	    (b->flags & BT_BOND_LTK) != 0) {
		if (c->handle < 0)
			c->handle = handle_of(e, &c->addr);
		if (c->handle >= 0 && le_start_encryption(c, b->ltk.key,
		    b->ltk.ediv, b->ltk.rand) == 0) {
			c->state = LC_ENCRYPTING;
			explicit_bzero(b, sizeof(*b));
			free(b);
			return;
		}
		explicit_bzero(b, sizeof(*b));
		free(b);
		le_conn_drop(c, "cannot start encryption");
		return;
	}
	if (b != NULL) {
		explicit_bzero(b, sizeof(*b));
		free(b);
	}
	/* Bonded without an LTK: carry on unencrypted. */
	c->state = LC_READY;
	hogp_start(c);
}

/* The SMP channel is (or could not be) open: a pairing waits for it. */
static void
smp_channel_ready(struct le_conn *c, bool ok)
{
	bt_status_t st;

	if (!c->pairing || c->state == LC_PAIRING)
		return;
	if (!ok) {
		le_pair_done(c, bt_status_pairing_failed_e);
		return;
	}
	st = smp_start(c);
	if (st != bt_status_success_e) {
		le_pair_done(c, st);
		return;
	}
	c->state = LC_PAIRING;
}

/* A connect() on one of the link's sockets finished. */
static void
conn_writable(struct le_conn *c, int fd)
{
	struct bt_engine *e = c->e;
	socklen_t len = sizeof(int);
	char s[BT_ADDR_STRLEN];
	int err = 0;

	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
		err = errno;

	if (fd == c->att.fd && c->state == LC_CONNECTING) {
		if (err != 0) {
			le_log(e, "connecting to %s failed: %s",
			    ntoa(&c->addr, s), strerror(err));
			if (c->pairing)
				le_pair_done(c, bt_status_pairing_failed_e);
			else
				conn_release(c, bt_status_pairing_failed_e);
			return;
		}
		c->state = LC_LINKED;
		c->connect_deadline = 0;
		if (c->handle < 0)
			c->handle = handle_of(e, &c->addr);
		le_watch(e, c->att.fd, c, false);
		le_log(e, "connected to %s (handle %d)", ntoa(&c->addr, s),
		    c->handle);
		raise_simple(e, BT_EV_CONNECTED, &c->identity);
		e->reconcile = true;

		c->smp_fd = l2cap_open(c, NG_L2CAP_SMP_CID);
		if (c->smp_fd < 0) {
			le_log(e, "no SMP channel to %s: %s",
			    ntoa(&c->addr, s), strerror(errno));
			smp_channel_ready(c, false);
			if (c->state == LC_FREE)
				return;
		}
		if (!c->pairing)
			secure_link(c);
		return;
	}

	if (fd == c->smp_fd && !c->smp_connected) {
		if (err != 0) {
			le_log(e, "SMP channel to %s failed: %s",
			    ntoa(&c->addr, s), strerror(err));
			le_unwatch(e, c->smp_fd);
			(void)close(c->smp_fd);
			c->smp_fd = -1;
			smp_channel_ready(c, false);
			return;
		}
		c->smp_connected = true;
		le_watch(e, c->smp_fd, c, false);
		smp_channel_ready(c, true);
	}
}

/* ---- pairing ----------------------------------------------------------------------- */

void
le_pair_done(struct le_conn *c, bt_status_t st)
{
	struct bt_engine *e = c->e;
	struct bt_event ev;
	struct bt_bond *b;
	char s[BT_ADDR_STRLEN], id[BT_ADDR_STRLEN];
	int i;

	if (!c->pairing) {
		smp_init(c);
		return;
	}
	c->pairing = false;
	c->pair_deadline = 0;

	if (st != bt_status_success_e) {
		(void)memset(&ev, 0, sizeof(ev));
		ev.kind = BT_EV_PAIR_FAILED;
		ev.peer = c->addr;
		ev.transport = BT_TRANSPORT_LE;
		ev.u.status = st;
		smp_init(c);
		le_raise(e, &ev);
		le_conn_drop(c, "pairing failed");
		return;
	}

	b = calloc(1, sizeof(*b));
	if (b == NULL) {
		smp_init(c);
		le_conn_drop(c, "out of memory");
		return;
	}
	smp_bond(c, b);
	smp_init(c);
	b->autoconnect = true;
	b->trusted = true;
	b->paired_at = time(NULL);
	c->identity = b->addr;
	i = bond_put(e, &b->addr, (b->flags & BT_BOND_IRK) != 0, b->irk, true);
	c->bond = i;
	le_log(e, "paired with %s (identity %s)%s%s", ntoa(&c->addr, s),
	    ntoa(&b->addr, id), (b->flags & BT_BOND_LTK) ? ", LTK" : "",
	    (b->flags & BT_BOND_IRK) ? ", IRK" : "");

	/* Keys first, so the PairedDevices entry exists when PAIRED lands. */
	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_BOND_UPDATED;
	ev.peer = c->addr;
	ev.transport = BT_TRANSPORT_LE;
	ev.u.bond = b;
	le_raise(e, &ev);
	ev.kind = BT_EV_PAIRED;
	le_raise(e, &ev);
	explicit_bzero(b, sizeof(*b));
	free(b);

	c->encrypted = true;
	c->state = LC_READY;
	hogp_start(c);
}

void
le_security_request(struct le_conn *c, uint8_t auth)
{
	char s[BT_ADDR_STRLEN];

	(void)auth;
	if (c->pairing || c->encrypted || c->state != LC_READY)
		return;		/* already on its way */
	le_log(c->e, "%s asks for security but is not paired here; "
	    "pair it with bluetooth pair", ntoa(&c->addr, s));
}

/* ---- HCI events --------------------------------------------------------------------- */

/* Copy a name out of AD data. */
static void
ad_parse(struct bt_found *f, const uint8_t *ad, size_t len)
{
	size_t i = 0;

	while (i + 1 < len) {
		uint8_t flen = ad[i], type;
		const uint8_t *v = ad + i + 2;
		size_t n;

		if (flen == 0 || i + 1 + flen > len)
			break;
		type = ad[i + 1];
		n = flen - 1u;
		if ((type == AD_NAME_COMPLETE || (type == AD_NAME_SHORT &&
		    f->name[0] == '\0')) && n > 0) {
			if (n > BT_NAME_MAX)
				n = BT_NAME_MAX;
			(void)memcpy(f->name, v, n);
			f->name[n] = '\0';
		} else if (type == AD_APPEARANCE && n >= 2) {
			f->appearance = (uint16_t)(v[0] | v[1] << 8);
		}
		i += 1u + flen;
	}
}

static void
adv_report(struct bt_engine *e, uint8_t evt, uint8_t atype,
    const uint8_t *bdaddr, const uint8_t *ad, size_t adlen, int8_t rssi)
{
	struct bt_addr a;
	int i;

	(void)memcpy(a.b, bdaddr, BT_ADDR_LEN);
	/* 2 and 3 are identities the controller resolved; we do not ask it. */
	a.type = (atype & 1) ? BT_ADDR_RANDOM : BT_ADDR_PUBLIC;

	if (e->user_scan) {
		struct bt_event ev;
		struct bt_found *f = &ev.u.found;

		(void)memset(&ev, 0, sizeof(ev));
		ev.kind = BT_EV_FOUND;
		ev.transport = BT_TRANSPORT_LE;
		ev.peer = a;
		f->addr = a;
		f->transports = BT_TRANSPORT_LE;
		f->rssi = rssi;
		f->last_seen = time(NULL);
		if (adlen > sizeof(f->eir))
			adlen = sizeof(f->eir);
		ad_parse(f, ad, adlen);
		if (evt != ADV_SCAN_RSP || adlen > 0) {
			(void)memcpy(f->eir, ad, adlen);
			f->eir_len = (uint16_t)adlen;
		}
		le_raise(e, &ev);
	}

	/* A bonded device advertising to be reconnected. */
	if (evt != ADV_IND && evt != ADV_DIRECT_IND)
		return;
	i = bond_for(e, &a);
	if (i < 0 || !e->bonds[i].autoconnect || bond_connected(e, i) ||
	    connecting(e) || (e->bonds[i].retry_at != 0 &&
	    bt_now_ms() < e->bonds[i].retry_at))
		return;
	(void)le_connect(e, &a, i, false);
}

static void
le_meta(struct bt_engine *e, const uint8_t *p, size_t len)
{
	struct le_conn *c;
	char s[BT_ADDR_STRLEN];

	if (len < 1)
		return;
	switch (p[0]) {
	case NG_HCI_LEEV_ADVREP: {
		size_t off = 2;
		int n, k;

		if (len < 2)
			return;
		n = p[1];
		/* One report after another: type, addr type, addr, len, data, rssi. */
		for (k = 0; k < n && off + 9 <= len; k++) {
			uint8_t dl = p[off + 8];

			if (off + 9 + dl + 1 > len)
				break;
			adv_report(e, p[off], p[off + 1], p + off + 2,
			    p + off + 9, dl, (int8_t)p[off + 9 + dl]);
			off += 9u + dl + 1u;
		}
		return;
	}
	case NG_HCI_LEEV_CON_COMPL:
	case NG_HCI_LEEV_ENH_CONN_COMPL: {
		/* Same leading layout in both: status, handle, role, type, addr. */
		uint8_t status;
		uint16_t h;
		struct bt_addr a;
		int i;

		if (len < 1 + 11)
			return;
		status = p[1];
		h = NG_HCI_CON_HANDLE(le16dec(p + 2));
		(void)memcpy(a.b, p + 6, BT_ADDR_LEN);
		for (i = 0; i < LE_CONN_MAX; i++) {
			c = &e->conns[i];
			if (c->state != LC_CONNECTING ||
			    memcmp(c->addr.b, a.b, BT_ADDR_LEN) != 0)
				continue;
			if (status == 0)
				c->handle = h;
			else
				le_log(e, "LE connection to %s failed: "
				    "0x%02x", ntoa(&c->addr, s), status);
		}
		return;
	}
	case NG_HCI_LEEV_LONG_TERM_KEY_REQUEST: {
		const ng_hci_le_long_term_key_request_ep *ep =
		    (const void *)(p + 1);
		struct bt_bond *b;
		uint16_t h, ediv;
		uint64_t rand;
		bool ok = false;

		if (len < 1 + sizeof(*ep))
			return;
		h = NG_HCI_CON_HANDLE(le16toh(ep->connection_handle));
		ediv = le16toh(ep->encrypted_diversifier);
		rand = le64toh(ep->random_number);
		c = conn_by_handle(e, h);
		b = calloc(1, sizeof(*b));
		if (c != NULL && b != NULL && e->host.bond_lookup != NULL &&
		    e->host.bond_lookup(e->host.ctx, &c->identity, b) &&
		    (b->flags & BT_BOND_LTK) != 0 && b->ltk.ediv == ediv &&
		    b->ltk.rand == rand) {
			ng_hci_le_long_term_key_request_reply_cp cp;

			cp.connection_handle = htole16(h);
			(void)memcpy(cp.long_term_key, b->ltk.key, 16);
			ok = le_hci_cmd(e,
			    NG_HCI_OCF_LE_LONG_TERM_KEY_REQUEST_REPLY, &cp,
			    sizeof(cp)) == 0;
			explicit_bzero(&cp, sizeof(cp));
		}
		if (b != NULL) {
			explicit_bzero(b, sizeof(*b));
			free(b);
		}
		if (!ok) {
			ng_hci_le_long_term_key_request_negative_reply_cp cp;

			cp.connection_handle = htole16(h);
			(void)le_hci_cmd(e,
			    NG_HCI_OCF_LE_LONG_TERM_KEY_REQUEST_NEGATIVE_REPLY,
			    &cp, sizeof(cp));
		}
		return;
	}
	default:
		/* 0x03 and 0x06: the kernel's (nextbsd-kernel-extensions#30). */
		return;
	}
}

/* The link's encryption changed. */
static void
encryption_changed(struct le_conn *c, uint8_t status, bool on)
{
	struct bt_engine *e = c->e;
	char s[BT_ADDR_STRLEN];
	bool ok = status == 0 && on;

	if (c->smp.stage == SMP_W4_ENCRYPTION) {
		c->encrypted = ok;
		smp_encrypted(c, ok ? 0 : (status != 0 ? status : 0xff));
		return;
	}
	if (c->state != LC_ENCRYPTING) {
		c->encrypted = ok;
		return;
	}
	if (!ok) {
		le_log(e, "encrypting the link to %s failed (0x%02x)%s",
		    ntoa(&c->addr, s), status, status == 0x06 ?
		    ": the device no longer has this computer's key; pair it "
		    "again" : "");
		if (c->bond >= 0)
			e->bonds[c->bond].retry_at = bt_now_ms() +
			    (status == 0x06 ? KEY_MISSING_RETRY_MS :
			    LE_RETRY_MS);
		le_conn_drop(c, "encryption failed");
		return;
	}
	c->encrypted = true;
	c->state = LC_READY;
	le_log(e, "link to %s encrypted", ntoa(&c->addr, s));
	hogp_start(c);
}

static void
le_hci_event(struct bt_engine *e, uint8_t code, const uint8_t *p, size_t len)
{
	struct hci_event ev;
	struct le_conn *c;
	uint16_t opcode;
	uint8_t status;
	const uint8_t *rp;
	size_t rplen;
	int i;

	ev.code = code;
	ev.len = (uint8_t)len;
	ev.params = p;
	if (hci_cmd_result(&ev, &opcode, &status, &rp, &rplen)) {
		if (opcode == HCI_OPCODE(OGF_LE,
		    NG_HCI_OCF_LE_SET_SCAN_ENABLE)) {
			scan_result(e, status);
		} else if (opcode == HCI_OPCODE(OGF_LE,
		    NG_HCI_OCF_LE_START_ENCRYPTION) && status != 0) {
			/* Refused outright: no Encryption Change follows. */
			for (i = 0; i < LE_CONN_MAX; i++) {
				c = &e->conns[i];
				if (c->state == LC_ENCRYPTING ||
				    c->smp.stage == SMP_W4_ENCRYPTION) {
					encryption_changed(c, status, false);
					break;
				}
			}
		} else if (status != 0 && (opcode >> 10) == OGF_LE &&
		    opcode != HCI_OPCODE(OGF_LE,
		    NG_HCI_OCF_LE_CREATE_CONNECTION_CANCEL))
			le_log(e, "LE command 0x%04x: status 0x%02x", opcode,
			    status);
		return;
	}

	switch (code) {
	case NG_HCI_EVENT_LE:
		le_meta(e, p, len);
		break;
	case NG_HCI_EVENT_ENCRYPTION_CHANGE: {
		const ng_hci_encryption_change_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return;
		c = conn_by_handle(e,
		    NG_HCI_CON_HANDLE(le16toh(ep->con_handle)));
		if (c != NULL)
			encryption_changed(c, ep->status,
			    ep->encryption_enable != 0);
		break;
	}
	case HCI_EVENT_ENCRYPTION_KEY_REFRESH:
		if (len < 3)
			return;
		c = conn_by_handle(e, NG_HCI_CON_HANDLE(le16dec(p + 1)));
		if (c != NULL)
			encryption_changed(c, p[0], true);
		break;
	case NG_HCI_EVENT_DISCON_COMPL: {
		const ng_hci_discon_compl_ep *ep = (const void *)p;
		char s[BT_ADDR_STRLEN];

		if (len < sizeof(*ep) || ep->status != 0)
			return;
		c = conn_by_handle(e,
		    NG_HCI_CON_HANDLE(le16toh(ep->con_handle)));
		if (c == NULL)
			return;
		le_log(e, "%s disconnected (reason 0x%02x)",
		    ntoa(&c->addr, s), ep->reason);
		conn_release(c, bt_status_pairing_failed_e);
		break;
	}
	default:
		break;
	}
}

/* ---- engine ops ---------------------------------------------------------------------- */

static bt_status_t
le_attach(const struct bt_controller_desc *d,
    const struct bt_engine_host *host, struct bt_engine **out)
{
	struct ng_btsocket_hci_raw_node_bdaddr r;
	struct bt_engine *e;
	int i;

	*out = NULL;
	if (d->hci_fd < 0)
		return (bt_status_no_adapter_e);
	(void)memset(&r, 0, sizeof(r));
	if (ioctl(d->hci_fd, SIOC_HCI_RAW_NODE_GET_BDADDR, &r) < 0)
		return (bt_status_no_adapter_e);
	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return (bt_status_internal_error_e);
	e->kq = kqueue();
	if (e->kq < 0) {
		free(e);
		return (bt_status_internal_error_e);
	}
	e->host = *host;
	(void)strlcpy(e->node, d->node, sizeof(e->node));
	(void)strlcpy(e->hci_node, d->hci_node, sizeof(e->hci_node));
	e->fd = d->hci_fd;
	hci_addr_from_wire((const uint8_t *)&r.bdaddr, &e->local);
	e->powered = d->powered;
	e->scan = LE_SCAN_OFF;
	e->reconcile = true;
	for (i = 0; i < LE_CONN_MAX; i++) {
		e->conns[i].att.fd = -1;
		e->conns[i].smp_fd = -1;
	}
	*out = e;
	return (bt_status_success_e);
}

static void
le_detach(struct bt_engine *e)
{
	int i;

	if (e == NULL)
		return;
	for (i = 0; i < LE_CONN_MAX; i++)
		conn_release(&e->conns[i], bt_status_no_adapter_e);
	if (e->kq >= 0)
		(void)close(e->kq);
	explicit_bzero(e->bonds, sizeof(e->bonds));
	free(e);
}

static int
le_event_fd(struct bt_engine *e)
{
	return (e->kq);
}

static bool
le_pump(struct bt_engine *e)
{
	struct kevent ev[16];
	struct timespec zero = { 0, 0 };
	int i, n;

	n = kevent(e->kq, NULL, 0, ev, 16, &zero);
	for (i = 0; i < n; i++) {
		struct le_conn *c = conn_from_cookie(e, ev[i].udata);
		int fd = (int)ev[i].ident;

		if (c == NULL)
			continue;
		if (ev[i].filter == EVFILT_WRITE)
			conn_writable(c, fd);
		else if (fd == c->att.fd)
			att_input(c);
		else if (fd == c->smp_fd)
			smp_input(c);
	}
	return (n > 0);
}

static bt_status_t
le_set_power(struct bt_engine *e, bool on)
{
	int i;

	if (on) {
		e->powered = true;
		e->reconcile = true;
		return (bt_status_success_e);
	}
	if (e->user_scan) {
		e->user_scan = false;
		raise_discovery(e, false);
	}
	for (i = 0; i < LE_CONN_MAX; i++) {
		struct le_conn *c = &e->conns[i];

		if (c->state == LC_FREE)
			continue;
		if (c->pairing && c->smp.stage != SMP_IDLE)
			smp_abort(c, bt_status_powered_off_e);
		if (c->state != LC_FREE)
			le_conn_drop(c, "powering off");
	}
	e->powered = false;
	if (e->scan != LE_SCAN_OFF)
		scan_enable(e, false);
	e->scan = LE_SCAN_OFF;
	return (bt_status_success_e);
}

static bt_status_t
le_discover(struct bt_engine *e, bool start, int seconds)
{
	if (!start) {
		if (e->user_scan) {
			e->user_scan = false;
			raise_discovery(e, false);
		}
		scan_apply(e);
		return (bt_status_success_e);
	}
	if (!e->powered)
		return (bt_status_powered_off_e);
	if (seconds <= 0)
		seconds = 10;
	e->user_scan_until = bt_now_ms() + (uint64_t)seconds * 1000;
	if (!e->user_scan) {
		e->user_scan = true;
		le_log(e, "scanning for %d s", seconds);
		raise_discovery(e, true);
	}
	scan_apply(e);
	return (bt_status_success_e);
}

static bt_status_t
le_pair(struct bt_engine *e, const struct bt_addr *peer, bt_io_cap_t io,
    const char *pin)
{
	struct le_conn *c;
	bt_status_t st;
	int i;

	(void)io;	/* SMP always offers KeyboardDisplay */
	(void)pin;	/* LE has no PIN */
	if (!e->powered)
		return (bt_status_powered_off_e);
	for (i = 0; i < LE_CONN_MAX; i++) {
		if (e->conns[i].state != LC_FREE && e->conns[i].pairing)
			return (bt_status_busy_e);
	}
	if (e->user_scan) {
		e->user_scan = false;
		raise_discovery(e, false);
	}

	c = conn_by_addr(e, peer);
	if (c != NULL) {
		/* Pair again over the link we already have. */
		if (c->state == LC_CONNECTING || c->state == LC_ENCRYPTING ||
		    c->state == LC_PAIRING)
			return (bt_status_busy_e);
		c->pairing = true;
		c->pair_deadline = bt_now_ms() + LE_PAIR_TIMEOUT_MS;
		if (!c->smp_connected) {
			c->pairing = false;
			return (bt_status_not_connected_e);
		}
		hogp_stop(c);
		st = smp_start(c);
		if (st != bt_status_success_e) {
			c->pairing = false;
			return (st);
		}
		c->state = LC_PAIRING;
		return (bt_status_success_e);
	}

	c = le_connect(e, peer, bond_for(e, peer), true);
	if (c == NULL)
		return (bt_status_busy_e);
	c->pair_created = true;
	c->pair_deadline = bt_now_ms() + LE_PAIR_TIMEOUT_MS;
	return (bt_status_success_e);
}

static bt_status_t
le_pair_reply(struct bt_engine *e, const struct bt_pair_answer *a)
{
	int i;

	for (i = 0; i < LE_CONN_MAX; i++) {
		struct le_conn *c = &e->conns[i];

		if (c->state != LC_FREE && c->smp.waiting != bt_pair_none_e &&
		    c->smp.prompt_id == a->id)
			return (smp_reply(c, a));
	}
	return (bt_status_no_prompt_e);
}

static bt_status_t
le_unpair(struct bt_engine *e, const struct bt_addr *peer)
{
	int i, b = bond_for(e, peer);

	for (i = 0; i < LE_CONN_MAX; i++) {
		struct le_conn *c = &e->conns[i];

		if (c->state == LC_FREE)
			continue;
		if ((b >= 0 && c->bond == b) || bt_addr_equal(&c->addr, peer)) {
			if (c->pairing && c->smp.stage != SMP_IDLE)
				smp_abort(c, bt_status_pairing_rejected_e);
			if (c->state != LC_FREE)
				le_conn_drop(c, "unpaired");
		}
	}
	if (b < 0)
		return (bt_status_success_e);
	explicit_bzero(&e->bonds[b], sizeof(e->bonds[b]));
	for (i = b; i + 1 < e->nbonds; i++)
		e->bonds[i] = e->bonds[i + 1];
	e->nbonds--;
	for (i = 0; i < LE_CONN_MAX; i++) {
		if (e->conns[i].state != LC_FREE && e->conns[i].bond > b)
			e->conns[i].bond--;
	}
	e->reconcile = true;
	return (bt_status_success_e);
}

static bt_status_t
le_connect_op(struct bt_engine *e, const struct bt_addr *peer)
{
	int b;

	if (!e->powered)
		return (bt_status_powered_off_e);
	if (conn_by_addr(e, peer) != NULL)
		return (bt_status_success_e);
	b = bond_for(e, peer);
	if (b < 0)
		return (bt_status_not_paired_e);
	if (connecting(e))
		return (bt_status_busy_e);
	return (le_connect(e, peer, b, false) != NULL ? bt_status_success_e :
	    bt_status_busy_e);
}

static bt_status_t
le_disconnect(struct bt_engine *e, const struct bt_addr *peer)
{
	struct le_conn *c = conn_by_addr(e, peer);

	if (c == NULL)
		return (bt_status_not_connected_e);
	if (c->pairing && c->smp.stage != SMP_IDLE)
		smp_abort(c, bt_status_pairing_rejected_e);
	if (c->state != LC_FREE)
		le_conn_drop(c, "disconnect requested");
	return (bt_status_success_e);
}

static bt_status_t
le_bond_import(struct bt_engine *e, const struct bt_bond *b)
{
	if ((b->transports & BT_TRANSPORT_LE) == 0 &&
	    (b->flags & (BT_BOND_LTK | BT_BOND_IRK)) == 0)
		return (bt_status_success_e);
	if (bond_put(e, &b->addr, (b->flags & BT_BOND_IRK) != 0, b->irk,
	    b->autoconnect) < 0)
		return (bt_status_internal_error_e);
	return (bt_status_success_e);
}

static uint64_t
earlier(uint64_t t, uint64_t u)
{
	if (u == 0)
		return (t);
	if (t == 0 || u < t)
		return (u);
	return (t);
}

static uint64_t
le_next_timer(struct bt_engine *e)
{
	uint64_t t = 0;
	int i;

	if (e->reconcile)
		return (bt_now_ms());
	if (e->user_scan)
		t = earlier(t, e->user_scan_until);
	t = earlier(t, e->rescan_at);
	for (i = 0; i < e->nbonds; i++) {
		if (e->bonds[i].autoconnect && e->bonds[i].retry_at != 0)
			t = earlier(t, e->bonds[i].retry_at);
	}
	for (i = 0; i < LE_CONN_MAX; i++) {
		struct le_conn *c = &e->conns[i];

		if (c->state == LC_FREE)
			continue;
		if (c->state == LC_CONNECTING)
			t = earlier(t, c->connect_deadline);
		if (c->pairing)
			t = earlier(t, c->pair_deadline);
		t = earlier(t, att_next_timer(c));
		t = earlier(t, smp_next_timer(c));
	}
	return (t);
}

static void
le_timer(struct bt_engine *e, uint64_t now)
{
	int i;

	if (e->user_scan && now >= e->user_scan_until) {
		e->user_scan = false;
		raise_discovery(e, false);
		e->reconcile = true;
	}
	if (e->rescan_at != 0 && now >= e->rescan_at) {
		e->rescan_at = 0;
		e->reconcile = true;
	}
	/*
	 * A bond's back-off ran out. The controller filters duplicate reports
	 * until scanning restarts, so restart it: the device's next
	 * advertisement is reported again.
	 */
	for (i = 0; i < e->nbonds; i++) {
		if (e->bonds[i].retry_at == 0 || now < e->bonds[i].retry_at)
			continue;
		e->bonds[i].retry_at = 0;
		if (e->scan != LE_SCAN_OFF && !bond_connected(e, i)) {
			scan_enable(e, false);
			e->scan = LE_SCAN_OFF;
			e->reconcile = true;
		}
	}
	for (i = 0; i < LE_CONN_MAX; i++) {
		struct le_conn *c = &e->conns[i];

		if (c->state == LC_CONNECTING && c->connect_deadline != 0 &&
		    now >= c->connect_deadline) {
			if (c->pairing) {
				c->pairing = false;
				le_raise(e, &(struct bt_event){
				    .kind = BT_EV_PAIR_FAILED, .peer = c->addr,
				    .transport = BT_TRANSPORT_LE,
				    .u.status = bt_status_pairing_timeout_e });
			}
			le_conn_drop(c, "the device did not answer");
			continue;
		}
		if (c->state != LC_FREE && c->pairing &&
		    c->pair_deadline != 0 && now >= c->pair_deadline) {
			if (c->smp.stage != SMP_IDLE)
				smp_abort(c, bt_status_pairing_timeout_e);
			else
				le_pair_done(c, bt_status_pairing_timeout_e);
			continue;
		}
		if (c->state != LC_FREE)
			att_timer(c, now);
		if (c->state != LC_FREE)
			smp_timer(c, now);
	}
	if (e->reconcile) {
		e->reconcile = false;
		scan_apply(e);
	}
}

const struct bt_engine_ops bt_le_engine = {
	.name		= "le",
	.transport	= BT_TRANSPORT_LE,
	.attach		= le_attach,
	.detach		= le_detach,
	.hci_event	= le_hci_event,
	.event_fd	= le_event_fd,
	.pump		= le_pump,
	.set_power	= le_set_power,
	.discover	= le_discover,
	.pair		= le_pair,
	.pair_reply	= le_pair_reply,
	.unpair		= le_unpair,
	.connect	= le_connect_op,
	.disconnect	= le_disconnect,
	.bond_import	= le_bond_import,
	.next_timer	= le_next_timer,
	.timer		= le_timer,
};
