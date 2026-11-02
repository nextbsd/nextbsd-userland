/*
 * security.c — the Classic engine's security manager. See security.h.
 *
 * The responder half is hcsecd's three handlers (usr.sbin/bluetooth/hcsecd/
 * hcsecd.c: process_pin_code_request_event, process_link_key_request_event,
 * process_link_key_notification_event) over the same raw socket, with three
 * differences: keys come from and go to the pairing store immediately, a key
 * from a device nobody paired is dropped rather than kept, and the PIN comes
 * from the user rather than a file. The Secure Simple Pairing half is new:
 * hcsecd never handled it, though the kernel has passed its events up since
 * FreeBSD 4ae0fa8a2f.
 */
#include "security.h"
#include "hci.h"
#include "hci_compat.h"

#include <string.h>
#include <time.h>

#define	OGF_LC		NG_HCI_OGF_LINK_CONTROL

/* Prompt ids, unique for the daemon's lifetime. */
static uint32_t	g_prompt_id;

static int
lc_cmd(struct bt_engine *e, uint16_t ocf, const void *p, size_t len)
{
	if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC, ocf), p, len) < 0) {
		classic_log(e, "send command 0x%04x failed",
		    HCI_OPCODE(OGF_LC, ocf));
		return (-1);
	}
	return (0);
}

/* A command whose only parameter is the peer's address. */
static void
lc_cmd_addr(struct bt_engine *e, uint16_t ocf, const struct bt_addr *peer)
{
	uint8_t wire[BT_ADDR_LEN];

	hci_addr_to_wire(peer, wire);
	(void)lc_cmd(e, ocf, wire, sizeof(wire));
}

static const char *
ntoa(const struct bt_addr *a, char *buf)
{
	return (bt_addr_ntoa(a, BT_ADDR_FMT_DISPLAY, buf, BT_ADDR_STRLEN));
}

/* Is `peer` the device the pairing in progress is with? */
static bool
pairing_with(const struct bt_engine *e, const struct bt_addr *peer)
{
	return (e->sec.stage != SEC_IDLE &&
	    memcmp(e->sec.peer.b, peer->b, BT_ADDR_LEN) == 0);
}

void
sec_init(struct bt_engine *e)
{
	(void)memset(&e->sec, 0, sizeof(e->sec));
	e->sec.handle = -1;
	e->sec.waiting = bt_pair_none_e;
}

/* End the pairing in progress, reporting `st` to the policy layer. */
static void
finish(struct bt_engine *e, bt_status_t st)
{
	struct bt_event ev;
	struct bt_bond b;
	char a[BT_ADDR_STRLEN];

	if (e->sec.stage == SEC_IDLE)
		return;
	(void)memset(&ev, 0, sizeof(ev));
	ev.peer = e->sec.peer;
	ev.transport = BT_TRANSPORT_BREDR;
	if (st == bt_status_success_e) {
		(void)memset(&b, 0, sizeof(b));
		b.addr = e->sec.peer;
		b.transports = BT_TRANSPORT_BREDR;
		ev.kind = BT_EV_PAIRED;
		ev.u.bond = &b;
		classic_log(e, "paired with %s (%s)", ntoa(&e->sec.peer, a),
		    e->sec.ssp ? "Secure Simple Pairing" : "legacy PIN");
	} else {
		ev.kind = BT_EV_PAIR_FAILED;
		ev.u.status = st;
		classic_log(e, "pairing with %s failed (status %d)",
		    ntoa(&e->sec.peer, a), (int)st);
	}
	sec_init(e);
	classic_raise(e, &ev);
}

/* Park a prompt with the policy layer. */
static void
prompt(struct bt_engine *e, bt_pair_method_t method, uint32_t passkey,
    uint8_t pin_min)
{
	struct bt_event ev;
	struct bt_pair_prompt *p = &ev.u.prompt;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_PAIR_PROMPT;
	ev.peer = e->sec.peer;
	ev.transport = BT_TRANSPORT_BREDR;
	if (++g_prompt_id == 0)
		g_prompt_id = 1;
	p->id = g_prompt_id;
	p->peer = e->sec.peer;
	p->transport = BT_TRANSPORT_BREDR;
	p->method = method;
	p->passkey = passkey;
	p->pin_min = pin_min;
	p->initiated = true;
	p->expires = time(NULL) + SEC_USER_TIMEOUT_MS / 1000;

	e->sec.waiting = method;
	e->sec.prompt_id = p->id;
	e->sec.user_deadline = bt_now_ms() + SEC_USER_TIMEOUT_MS;
	classic_raise(e, &ev);
}

/* The negative reply for whatever the user was asked. */
static void
refuse_waiting(struct bt_engine *e)
{
	switch (e->sec.waiting) {
	case bt_pair_just_works_e:
	case bt_pair_numeric_comparison_e:
		lc_cmd_addr(e, NG_HCI_USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY,
		    &e->sec.peer);
		break;
	case bt_pair_passkey_entry_e:
		lc_cmd_addr(e, HCI_OCF_USER_PASSKEY_REQ_NEG_REPLY,
		    &e->sec.peer);
		break;
	case bt_pair_pin_e:
		lc_cmd_addr(e, NG_HCI_OCF_PIN_CODE_NEG_REP, &e->sec.peer);
		break;
	case bt_pair_passkey_display_e:
		/* Nothing to answer; dropping the link ends it. */
		if (e->sec.handle >= 0) {
			ng_hci_discon_cp cp;

			cp.con_handle = htole16((uint16_t)e->sec.handle);
			cp.reason = HCI_ERR_REMOTE_USER_TERMINATED;
			(void)lc_cmd(e, NG_HCI_OCF_DISCON, &cp, sizeof(cp));
		}
		break;
	default:
		break;
	}
	e->sec.waiting = bt_pair_none_e;
}

static void
send_pin(struct bt_engine *e, const struct bt_addr *peer, const char *pin)
{
	ng_hci_pin_code_rep_cp cp;

	(void)memset(&cp, 0, sizeof(cp));
	hci_addr_to_wire(peer, (uint8_t *)&cp.bdaddr);
	(void)strncpy((char *)cp.pin, pin, sizeof(cp.pin));
	cp.pin_size = (uint8_t)strnlen(pin, sizeof(cp.pin));
	(void)lc_cmd(e, NG_HCI_OCF_PIN_CODE_REP, &cp, sizeof(cp));
}

static void
auth_request(struct bt_engine *e)
{
	ng_hci_auth_req_cp cp;

	cp.con_handle = htole16((uint16_t)e->sec.handle);
	e->sec.stage = SEC_AUTHENTICATING;
	if (lc_cmd(e, NG_HCI_OCF_AUTH_REQ, &cp, sizeof(cp)) < 0)
		finish(e, bt_status_internal_error_e);
}

/* ---- events ------------------------------------------------------------ */

static void
on_link_key_request(struct bt_engine *e, const struct bt_addr *peer)
{
	struct bt_bond b;
	char a[BT_ADDR_STRLEN];

	/* An explicit pair replaces whatever key there was. */
	if (!pairing_with(e, peer) &&
	    e->host.bond_lookup(e->host.ctx, peer, &b) &&
	    (b.flags & BT_BOND_LINK_KEY) != 0) {
		ng_hci_link_key_rep_cp cp;

		hci_addr_to_wire(peer, (uint8_t *)&cp.bdaddr);
		(void)memcpy(cp.key, b.link_key, sizeof(cp.key));
		(void)lc_cmd(e, NG_HCI_OCF_LINK_KEY_REP, &cp, sizeof(cp));
		(void)memset(&cp, 0, sizeof(cp));
		(void)memset(b.link_key, 0, sizeof(b.link_key));
		classic_log(e, "link key for %s answered from the store",
		    ntoa(peer, a));
		return;
	}
	lc_cmd_addr(e, NG_HCI_OCF_LINK_KEY_NEG_REP, peer);
}

static void
on_link_key_notification(struct bt_engine *e,
    const ng_hci_link_key_notification_ep *ep)
{
	struct bt_event ev;
	struct bt_bond b;
	struct bt_addr peer;
	char a[BT_ADDR_STRLEN];
	bool pairing, bonded;

	hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
	pairing = pairing_with(e, &peer);
	bonded = e->host.bond_lookup(e->host.ctx, &peer, &b);
	if (!pairing && !bonded) {
		/* hcsecd kept these; a stranger's key is not a bond. */
		classic_log(e, "dropping a link key from %s: not paired here",
		    ntoa(&peer, a));
		return;
	}

	(void)memset(&b, 0, sizeof(b));
	b.addr = peer;
	b.transports = BT_TRANSPORT_BREDR;
	b.flags = BT_BOND_LINK_KEY;
	(void)memcpy(b.link_key, ep->key, sizeof(b.link_key));
	b.link_key_type = ep->key_type;
	if (pairing)
		e->sec.got_key = true;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_BOND_UPDATED;
	ev.peer = peer;
	ev.transport = BT_TRANSPORT_BREDR;
	ev.u.bond = &b;
	classic_log(e, "new link key (type %u) for %s", ep->key_type,
	    ntoa(&peer, a));
	classic_raise(e, &ev);
	(void)memset(b.link_key, 0, sizeof(b.link_key));
}

static void
on_pin_request(struct bt_engine *e, const struct bt_addr *peer)
{
	char a[BT_ADDR_STRLEN];

	if (!pairing_with(e, peer)) {
		classic_log(e, "refusing a PIN request from %s: no pairing "
		    "was started here", ntoa(peer, a));
		lc_cmd_addr(e, NG_HCI_OCF_PIN_CODE_NEG_REP, peer);
		return;
	}
	if (e->sec.pin[0] != '\0') {
		send_pin(e, peer, e->sec.pin);
		return;
	}
	prompt(e, bt_pair_pin_e, 0, 1);
}

static void
on_io_cap_request(struct bt_engine *e, const struct bt_addr *peer)
{
	char a[BT_ADDR_STRLEN];

	if (!pairing_with(e, peer)) {
		ng_hci_io_capability_request_negative_reply_cp cp;

		classic_log(e, "rejecting an incoming pairing from %s",
		    ntoa(peer, a));
		hci_addr_to_wire(peer, (uint8_t *)&cp.bdaddr);
		cp.reason = HCI_ERR_PAIRING_NOT_ALLOWED;
		(void)lc_cmd(e, NG_HCI_IO_CAPABILITY_REQUEST_NEGATIVE_REPLY,
		    &cp, sizeof(cp));
		return;
	}
	{
		ng_hci_io_capability_request_reply_cp cp;

		hci_addr_to_wire(peer, (uint8_t *)&cp.bdaddr);
		cp.io_capability = HCI_IO_DISPLAY_YES_NO;
		cp.oob_data_present = 0;
		cp.authentication_requirements =
		    HCI_AUTH_MITM_DEDICATED_BONDING;
		e->sec.ssp = true;
		(void)lc_cmd(e, NG_HCI_IO_CAPABILITY_REQUEST_REPLY, &cp,
		    sizeof(cp));
	}
}

static void
on_user_confirmation(struct bt_engine *e, const struct bt_addr *peer,
    uint32_t value)
{
	char a[BT_ADDR_STRLEN];

	if (!pairing_with(e, peer)) {
		lc_cmd_addr(e, NG_HCI_USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY,
		    peer);
		return;
	}
	/*
	 * A peer with no input and no output (headphones, speakers) cannot
	 * compare anything: Just Works. We started this pairing, so accept.
	 */
	if (e->sec.peer_io_known &&
	    e->sec.peer_io == HCI_IO_NO_INPUT_NO_OUTPUT) {
		classic_log(e, "%s has no input or output: accepting",
		    ntoa(peer, a));
		lc_cmd_addr(e, NG_HCI_USER_CONFIRMATION_REQUEST_REPLY, peer);
		return;
	}
	prompt(e, bt_pair_numeric_comparison_e, value % 1000000, 0);
}

bool
sec_hci_event(struct bt_engine *e, uint8_t code, const uint8_t *p, size_t len)
{
	struct bt_addr peer;
	char a[BT_ADDR_STRLEN];

	switch (code) {
	case NG_HCI_EVENT_LINK_KEY_REQ:
		if (len < BT_ADDR_LEN)
			return (true);
		hci_addr_from_wire(p, &peer);
		on_link_key_request(e, &peer);
		return (true);

	case NG_HCI_EVENT_LINK_KEY_NOTIFICATION:
		if (len < sizeof(ng_hci_link_key_notification_ep))
			return (true);
		on_link_key_notification(e,
		    (const ng_hci_link_key_notification_ep *)p);
		return (true);

	case NG_HCI_EVENT_PIN_CODE_REQ:
		if (len < BT_ADDR_LEN)
			return (true);
		hci_addr_from_wire(p, &peer);
		on_pin_request(e, &peer);
		return (true);

	case NG_HCI_EVENT_IO_CAPABILITY_REQUEST:
		if (len < BT_ADDR_LEN)
			return (true);
		hci_addr_from_wire(p, &peer);
		on_io_cap_request(e, &peer);
		return (true);

	case HCI_EVENT_IO_CAPABILITY_RESPONSE: {
		const hci_io_capability_response_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (true);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		if (pairing_with(e, &peer)) {
			e->sec.peer_io_known = true;
			e->sec.peer_io = ep->io_capability;
			e->sec.peer_auth = ep->authentication_requirements;
		}
		return (true);
	}

	case NG_HCI_EVENT_USER_CONFIRMATION_REQUEST: {
		const ng_hci_user_confirmation_request_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (true);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		on_user_confirmation(e, &peer, le32toh(ep->numeric_value));
		return (true);
	}

	case HCI_EVENT_USER_PASSKEY_REQUEST:
		if (len < BT_ADDR_LEN)
			return (true);
		hci_addr_from_wire(p, &peer);
		if (!pairing_with(e, &peer))
			lc_cmd_addr(e, HCI_OCF_USER_PASSKEY_REQ_NEG_REPLY,
			    &peer);
		else
			prompt(e, bt_pair_passkey_entry_e, 0, 0);
		return (true);

	case HCI_EVENT_USER_PASSKEY_NOTIFICATION: {
		const hci_user_passkey_notification_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (true);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		if (pairing_with(e, &peer))
			prompt(e, bt_pair_passkey_display_e,
			    le32toh(ep->passkey) % 1000000, 0);
		return (true);
	}

	case HCI_EVENT_KEYPRESS_NOTIFICATION:
		return (true);

	case NG_HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
		const ng_hci_simple_pairing_complete_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (true);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		if (pairing_with(e, &peer)) {
			e->sec.waiting = bt_pair_none_e;
			if (ep->status != 0)
				finish(e, bt_status_pairing_failed_e);
		}
		return (true);
	}

	case NG_HCI_EVENT_CON_COMPL: {
		const ng_hci_con_compl_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (false);
		hci_addr_from_wire((const uint8_t *)&ep->bdaddr, &peer);
		if (!pairing_with(e, &peer) ||
		    e->sec.stage != SEC_CONNECTING ||
		    ep->link_type != NG_HCI_LINK_ACL)
			return (false);
		if (ep->status != 0) {
			classic_log(e, "connecting to %s failed: 0x%02x",
			    ntoa(&peer, a), ep->status);
			finish(e, bt_status_pairing_failed_e);
			return (false);
		}
		e->sec.handle = NG_HCI_CON_HANDLE(le16toh(ep->con_handle));
		auth_request(e);
		return (false);	/* discovery may want it too */
	}

	case NG_HCI_EVENT_AUTH_COMPL: {
		const ng_hci_auth_compl_ep *ep = (const void *)p;
		int h;

		if (len < sizeof(*ep))
			return (true);
		h = NG_HCI_CON_HANDLE(le16toh(ep->con_handle));
		if (e->sec.stage != SEC_AUTHENTICATING || h != e->sec.handle) {
			if (ep->status != 0)
				classic_log(e, "authentication on handle %d "
				    "failed: 0x%02x", h, ep->status);
			return (true);
		}
		if (ep->status != 0) {
			finish(e, ep->status == HCI_ERR_AUTH_FAILURE ?
			    bt_status_pairing_rejected_e :
			    bt_status_pairing_failed_e);
			return (true);
		}
		{
			ng_hci_set_con_encryption_cp cp;

			cp.con_handle = htole16((uint16_t)h);
			cp.encryption_enable = 1;
			(void)lc_cmd(e, NG_HCI_OCF_SET_CON_ENCRYPTION, &cp,
			    sizeof(cp));
		}
		finish(e, e->sec.got_key ? bt_status_success_e :
		    bt_status_pairing_failed_e);
		return (true);
	}

	case NG_HCI_EVENT_DISCON_COMPL: {
		const ng_hci_discon_compl_ep *ep = (const void *)p;

		if (len < sizeof(*ep))
			return (false);
		if (e->sec.stage != SEC_IDLE && e->sec.handle >= 0 &&
		    NG_HCI_CON_HANDLE(le16toh(ep->con_handle)) ==
		    e->sec.handle)
			finish(e, bt_status_pairing_failed_e);
		return (false);
	}

	case NG_HCI_EVENT_ENCRYPTION_CHANGE: {
		const ng_hci_encryption_change_ep *ep = (const void *)p;

		if (len >= sizeof(*ep) && ep->status != 0)
			classic_log(e, "encryption on handle %u failed: "
			    "0x%02x", NG_HCI_CON_HANDLE(
			    le16toh(ep->con_handle)), ep->status);
		return (true);
	}

	default:
		return (false);
	}
}

void
sec_cmd_result(struct bt_engine *e, uint16_t opcode, uint8_t status)
{
	if (status == 0 || e->sec.stage == SEC_IDLE)
		return;
	if (opcode == HCI_OPCODE(OGF_LC, NG_HCI_OCF_CREATE_CON) &&
	    e->sec.stage == SEC_CONNECTING) {
		classic_log(e, "Create_Connection refused: 0x%02x", status);
		finish(e, bt_status_pairing_failed_e);
	} else if (opcode == HCI_OPCODE(OGF_LC, NG_HCI_OCF_AUTH_REQ) &&
	    e->sec.stage == SEC_AUTHENTICATING) {
		classic_log(e, "Authentication_Requested refused: 0x%02x",
		    status);
		finish(e, bt_status_pairing_failed_e);
	}
}

bt_status_t
sec_pair(struct bt_engine *e, const struct bt_addr *peer, const char *pin)
{
	char a[BT_ADDR_STRLEN];
	int i;

	if (!e->powered)
		return (bt_status_powered_off_e);
	if (e->sec.stage != SEC_IDLE)
		return (bt_status_busy_e);
	if (pin != NULL && strlen(pin) > BT_PIN_MAX)
		return (bt_status_invalid_parameter_e);

	sec_init(e);
	e->sec.peer = *peer;
	e->sec.peer.type = BT_ADDR_PUBLIC;
	if (pin != NULL)
		(void)strlcpy(e->sec.pin, pin, sizeof(e->sec.pin));
	e->sec.deadline = bt_now_ms() + SEC_PAIR_TIMEOUT_MS;

	e->sec.handle = hci_acl_handle(e->fd, peer);
	classic_log(e, "pairing with %s (%s)", ntoa(peer, a),
	    e->sec.handle >= 0 ? "already connected" : "connecting");
	if (e->sec.handle >= 0) {
		auth_request(e);
	} else {
		ng_hci_create_con_cp cp;

		(void)memset(&cp, 0, sizeof(cp));
		hci_addr_to_wire(peer, (uint8_t *)&cp.bdaddr);
		cp.pkt_type = htole16(NG_HCI_PKT_DM1 | NG_HCI_PKT_DH1 |
		    NG_HCI_PKT_DM3 | NG_HCI_PKT_DH3 |
		    NG_HCI_PKT_DM5 | NG_HCI_PKT_DH5);
		cp.page_scan_rep_mode = 0x02;	/* R2 unless discovery knew */
		cp.page_scan_mode = 0;
		cp.clock_offset = 0;
		cp.accept_role_switch = 1;
		for (i = 0; i < e->ninq; i++) {
			if (memcmp(e->inq[i].addr.b, peer->b,
			    BT_ADDR_LEN) == 0) {
				cp.page_scan_rep_mode = e->inq[i].psrm;
				cp.clock_offset = htole16(
				    e->inq[i].clock_offset | 0x8000);
				break;
			}
		}
		e->sec.stage = SEC_CONNECTING;
		if (lc_cmd(e, NG_HCI_OCF_CREATE_CON, &cp, sizeof(cp)) < 0) {
			sec_init(e);
			return (bt_status_internal_error_e);
		}
	}
	/* auth_request may already have failed it. */
	return (e->sec.stage == SEC_IDLE ? bt_status_internal_error_e :
	    bt_status_success_e);
}

bt_status_t
sec_pair_reply(struct bt_engine *e, const struct bt_pair_answer *ans)
{
	bt_pair_method_t m = e->sec.waiting;

	if (m == bt_pair_none_e || ans->id != e->sec.prompt_id ||
	    !pairing_with(e, &ans->peer))
		return (bt_status_no_prompt_e);

	if (!ans->accept) {
		refuse_waiting(e);
		finish(e, bt_status_pairing_rejected_e);
		return (bt_status_success_e);
	}

	switch (m) {
	case bt_pair_just_works_e:
	case bt_pair_numeric_comparison_e:
		lc_cmd_addr(e, NG_HCI_USER_CONFIRMATION_REQUEST_REPLY,
		    &e->sec.peer);
		break;
	case bt_pair_passkey_entry_e: {
		hci_user_passkey_reply_cp cp;

		if (ans->passkey > BT_PASSKEY_MAX)
			return (bt_status_invalid_parameter_e);
		hci_addr_to_wire(&e->sec.peer, (uint8_t *)&cp.bdaddr);
		cp.passkey = htole32(ans->passkey);
		(void)lc_cmd(e, HCI_OCF_USER_PASSKEY_REQ_REPLY, &cp,
		    sizeof(cp));
		break;
	}
	case bt_pair_pin_e:
		if (ans->pin[0] == '\0' || strlen(ans->pin) > BT_PIN_MAX)
			return (bt_status_invalid_parameter_e);
		send_pin(e, &e->sec.peer, ans->pin);
		break;
	case bt_pair_passkey_display_e:
	default:
		/* Acknowledged; the peer's keyboard finishes it. */
		break;
	}
	e->sec.waiting = bt_pair_none_e;
	return (bt_status_success_e);
}

bt_status_t
sec_unpair(struct bt_engine *e, const struct bt_addr *peer)
{
	ng_hci_delete_stored_link_key_cp dk;
	int h;

	if (pairing_with(e, peer)) {
		refuse_waiting(e);
		finish(e, bt_status_pairing_rejected_e);
	}

	/* Keys some controllers keep for themselves. */
	(void)memset(&dk, 0, sizeof(dk));
	hci_addr_to_wire(peer, (uint8_t *)&dk.bdaddr);
	dk.delete_all = 0;
	(void)hci_send_cmd(e->fd, HCI_OPCODE(NG_HCI_OGF_HC_BASEBAND,
	    NG_HCI_OCF_DELETE_STORED_LINK_KEY), &dk, sizeof(dk));

	h = hci_acl_handle(e->fd, peer);
	if (h >= 0) {
		ng_hci_discon_cp cp;

		cp.con_handle = htole16((uint16_t)h);
		cp.reason = HCI_ERR_REMOTE_USER_TERMINATED;
		(void)lc_cmd(e, NG_HCI_OCF_DISCON, &cp, sizeof(cp));
	}
	return (bt_status_success_e);
}

void
sec_abort(struct bt_engine *e, bt_status_t why)
{
	if (e->sec.stage == SEC_IDLE)
		return;
	refuse_waiting(e);
	finish(e, why);
}

uint64_t
sec_next_timer(struct bt_engine *e)
{
	uint64_t t = 0;

	if (e->sec.stage == SEC_IDLE)
		return (0);
	t = e->sec.deadline;
	if (e->sec.waiting != bt_pair_none_e && e->sec.user_deadline < t)
		t = e->sec.user_deadline;
	return (t);
}

void
sec_timer(struct bt_engine *e, uint64_t now)
{
	if (e->sec.stage == SEC_IDLE)
		return;
	if ((e->sec.waiting != bt_pair_none_e && now >= e->sec.user_deadline) ||
	    now >= e->sec.deadline) {
		refuse_waiting(e);
		finish(e, bt_status_pairing_timeout_e);
	}
}
