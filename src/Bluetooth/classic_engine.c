/*
 * classic_engine.c — the BR/EDR engine behind engine.h.
 *
 * Two halves over the controller's shared raw HCI socket:
 *
 *   discovery (here)   Inquiry with RSSI and Extended Inquiry Response, then
 *                      a Remote_Name_Request for each device whose name
 *                      neither its EIR nor bluetoothd's cache gave (#208)
 *   security           security.c: link keys, PIN, Secure Simple Pairing,
 *                      pair and unpair (#209)
 *
 * Every device seen is raised as BT_EV_FOUND, a name found later as another
 * BT_EV_FOUND for the same address; the policy layer merges them into its
 * scan cache. Connections (#213) come later.
 */
#include "classic.h"
#include "hci.h"
#include "hci_compat.h"
#include "security.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define	OGF_LC			NG_HCI_OGF_LINK_CONTROL

/* General/Unlimited Inquiry Access Code, 0x9e8b33, LSB first. */
static const uint8_t giac[NG_HCI_LAP_SIZE] = { 0x33, 0x8b, 0x9e };

#define	INQUIRY_UNIT_MS		1280	/* Inquiry_Length is in 1.28 s */
#define	INQUIRY_GUARD_MS	5000	/* beyond the length, then give up */
#define	NAME_REQUEST_MS		6000	/* one Remote_Name_Request */
#define	NAME_PASS_MS		15000	/* all of them after an inquiry */

void
classic_log(struct bt_engine *e, const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[classic] %s: ", e->node);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

void
classic_raise(struct bt_engine *e, const struct bt_event *ev)
{
	if (e->host.event != NULL)
		e->host.event(e->host.ctx, e, ev);
}

static void
raise_discovery(struct bt_engine *e, bool scanning)
{
	struct bt_event ev;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_DISCOVERY;
	ev.transport = BT_TRANSPORT_BREDR;
	ev.u.scanning = scanning;
	classic_raise(e, &ev);
}

/* ---- discovery ---------------------------------------------------------- */

/* Copy the device name out of EIR data, if it has one. */
static bool
eir_name(const uint8_t *eir, size_t len, char *out, size_t outlen)
{
	size_t i = 0;

	while (i + 1 < len) {
		uint8_t flen = eir[i], type;

		if (flen == 0 || i + 1 + flen > len)
			break;
		type = eir[i + 1];
		if (type == HCI_EIR_NAME_COMPLETE ||
		    type == HCI_EIR_NAME_SHORT) {
			size_t n = flen - 1;

			if (n >= outlen)
				n = outlen - 1;
			(void)memcpy(out, eir + i + 2, n);
			out[n] = '\0';
			return (n > 0);
		}
		i += 1 + flen;
	}
	return (false);
}

static uint32_t
cod(const uint8_t c[NG_HCI_CLASS_SIZE])
{
	return ((uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16);
}

/* Record one inquiry response and tell the policy layer. */
static void
found(struct bt_engine *e, const uint8_t *bdaddr, uint8_t psrm,
    uint16_t clock_offset, const uint8_t uclass[NG_HCI_CLASS_SIZE],
    int rssi, const uint8_t *eir, size_t eir_len)
{
	struct bt_event ev;
	struct bt_found *f = &ev.u.found;
	struct classic_inq *q = NULL;
	int i;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_FOUND;
	ev.transport = BT_TRANSPORT_BREDR;
	hci_addr_from_wire(bdaddr, &f->addr);
	ev.peer = f->addr;
	f->transports = BT_TRANSPORT_BREDR;
	f->rssi = (int8_t)rssi;
	f->class_of_device = cod(uclass);
	f->last_seen = time(NULL);
	if (eir != NULL && eir_len > 0) {
		if (eir_len > sizeof(f->eir))
			eir_len = sizeof(f->eir);
		(void)memcpy(f->eir, eir, eir_len);
		f->eir_len = (uint16_t)eir_len;
		(void)eir_name(eir, eir_len, f->name, sizeof(f->name));
	}

	for (i = 0; i < e->ninq; i++) {
		if (bt_addr_equal(&e->inq[i].addr, &f->addr)) {
			q = &e->inq[i];
			break;
		}
	}
	if (q == NULL && e->ninq < CLASSIC_INQ_MAX)
		q = &e->inq[e->ninq++];
	if (q != NULL) {
		q->addr = f->addr;
		q->psrm = psrm;
		q->clock_offset = le16toh(clock_offset);
		if (f->name[0] != '\0')
			q->named = true;
	}
	classic_raise(e, &ev);
}

/* Ask for the next unnamed device's name; end the pass when none is left. */
static void
name_next(struct bt_engine *e)
{
	uint64_t now = bt_now_ms();
	int i;

	e->name_idx = -1;
	if (!e->naming)
		return;
	for (i = 0; i < e->ninq && now < e->names_deadline; i++) {
		struct classic_inq *q = &e->inq[i];
		ng_hci_remote_name_req_cp cp;

		if (q->named)
			continue;
		q->named = true;	/* one try per pass */
		if (e->host.name_known != NULL &&
		    e->host.name_known(e->host.ctx, &q->addr))
			continue;
		(void)memset(&cp, 0, sizeof(cp));
		hci_addr_to_wire(&q->addr, (uint8_t *)&cp.bdaddr);
		cp.page_scan_rep_mode = q->psrm;
		cp.clock_offset = htole16(q->clock_offset | 0x8000);
		if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC,
		    NG_HCI_OCF_REMOTE_NAME_REQ), &cp, sizeof(cp)) < 0)
			continue;
		e->name_idx = i;
		e->name_deadline = now + NAME_REQUEST_MS;
		return;
	}
	e->naming = false;
	raise_discovery(e, false);
}

static void
inquiry_done(struct bt_engine *e)
{
	e->inquiring = false;
	e->naming = true;
	e->names_deadline = bt_now_ms() + NAME_PASS_MS;
	name_next(e);
}

static bt_status_t
discover(struct bt_engine *e, bool start, int seconds)
{
	ng_hci_inquiry_cp cp;
	int len;

	if (!start) {
		if (e->inquiring)
			(void)hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC,
			    NG_HCI_OCF_INQUIRY_CANCEL), NULL, 0);
		if (e->inquiring || e->naming) {
			e->inquiring = e->naming = false;
			e->name_idx = -1;
			raise_discovery(e, false);
		}
		return (bt_status_success_e);
	}
	if (!e->powered)
		return (bt_status_powered_off_e);
	if (e->inquiring || e->naming)
		return (bt_status_success_e);	/* join the scan running */

	len = (seconds * 1000 + INQUIRY_UNIT_MS - 1) / INQUIRY_UNIT_MS;
	if (len < 1)
		len = 1;
	if (len > 0x30)
		len = 0x30;
	(void)memcpy(cp.lap, giac, sizeof(cp.lap));
	cp.inquiry_length = (uint8_t)len;
	cp.num_responses = 0;		/* unlimited */
	if (hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC, NG_HCI_OCF_INQUIRY), &cp,
	    sizeof(cp)) < 0)
		return (bt_status_internal_error_e);

	e->inquiring = true;
	e->naming = false;
	e->ninq = 0;
	e->name_idx = -1;
	e->inquiry_deadline = bt_now_ms() + (uint64_t)len * INQUIRY_UNIT_MS +
	    INQUIRY_GUARD_MS;
	classic_log(e, "inquiry for %d.%02d s", len * 128 / 100,
	    len * 128 % 100);
	raise_discovery(e, true);
	return (bt_status_success_e);
}

static void
discovery_event(struct bt_engine *e, uint8_t code, const uint8_t *p,
    size_t len)
{
	size_t i, n;

	switch (code) {
	case NG_HCI_EVENT_INQUIRY_RESULT: {
		const ng_hci_inquiry_response *r;

		if (len < 1)
			return;
		n = p[0];
		r = (const ng_hci_inquiry_response *)(p + 1);
		for (i = 0; i < n && 1 + (i + 1) * sizeof(*r) <= len; i++)
			found(e, (const uint8_t *)&r[i].bdaddr,
			    r[i].page_scan_rep_mode, r[i].clock_offset,
			    r[i].uclass, 127, NULL, 0);
		break;
	}
	case HCI_EVENT_INQUIRY_RESULT_RSSI: {
		const hci_inquiry_response_rssi *r;

		if (len < 1)
			return;
		n = p[0];
		r = (const hci_inquiry_response_rssi *)(p + 1);
		for (i = 0; i < n && 1 + (i + 1) * sizeof(*r) <= len; i++)
			found(e, (const uint8_t *)&r[i].bdaddr,
			    r[i].page_scan_rep_mode, r[i].clock_offset,
			    r[i].uclass, r[i].rssi, NULL, 0);
		break;
	}
	case HCI_EVENT_EXT_INQUIRY_RESULT: {
		const hci_ext_inquiry_result_ep *r = (const void *)p;
		size_t hdr = offsetof(hci_ext_inquiry_result_ep, eir);

		if (len < hdr)
			return;
		found(e, (const uint8_t *)&r->bdaddr, r->page_scan_rep_mode,
		    r->clock_offset, r->uclass, r->rssi, r->eir, len - hdr);
		break;
	}
	case NG_HCI_EVENT_INQUIRY_COMPL:
		if (e->inquiring)
			inquiry_done(e);
		break;
	case NG_HCI_EVENT_REMOTE_NAME_REQ_COMPL: {
		const ng_hci_remote_name_req_compl_ep *r = (const void *)p;
		struct bt_event ev;
		struct bt_addr a;

		if (len < offsetof(ng_hci_remote_name_req_compl_ep, name))
			return;
		hci_addr_from_wire((const uint8_t *)&r->bdaddr, &a);
		if (r->status == 0) {
			size_t nl = len - offsetof(
			    ng_hci_remote_name_req_compl_ep, name);

			(void)memset(&ev, 0, sizeof(ev));
			ev.kind = BT_EV_FOUND;
			ev.transport = BT_TRANSPORT_BREDR;
			ev.peer = a;
			ev.u.found.addr = a;
			ev.u.found.transports = BT_TRANSPORT_BREDR;
			ev.u.found.rssi = 127;
			ev.u.found.last_seen = time(NULL);
			if (nl > BT_NAME_MAX)
				nl = BT_NAME_MAX;
			(void)memcpy(ev.u.found.name, r->name, nl);
			ev.u.found.name[nl] = '\0';
			if (ev.u.found.name[0] != '\0')
				classic_raise(e, &ev);
		}
		if (e->naming && e->name_idx >= 0 &&
		    bt_addr_equal(&e->inq[e->name_idx].addr, &a))
			name_next(e);
		break;
	}
	default:
		break;
	}
}

/* ---- engine ops ----------------------------------------------------------- */

static bt_status_t
classic_attach(const struct bt_controller_desc *c,
    const struct bt_engine_host *host, struct bt_engine **out)
{
	struct bt_engine *e;

	*out = NULL;
	if (c->hci_fd < 0)
		return (bt_status_no_adapter_e);
	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return (bt_status_internal_error_e);
	e->host = *host;
	(void)strlcpy(e->node, c->node, sizeof(e->node));
	(void)strlcpy(e->hci_node, c->hci_node, sizeof(e->hci_node));
	e->fd = c->hci_fd;
	(void)memcpy(e->features, c->features, sizeof(e->features));
	e->powered = c->powered;
	e->name_idx = -1;
	sec_init(e);
	*out = e;
	return (bt_status_success_e);
}

static void
classic_detach(struct bt_engine *e)
{
	if (e == NULL)
		return;
	sec_abort(e, bt_status_no_adapter_e);
	free(e);
}

static void
classic_hci_event(struct bt_engine *e, uint8_t code, const uint8_t *params,
    size_t len)
{
	struct hci_event ev;
	uint16_t opcode;
	uint8_t status;
	const uint8_t *rp;
	size_t rplen;

	ev.code = code;
	ev.len = (uint8_t)len;
	ev.params = params;
	if (hci_cmd_result(&ev, &opcode, &status, &rp, &rplen)) {
		sec_cmd_result(e, opcode, status);
		if (status == 0)
			return;
		if (opcode == HCI_OPCODE(OGF_LC, NG_HCI_OCF_INQUIRY) &&
		    e->inquiring) {
			classic_log(e, "Inquiry refused: 0x%02x", status);
			e->inquiring = false;
			raise_discovery(e, false);
		} else if (opcode == HCI_OPCODE(OGF_LC,
		    NG_HCI_OCF_REMOTE_NAME_REQ) && e->naming) {
			name_next(e);
		}
		return;
	}
	if (sec_hci_event(e, code, params, len))
		return;
	discovery_event(e, code, params, len);
}

static bt_status_t
classic_set_power(struct bt_engine *e, bool on)
{
	if (!on) {
		(void)discover(e, false, 0);
		sec_abort(e, bt_status_powered_off_e);
	}
	e->powered = on;
	return (bt_status_success_e);
}

static bt_status_t
classic_pair(struct bt_engine *e, const struct bt_addr *peer, bt_io_cap_t io,
    const char *pin)
{
	(void)io;	/* Classic always offers DisplayYesNo */
	if (e->inquiring || e->naming)
		(void)discover(e, false, 0);	/* paging beats inquiring */
	return (sec_pair(e, peer, pin));
}

static bt_status_t
classic_pair_reply(struct bt_engine *e, const struct bt_pair_answer *a)
{
	return (sec_pair_reply(e, a));
}

static bt_status_t
classic_unpair(struct bt_engine *e, const struct bt_addr *peer)
{
	return (sec_unpair(e, peer));
}

static uint64_t
classic_next_timer(struct bt_engine *e)
{
	uint64_t t = sec_next_timer(e);

	if (e->inquiring && (t == 0 || e->inquiry_deadline < t))
		t = e->inquiry_deadline;
	if (e->naming) {
		uint64_t n = e->name_idx >= 0 ? e->name_deadline :
		    e->names_deadline;

		if (t == 0 || n < t)
			t = n;
	}
	return (t);
}

static void
classic_timer(struct bt_engine *e, uint64_t now)
{
	sec_timer(e, now);
	if (e->inquiring && now >= e->inquiry_deadline) {
		/* The controller never said Inquiry_Complete. */
		classic_log(e, "inquiry overran; ending it");
		(void)hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC,
		    NG_HCI_OCF_INQUIRY_CANCEL), NULL, 0);
		inquiry_done(e);
	}
	if (e->naming && e->name_idx >= 0 && now >= e->name_deadline) {
		ng_hci_remote_name_req_cp cp;

		(void)memset(&cp, 0, sizeof(cp));
		hci_addr_to_wire(&e->inq[e->name_idx].addr,
		    (uint8_t *)&cp.bdaddr);
		/* Remote_Name_Request_Cancel takes just the address. */
		(void)hci_send_cmd(e->fd, HCI_OPCODE(OGF_LC, 0x001a),
		    &cp.bdaddr, sizeof(cp.bdaddr));
		name_next(e);
	} else if (e->naming && e->name_idx < 0 && now >= e->names_deadline) {
		name_next(e);
	}
}

const struct bt_engine_ops bt_classic_engine = {
	.name		= "classic",
	.transport	= BT_TRANSPORT_BREDR,
	.attach		= classic_attach,
	.detach		= classic_detach,
	.hci_event	= classic_hci_event,
	.set_power	= classic_set_power,
	.discover	= discover,
	.pair		= classic_pair,
	.pair_reply	= classic_pair_reply,
	.unpair		= classic_unpair,
	.next_timer	= classic_next_timer,
	.timer		= classic_timer,
};
