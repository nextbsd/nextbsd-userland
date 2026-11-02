/*
 * links.c — the Classic engine's ACL links. See links.h.
 */
#include "links.h"
#include "hci.h"
#include "hci_compat.h"

#include <string.h>

#define	OGF_LC		NG_HCI_OGF_LINK_CONTROL
#define	OGF_STATUS	NG_HCI_OGF_STATUS

/* Create_Connection_Cancel: only the address. */
#define	HCI_OCF_CREATE_CON_CANCEL	0x0008

/* "Unknown HCI command": the controller has no Read_RSSI. */
#define	HCI_ERR_UNKNOWN_COMMAND		0x01

static const char *
ntoa(const struct bt_addr *a, char *buf)
{
	return (bt_addr_ntoa(a, BT_ADDR_FMT_DISPLAY, buf, BT_ADDR_STRLEN));
}

void
links_init(struct bt_engine *e)
{
	int i;

	(void)memset(e->links, 0, sizeof(e->links));
	for (i = 0; i < CLASSIC_LINK_MAX; i++)
		e->links[i].handle = -1;
	e->rssi_next = 0;
	e->rssi_unsupported = false;
}

static struct classic_link *
by_peer(struct bt_engine *e, const struct bt_addr *peer)
{
	int i;

	for (i = 0; i < CLASSIC_LINK_MAX; i++) {
		if (e->links[i].used &&
		    memcmp(e->links[i].peer.b, peer->b, BT_ADDR_LEN) == 0)
			return (&e->links[i]);
	}
	return (NULL);
}

static struct classic_link *
by_handle(struct bt_engine *e, int handle)
{
	int i;

	for (i = 0; i < CLASSIC_LINK_MAX; i++) {
		if (e->links[i].used && e->links[i].handle == handle)
			return (&e->links[i]);
	}
	return (NULL);
}

static struct classic_link *
add(struct bt_engine *e, const struct bt_addr *peer)
{
	struct classic_link *l = by_peer(e, peer);
	int i;

	for (i = 0; l == NULL && i < CLASSIC_LINK_MAX; i++) {
		if (!e->links[i].used)
			l = &e->links[i];
	}
	if (l == NULL)
		return (NULL);	/* more than the controller can hold */
	if (!l->used) {
		(void)memset(l, 0, sizeof(*l));
		l->used = true;
		l->peer = *peer;
		l->peer.type = BT_ADDR_PUBLIC;
		l->handle = -1;
		l->rssi = 127;
	}
	return (l);
}

static void
drop(struct classic_link *l)
{
	(void)memset(l, 0, sizeof(*l));
	l->handle = -1;
}

static void
raise_link(struct bt_engine *e, bt_event_kind_t kind,
    const struct classic_link *l, uint8_t reason)
{
	struct bt_event ev;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.peer = l->peer;
	ev.transport = BT_TRANSPORT_BREDR;
	ev.u.link.handle = l->handle;
	ev.u.link.incoming = l->incoming;
	ev.u.link.encrypted = l->encrypted;
	ev.u.link.central = l->central;
	ev.u.link.rssi = l->rssi;
	ev.u.link.reason = reason;
	ev.u.link.class_of_device = l->class_of_device;
	classic_raise(e, &ev);
}

/*
 * A new link to a bonded device: authenticate it with the stored key, then
 * encrypt it (on Authentication_Complete). HID devices and headsets expect
 * the host to do this; a peer that has already encrypted the link is left
 * alone, and the pairing in progress secures its own link.
 */
static void
secure(struct bt_engine *e, struct classic_link *l)
{
	struct bt_bond b;
	ng_hci_auth_req_cp cp;
	char a[BT_ADDR_STRLEN];
	bool bonded;

	if (l->encrypted || l->securing || l->closing || l->handle < 0 ||
	    classic_pairing_with(e, &l->peer))
		return;
	bonded = e->host.bond_lookup != NULL &&
	    e->host.bond_lookup(e->host.ctx, &l->peer, &b) &&
	    (b.flags & BT_BOND_LINK_KEY) != 0;
	(void)memset(b.link_key, 0, sizeof(b.link_key));
	if (!bonded)
		return;
	cp.con_handle = htole16((uint16_t)l->handle);
	if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC, NG_HCI_OCF_AUTH_REQ), &cp,
	    sizeof(cp)) < 0) {
		classic_log(e, "Authentication_Requested for %s: send failed",
		    ntoa(&l->peer, a));
		return;
	}
	l->securing = true;
}

static void
encrypt(struct bt_engine *e, struct classic_link *l)
{
	ng_hci_set_con_encryption_cp cp;

	cp.con_handle = htole16((uint16_t)l->handle);
	cp.encryption_enable = 1;
	if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC,
	    NG_HCI_OCF_SET_CON_ENCRYPTION), &cp, sizeof(cp)) < 0)
		l->securing = false;
}

bool
links_hci_event(struct bt_engine *e, uint8_t code, const uint8_t *p,
    size_t len)
{
	struct classic_link *l;
	struct bt_addr peer;
	char a[BT_ADDR_STRLEN];

	switch (code) {
	case NG_HCI_EVENT_CON_REQ: {
		const ng_hci_con_req_ep *ep = (const void *)p;

		if (len < sizeof(*ep) || ep->link_type != NG_HCI_LINK_ACL)
			return (false);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		l = add(e, &peer);
		if (l != NULL && l->handle < 0) {
			l->incoming = true;
			l->class_of_device = (uint32_t)ep->uclass[0] |
			    (uint32_t)ep->uclass[1] << 8 |
			    (uint32_t)ep->uclass[2] << 16;
		}
		return (true);
	}

	case NG_HCI_EVENT_CON_COMPL: {
		const ng_hci_con_compl_ep *ep = (const void *)p;

		if (len < sizeof(*ep) || ep->link_type != NG_HCI_LINK_ACL)
			return (false);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		l = add(e, &peer);
		if (l == NULL)
			return (false);
		if (ep->status != 0) {
			if (l->requested || l->incoming)
				classic_log(e, "connection %s %s failed: 0x%02x",
				    l->incoming ? "from" : "to",
				    ntoa(&peer, a), ep->status);
			l->handle = -1;
			raise_link(e, BT_EV_DISCONNECTED, l, ep->status);
			drop(l);
			return (false);
		}
		l->handle = NG_HCI_CON_HANDLE(le16toh(ep->con_handle));
		l->central = !l->incoming;
		l->encrypted = (ep->encryption_mode != 0);
		classic_log(e, "connected %s %s (handle %d)",
		    l->incoming ? "from" : "to", ntoa(&peer, a), l->handle);
		raise_link(e, BT_EV_CONNECTED, l, 0);
		/* The policy may have dropped it; secure what is left. */
		if (l->used && l->handle >= 0)
			secure(e, l);
		if (e->rssi_next == 0)
			e->rssi_next = bt_now_ms() + LINKS_RSSI_MS;
		return (false);
	}

	case NG_HCI_EVENT_DISCON_COMPL: {
		const ng_hci_discon_compl_ep *ep = (const void *)p;

		if (len < sizeof(*ep) || ep->status != 0)
			return (false);
		l = by_handle(e, NG_HCI_CON_HANDLE(le16toh(ep->con_handle)));
		if (l == NULL)
			return (false);		/* SCO, or not ours */
		classic_log(e, "disconnected %s (reason 0x%02x)",
		    ntoa(&l->peer, a), ep->reason);
		raise_link(e, BT_EV_DISCONNECTED, l, ep->reason);
		drop(l);
		return (false);
	}

	case NG_HCI_EVENT_AUTH_COMPL: {
		const ng_hci_auth_compl_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (true);
		l = by_handle(e, NG_HCI_CON_HANDLE(le16toh(ep->con_handle)));
		if (l == NULL)
			return (true);
		if (ep->status != 0) {
			classic_log(e, "authentication of %s failed: 0x%02x",
			    ntoa(&l->peer, a), ep->status);
			l->securing = false;
			return (true);
		}
		if (l->securing && !l->encrypted)
			encrypt(e, l);
		return (true);
	}

	case NG_HCI_EVENT_ENCRYPTION_CHANGE: {
		const ng_hci_encryption_change_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (true);
		l = by_handle(e, NG_HCI_CON_HANDLE(le16toh(ep->con_handle)));
		if (ep->status != 0) {
			classic_log(e, "encryption on handle %u failed: 0x%02x",
			    NG_HCI_CON_HANDLE(le16toh(ep->con_handle)),
			    ep->status);
			if (l != NULL)
				l->securing = false;
			return (true);
		}
		if (l == NULL)
			return (true);
		l->securing = false;
		if (l->encrypted != (ep->encryption_enable != 0)) {
			l->encrypted = (ep->encryption_enable != 0);
			raise_link(e, BT_EV_LINK_CHANGED, l, 0);
		}
		return (true);
	}

	case NG_HCI_EVENT_ROLE_CHANGE: {
		const ng_hci_role_change_ep *ep = (const void *)p;

		if (len < sizeof(*ep) || ep->status != 0)
			return (true);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		l = by_peer(e, &peer);
		if (l != NULL && l->handle >= 0) {
			l->central = (ep->role == NG_HCI_ROLE_MASTER);
			raise_link(e, BT_EV_LINK_CHANGED, l, 0);
		}
		return (true);
	}

	default:
		return (false);
	}
}

void
links_cmd_result(struct bt_engine *e, uint16_t opcode, uint8_t status,
    const uint8_t *rp, size_t rplen)
{
	struct classic_link *l;
	int i;

	if (opcode == HCI_OPCODE(OGF_STATUS, NG_HCI_OCF_READ_RSSI)) {
		if (status == HCI_ERR_UNKNOWN_COMMAND) {
			if (!e->rssi_unsupported)
				classic_log(e, "no Read_RSSI on this "
				    "controller; not asking again");
			e->rssi_unsupported = true;
			return;
		}
		if (status != 0 || rplen < 3)
			return;
		l = by_handle(e, NG_HCI_CON_HANDLE(rp[0] | rp[1] << 8));
		if (l != NULL && l->rssi != (int8_t)rp[2]) {
			l->rssi = (int8_t)rp[2];
			raise_link(e, BT_EV_LINK_CHANGED, l, 0);
		}
		return;
	}
	if (status == 0)
		return;
	if (opcode == HCI_OPCODE(OGF_LC, NG_HCI_OCF_CREATE_CON)) {
		/*
		 * Refused outright (no Connection_Complete follows): the
		 * oldest link we asked for that has not come up.
		 */
		for (i = 0; i < CLASSIC_LINK_MAX; i++) {
			l = &e->links[i];
			if (!l->used || !l->requested || l->handle >= 0 ||
			    l->incoming)
				continue;
			classic_log(e, "Create_Connection refused: 0x%02x",
			    status);
			raise_link(e, BT_EV_DISCONNECTED, l, status);
			drop(l);
			break;
		}
	} else if (opcode == HCI_OPCODE(OGF_LC, NG_HCI_OCF_AUTH_REQ) ||
	    opcode == HCI_OPCODE(OGF_LC, NG_HCI_OCF_SET_CON_ENCRYPTION)) {
		/* Which link is not said; none of them is securing now. */
		for (i = 0; i < CLASSIC_LINK_MAX; i++)
			e->links[i].securing = false;
	}
}

bt_status_t
links_connect(struct bt_engine *e, const struct bt_addr *peer)
{
	ng_hci_create_con_cp cp;
	struct classic_link *l;
	char a[BT_ADDR_STRLEN];
	int i;

	if (!e->powered)
		return (bt_status_powered_off_e);
	if (by_peer(e, peer) != NULL || classic_pairing_with(e, peer))
		return (bt_status_success_e);	/* up, or on its way */
	if (hci_acl_handle(e->fd, peer) >= 0)
		return (bt_status_success_e);
	l = add(e, peer);
	if (l == NULL)
		return (bt_status_busy_e);

	(void)memset(&cp, 0, sizeof(cp));
	hci_addr_to_wire(peer, (uint8_t *)&cp.bdaddr);
	cp.pkt_type = htole16(NG_HCI_PKT_DM1 | NG_HCI_PKT_DH1 |
	    NG_HCI_PKT_DM3 | NG_HCI_PKT_DH3 | NG_HCI_PKT_DM5 | NG_HCI_PKT_DH5);
	cp.page_scan_rep_mode = 0x02;	/* R2 unless discovery knew */
	cp.accept_role_switch = 1;
	for (i = 0; i < e->ninq; i++) {
		if (memcmp(e->inq[i].addr.b, peer->b, BT_ADDR_LEN) == 0) {
			cp.page_scan_rep_mode = e->inq[i].psrm;
			cp.clock_offset = htole16(e->inq[i].clock_offset |
			    0x8000);
			break;
		}
	}
	if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC, NG_HCI_OCF_CREATE_CON),
	    &cp, sizeof(cp)) < 0) {
		drop(l);
		return (bt_status_internal_error_e);
	}
	l->requested = true;
	classic_log(e, "connecting to %s", ntoa(peer, a));
	return (bt_status_success_e);
}

bt_status_t
links_disconnect(struct bt_engine *e, const struct bt_addr *peer)
{
	struct classic_link *l = by_peer(e, peer);
	ng_hci_discon_cp cp;
	int h;

	if (l != NULL && l->handle < 0) {
		uint8_t wire[BT_ADDR_LEN];

		if (!l->requested)
			return (bt_status_busy_e);	/* coming up from them */
		hci_addr_to_wire(peer, wire);
		(void)hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC,
		    HCI_OCF_CREATE_CON_CANCEL), wire, sizeof(wire));
		return (bt_status_success_e);
	}
	h = (l != NULL) ? l->handle : hci_acl_handle(e->fd, peer);
	if (h < 0)
		return (bt_status_not_connected_e);
	cp.con_handle = htole16((uint16_t)h);
	cp.reason = HCI_ERR_REMOTE_USER_TERMINATED;
	if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC, NG_HCI_OCF_DISCON), &cp,
	    sizeof(cp)) < 0)
		return (bt_status_internal_error_e);
	if (l != NULL)
		l->closing = true;
	return (bt_status_success_e);
}

uint64_t
links_next_timer(struct bt_engine *e)
{
	int i;

	if (e->rssi_unsupported || e->rssi_next == 0)
		return (0);
	for (i = 0; i < CLASSIC_LINK_MAX; i++) {
		if (e->links[i].used && e->links[i].handle >= 0)
			return (e->rssi_next);
	}
	e->rssi_next = 0;	/* no link: no round until the next one */
	return (0);
}

void
links_timer(struct bt_engine *e, uint64_t now)
{
	ng_hci_read_rssi_cp cp;
	int i;

	if (e->rssi_unsupported || e->rssi_next == 0 || now < e->rssi_next)
		return;
	e->rssi_next = now + LINKS_RSSI_MS;
	for (i = 0; i < CLASSIC_LINK_MAX; i++) {
		if (!e->links[i].used || e->links[i].handle < 0)
			continue;
		cp.con_handle = htole16((uint16_t)e->links[i].handle);
		(void)hci_send_cmd(e->fd, HCI_OPCODE(OGF_STATUS,
		    NG_HCI_OCF_READ_RSSI), &cp, sizeof(cp));
	}
}
