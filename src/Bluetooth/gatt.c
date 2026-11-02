/*
 * gatt.c — the GATT client procedures (Core v5.4 Vol 3 Part G §4) over the
 * ATT bearer. See le.h.
 *
 * Discovery is one state machine, driven by the ATT responses:
 *
 *   MTU         Exchange MTU (§4.3.1)
 *   SERVICES    Discover All Primary Services: Read By Group Type 0x2800
 *               from handle 1 until "attribute not found" (§4.4.1)
 *   CHARS       Discover All Characteristics of the services HOGP reads
 *               (HID, Battery, Device Information): Read By Type 0x2803
 *               over each service's range (§4.6.1)
 *   DESCS       Discover All Characteristic Descriptors of the
 *               characteristics that can have the two HOGP cares about
 *               (CCCD 0x2902, Report Reference 0x2908): Find Information
 *               between the value handle and the characteristic's end
 *               (§4.7.1)
 *
 * Every step continues from the last handle returned plus one, and gives up
 * if a response does not move forward, so a confused server cannot loop us.
 * 128-bit UUIDs are kept as UUID 0: nothing HOGP needs has one.
 */
#include "le.h"

#include <string.h>

enum {
	GD_IDLE = 0,
	GD_MTU,
	GD_SERVICES,
	GD_CHARS,
	GD_DESCS
};

static uint16_t
get16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static bool
wanted_service(uint16_t uuid)
{
	return (uuid == GATT_SVC_HID || uuid == GATT_SVC_BATTERY ||
	    uuid == GATT_SVC_DEVICE_INFO);
}

static void	disc_step(struct le_conn *c);

static void
disc_finish(struct le_conn *c, bool ok)
{
	struct gatt_db *db = &c->db;
	gatt_done_fn done = db->done;

	db->phase = GD_IDLE;
	db->done = NULL;
	if (done != NULL)
		done(c, ok);
}

static void
disc_rsp(struct le_conn *c, void *arg, uint8_t err, const uint8_t *pdu,
    size_t len)
{
	struct gatt_db *db = &c->db;
	size_t elen, off;
	uint16_t last = 0;

	(void)arg;
	switch (db->phase) {
	case GD_MTU:
		if (err == 0 && len >= 3) {
			uint16_t m = get16(pdu + 1);

			if (m < LE_ATT_MTU_DEFAULT)
				m = LE_ATT_MTU_DEFAULT;
			c->att.mtu = m < LE_ATT_MTU_LOCAL ? m :
			    LE_ATT_MTU_LOCAL;
		}
		/* An old server that refuses the exchange keeps 23. */
		db->phase = GD_SERVICES;
		db->next = 0x0001;
		break;

	case GD_SERVICES:
		if (err == ATT_ECODE_ATTR_NOT_FOUND) {
			db->phase = GD_CHARS;
			db->idx = -1;
			db->next = 0;
			break;
		}
		if (err != 0 || len < 2 || (elen = pdu[1]) < 6) {
			le_log(c->e, "service discovery failed (0x%02x)", err);
			disc_finish(c, false);
			return;
		}
		for (off = 2; off + elen <= len; off += elen) {
			struct gatt_svc *s;

			if (db->nsvc >= LE_GATT_SVC_MAX)
				break;
			s = &db->svc[db->nsvc++];
			s->start = get16(pdu + off);
			s->end = get16(pdu + off + 2);
			s->uuid = elen == 6 ? get16(pdu + off + 4) : 0;
			last = s->end;
		}
		if (last < db->next || last == 0xffff ||
		    db->nsvc >= LE_GATT_SVC_MAX) {
			db->phase = GD_CHARS;
			db->idx = -1;
			db->next = 0;
		} else
			db->next = (uint16_t)(last + 1);
		break;

	case GD_CHARS: {
		struct gatt_svc *s = &db->svc[db->idx];

		if (err == ATT_ECODE_ATTR_NOT_FOUND) {
			db->next = 0;	/* this service is done */
			break;
		}
		if (err != 0 || len < 2 || (elen = pdu[1]) < 7) {
			le_log(c->e, "characteristic discovery failed "
			    "(0x%02x)", err);
			disc_finish(c, false);
			return;
		}
		for (off = 2; off + elen <= len; off += elen) {
			struct gatt_chr *ch;
			uint16_t decl = get16(pdu + off);

			if (db->nchr >= LE_GATT_CHR_MAX)
				break;
			/* The one before ends where this one starts. */
			if (db->nchr > 0 && db->chr[db->nchr - 1].svc ==
			    db->idx && decl > 0)
				db->chr[db->nchr - 1].end =
				    (uint16_t)(decl - 1);
			ch = &db->chr[db->nchr++];
			(void)memset(ch, 0, sizeof(*ch));
			ch->svc = db->idx;
			ch->svc_uuid = s->uuid;
			ch->decl = decl;
			ch->props = pdu[off + 2];
			ch->value = get16(pdu + off + 3);
			ch->uuid = elen == 7 ? get16(pdu + off + 5) : 0;
			ch->end = s->end;
			last = decl;
		}
		if (last < db->next || last >= s->end ||
		    db->nchr >= LE_GATT_CHR_MAX)
			db->next = 0;
		else
			db->next = (uint16_t)(last + 1);
		break;
	}

	case GD_DESCS: {
		struct gatt_chr *ch = &db->chr[db->idx];

		if (err == ATT_ECODE_ATTR_NOT_FOUND) {
			db->next = 0;
			break;
		}
		if (err != 0 || len < 2) {
			/* Descriptors are optional to HOGP; carry on. */
			db->next = 0;
			break;
		}
		elen = pdu[1] == 1 ? 4 : 18;
		for (off = 2; off + elen <= len; off += elen) {
			uint16_t h = get16(pdu + off);

			if (elen == 4) {
				uint16_t u = get16(pdu + off + 2);

				if (u == GATT_CCCD)
					ch->cccd = h;
				else if (u == GATT_REPORT_REF)
					ch->report_ref = h;
			}
			last = h;
		}
		if (last < db->next || last >= ch->end)
			db->next = 0;
		else
			db->next = (uint16_t)(last + 1);
		break;
	}

	default:
		return;
	}
	disc_step(c);
}

/* Send the next request of the discovery, or finish it. */
static void
disc_step(struct le_conn *c)
{
	struct gatt_db *db = &c->db;
	uint8_t pdu[7];

	switch (db->phase) {
	case GD_SERVICES:
		pdu[0] = ATT_READ_BY_GROUP_REQ;
		put16(pdu + 1, db->next);
		put16(pdu + 3, 0xffff);
		put16(pdu + 5, GATT_PRIMARY_SERVICE);
		if (att_request(c, pdu, 7, disc_rsp, NULL) != 0)
			disc_finish(c, false);
		return;

	case GD_CHARS:
		if (db->next == 0) {
			/* On to the next service HOGP wants. */
			while (++db->idx < db->nsvc &&
			    !wanted_service(db->svc[db->idx].uuid))
				;
			if (db->idx >= db->nsvc) {
				db->phase = GD_DESCS;
				db->idx = -1;
				db->next = 0;
				disc_step(c);
				return;
			}
			db->next = db->svc[db->idx].start;
		}
		pdu[0] = ATT_READ_BY_TYPE_REQ;
		put16(pdu + 1, db->next);
		put16(pdu + 3, db->svc[db->idx].end);
		put16(pdu + 5, GATT_CHARACTERISTIC);
		if (att_request(c, pdu, 7, disc_rsp, NULL) != 0)
			disc_finish(c, false);
		return;

	case GD_DESCS:
		if (db->next == 0) {
			struct gatt_chr *ch = NULL;

			while (++db->idx < db->nchr) {
				ch = &db->chr[db->idx];
				if (ch->value < ch->end &&
				    (ch->uuid == GATT_CHR_REPORT ||
				    (ch->props & (GATT_PROP_NOTIFY |
				    GATT_PROP_INDICATE)) != 0))
					break;
			}
			if (db->idx >= db->nchr) {
				disc_finish(c, true);
				return;
			}
			db->next = (uint16_t)(ch->value + 1);
		}
		pdu[0] = ATT_FIND_INFO_REQ;
		put16(pdu + 1, db->next);
		put16(pdu + 3, db->chr[db->idx].end);
		if (att_request(c, pdu, 5, disc_rsp, NULL) != 0)
			disc_finish(c, false);
		return;

	default:
		return;
	}
}

void
gatt_discover(struct le_conn *c, gatt_done_fn done)
{
	struct gatt_db *db = &c->db;
	uint8_t pdu[3];

	db->nsvc = 0;
	db->nchr = 0;
	db->done = done;
	db->phase = GD_MTU;
	pdu[0] = ATT_MTU_REQ;
	put16(pdu + 1, LE_ATT_MTU_LOCAL);
	if (att_request(c, pdu, sizeof(pdu), disc_rsp, NULL) != 0)
		disc_finish(c, false);
}

struct gatt_chr *
gatt_find(struct le_conn *c, uint16_t svc, uint16_t uuid,
    struct gatt_chr *from)
{
	struct gatt_db *db = &c->db;
	int i = from != NULL ? (int)(from - db->chr) + 1 : 0;

	for (; i < db->nchr; i++) {
		if (db->chr[i].svc_uuid == svc && db->chr[i].uuid == uuid)
			return (&db->chr[i]);
	}
	return (NULL);
}

/* ---- reads and writes ---------------------------------------------------------- */

int
gatt_read(struct le_conn *c, uint16_t handle, att_done_fn done, void *arg)
{
	uint8_t pdu[3];

	pdu[0] = ATT_READ_REQ;
	put16(pdu + 1, handle);
	return (att_request(c, pdu, sizeof(pdu), done, arg));
}

static void
lr_finish(struct le_conn *c, uint8_t err)
{
	struct gatt_db *db = &c->db;
	gatt_read_fn done = db->lr_done;

	db->lr_active = false;
	db->lr_done = NULL;
	if (done != NULL)
		done(c, db->lr_arg, err, db->lr_buf, db->lr_len);
}

static void
lr_rsp(struct le_conn *c, void *arg, uint8_t err, const uint8_t *pdu,
    size_t len)
{
	struct gatt_db *db = &c->db;
	size_t n;
	uint8_t req[5];

	(void)arg;
	if (!db->lr_active)
		return;
	if (err != 0) {
		/* A value that is exactly MTU-1 long ends this way. */
		lr_finish(c, (db->lr_len > 0 &&
		    (err == ATT_ECODE_ATTR_NOT_LONG || err == 0x07)) ? 0 :
		    err);
		return;
	}
	n = len - 1;
	if (n > sizeof(db->lr_buf) - db->lr_len)
		n = sizeof(db->lr_buf) - db->lr_len;
	(void)memcpy(db->lr_buf + db->lr_len, pdu + 1, n);
	db->lr_len += n;
	if (len - 1 < (size_t)(c->att.mtu - 1) ||
	    db->lr_len >= sizeof(db->lr_buf)) {
		lr_finish(c, 0);
		return;
	}
	req[0] = ATT_READ_BLOB_REQ;
	put16(req + 1, db->lr_handle);
	put16(req + 3, (uint16_t)db->lr_len);
	if (att_request(c, req, sizeof(req), lr_rsp, NULL) != 0)
		lr_finish(c, ATT_ECODE_LINK_LOST);
}

int
gatt_read_long(struct le_conn *c, uint16_t handle, gatt_read_fn done,
    void *arg)
{
	struct gatt_db *db = &c->db;

	if (db->lr_active)
		return (-1);
	db->lr_active = true;
	db->lr_handle = handle;
	db->lr_len = 0;
	db->lr_done = done;
	db->lr_arg = arg;
	if (gatt_read(c, handle, lr_rsp, NULL) != 0) {
		db->lr_active = false;
		return (-1);
	}
	return (0);
}

int
gatt_write(struct le_conn *c, uint16_t handle, const uint8_t *v, size_t len,
    att_done_fn done, void *arg)
{
	uint8_t pdu[LE_ATT_PDU_MAX];

	if (len + 3 > sizeof(pdu))
		return (-1);
	pdu[0] = ATT_WRITE_REQ;
	put16(pdu + 1, handle);
	if (len > 0)
		(void)memcpy(pdu + 3, v, len);
	return (att_request(c, pdu, len + 3, done, arg));
}

int
gatt_write_cmd(struct le_conn *c, uint16_t handle, const uint8_t *v,
    size_t len)
{
	uint8_t pdu[LE_ATT_PDU_MAX];

	if (len + 3 > sizeof(pdu))
		return (-1);
	pdu[0] = ATT_WRITE_CMD;
	put16(pdu + 1, handle);
	if (len > 0)
		(void)memcpy(pdu + 3, v, len);
	return (att_command(c, pdu, len + 3));
}

int
gatt_subscribe(struct le_conn *c, struct gatt_chr *ch, att_done_fn done,
    void *arg)
{
	uint8_t v[2];

	if (ch->cccd == 0)
		return (-1);
	/* Notifications if it has them, else indications. */
	put16(v, (ch->props & GATT_PROP_NOTIFY) != 0 ? 0x0001 : 0x0002);
	return (gatt_write(c, ch->cccd, v, sizeof(v), done, arg));
}
