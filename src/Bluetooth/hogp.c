/*
 * hogp.c — HID over GATT (HOGP 1.0, HIDS 1.0) into IOHIDUserDevice (#217).
 *
 * On current macOS an LE keyboard or mouse is an IOHIDUserDevice that a root
 * daemon (BTLEServer) creates; here bluetoothd does the same, and hkbd, hms,
 * hmt or hcons attach to it like any other HID device. Once the link is
 * encrypted:
 *
 *   1. GATT discovery (gatt.c): the HID service(s) 0x1812, Battery 0x180F
 *      and Device Information 0x180A.
 *   2. Each Report 0x2A4D's Report Reference 0x2908: its report ID and type
 *      (input, output, feature).
 *   3. The Report Map 0x2A4B: the HID report descriptor. The first HID
 *      service's map is cached in the bond (HIDDescriptor), so a reconnect
 *      skips the read.
 *   4. PnP ID 0x2A50 for VendorID/ProductID, cached in the bond too.
 *   5. Protocol Mode 0x2A4E := Report, where the device has it.
 *   6. Notifications on every input report (their CCCDs), and on the
 *      Battery Level.
 *   7. One IOHIDUserDevice per HID service, Transport "BluetoothLowEnergy",
 *      SerialNumber the device's address.
 *
 * Input reports arrive as notifications and go to IOHIDUserDeviceHandleReport
 * with the report ID put in front when the descriptor numbers its reports
 * (HOGP strips it on the air). Output reports from the HID drivers (keyboard
 * LEDs) are written with Write Without Response where the characteristic
 * allows it; feature reports with Write Request. Get-report requests read the
 * characteristic.
 *
 * Threads: everything here runs on the main loop with bluetoothd_lock() held,
 * except the IOHIDUserDevice get/set handlers, which run on the HID queue and
 * take the lock themselves. They reach the link through a small context that
 * hogp_stop() detaches under the lock and the device's cancel handler frees.
 * ATT callbacks never carry a pointer that can die: their argument is the
 * HOGP session's token, checked against the link's current session.
 */
#include "le.h"
#include "bluetoothd.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUserDevice.h>
#include <dispatch/dispatch.h>

#include <stdlib.h>
#include <string.h>

#define	HOGP_INST_MAX		4	/* HID services on one device */
#define	HOGP_REPORT_MAX		32
#define	HOGP_GET_TIMEOUT_MS	900	/* under the transport's 1 s default */

/* Report Reference report types. */
#define	RT_INPUT		1
#define	RT_OUTPUT		2
#define	RT_FEATURE		3

struct hogp_report {
	int		inst;
	uint16_t	handle;
	uint16_t	cccd;
	uint16_t	ref;
	uint8_t		props;
	uint8_t		id;
	uint8_t		type;		/* RT_*, 0 unknown */
};

struct hogp_inst;

/* What a device's handlers share with the main loop. */
struct hogp_ctx {
	struct hogp_inst	*inst;	/* NULL once the link is gone */
	IOHIDUserDeviceRef	dev;
	dispatch_semaphore_t	sem;
	bool			waiting;
	bool			done;
	uint8_t			err;
	uint8_t			buf[GATT_VALUE_MAX + 1];
	size_t			len;
};

struct hogp_inst {
	struct hogp		*h;
	int			svc;		/* gatt_db.svc index */
	uint16_t		map_handle;
	uint8_t			map[BT_HID_DESC_MAX];
	size_t			map_len;
	bool			uses_ids;
	IOHIDUserDeviceRef	dev;
	struct hogp_ctx		*ctx;
};

enum {
	S_REFS = 0,
	S_MAPS,
	S_PNP,
	S_SUBSCRIBE,
	S_BATTERY,
	S_BATTERY_SUB,
	S_CREATE,
	S_DONE
};

struct hogp {
	struct le_conn		*c;
	uintptr_t		token;
	int			step;
	int			idx;
	struct hogp_inst	inst[HOGP_INST_MAX];
	int			ninst;
	struct hogp_report	rep[HOGP_REPORT_MAX];
	int			nrep;
	struct bt_pnp_id	pnp;
	bool			have_pnp;
	bool			pnp_new;
	bool			map_new;
	char			name[BT_NAME_MAX + 1];
	int			get_inst;	/* the get-report in flight */
};

static uintptr_t	g_token;
static dispatch_queue_t	g_hid_queue;

static uint16_t
get16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

/* Does the descriptor number its reports (a Report ID item, 0x85)? */
static bool
map_uses_ids(const uint8_t *d, size_t len)
{
	size_t i = 0;

	while (i < len) {
		uint8_t b = d[i];
		size_t size;

		if (b == 0xfe) {		/* long item */
			if (i + 2 >= len)
				break;
			i += 3u + d[i + 1];
			continue;
		}
		size = b & 3;
		if (size == 3)
			size = 4;
		if ((b & 0xfc) == 0x84)
			return (true);
		i += 1 + size;
	}
	return (false);
}

/* The session for an ATT callback, or NULL if it has gone. */
static struct hogp *
session(struct le_conn *c, void *arg)
{
	struct hogp *h = c->hogp;

	return (h != NULL && h->token == (uintptr_t)arg ? h : NULL);
}

static void	step(struct hogp *h);

/* ---- the HID device's handlers (HID queue) ------------------------------------------ */

static struct hogp_report *
find_report(struct hogp_inst *inst, IOHIDReportType type, uint32_t id)
{
	struct hogp *h = inst->h;
	int want = (int)type + 1, i;

	for (i = 0; i < h->nrep; i++) {
		struct hogp_report *r = &h->rep[i];

		if (r->inst == (int)(inst - h->inst) && r->type == want &&
		    (!inst->uses_ids || r->id == id))
			return (r);
	}
	return (NULL);
}

static IOReturn
hid_set(struct hogp_ctx *ctx, IOHIDReportType type, uint32_t id,
    const uint8_t *report, CFIndex len)
{
	struct hogp_inst *inst;
	struct hogp_report *r;
	IOReturn ret = kIOReturnSuccess;
	int rc;

	if (len < 0)
		return (kIOReturnBadArgument);
	bluetoothd_lock();
	inst = ctx->inst;
	if (inst == NULL) {
		bluetoothd_unlock();
		return (kIOReturnOffline);
	}
	r = find_report(inst, type, id);
	if (r == NULL) {
		bluetoothd_unlock();
		return (kIOReturnUnsupported);
	}
	/* The buffer starts with the ID when the descriptor numbers reports. */
	if (inst->uses_ids && len > 0 && report[0] == id) {
		report++;
		len--;
	}
	if (type == kIOHIDReportTypeOutput &&
	    (r->props & GATT_PROP_WRITE_NR) != 0)
		rc = gatt_write_cmd(inst->h->c, r->handle, report,
		    (size_t)len);
	else
		rc = gatt_write(inst->h->c, r->handle, report, (size_t)len,
		    NULL, NULL);
	if (rc != 0)
		ret = kIOReturnIOError;
	bluetoothd_unlock();
	return (ret);
}

static void
get_done(struct le_conn *c, void *arg, uint8_t err, const uint8_t *pdu,
    size_t len)
{
	struct hogp *h = session(c, arg);
	struct hogp_ctx *ctx;

	if (h == NULL || h->get_inst < 0)
		return;
	ctx = h->inst[h->get_inst].ctx;
	h->get_inst = -1;
	if (ctx == NULL || !ctx->waiting)
		return;
	ctx->err = err;
	ctx->len = 0;
	if (err == 0 && len > 1) {
		ctx->len = len - 1;
		if (ctx->len > GATT_VALUE_MAX)
			ctx->len = GATT_VALUE_MAX;
		(void)memcpy(ctx->buf, pdu + 1, ctx->len);
	}
	ctx->done = true;
	dispatch_semaphore_signal(ctx->sem);
}

static IOReturn
hid_get(struct hogp_ctx *ctx, IOHIDReportType type, uint32_t id,
    uint8_t *report, CFIndex *len)
{
	struct hogp_inst *inst;
	struct hogp_report *r;
	IOReturn ret;
	size_t off = 0, n;
	long rc;

	bluetoothd_lock();
	inst = ctx->inst;
	if (inst == NULL) {
		bluetoothd_unlock();
		return (kIOReturnOffline);
	}
	r = find_report(inst, type, id);
	if (r == NULL || (r->props & GATT_PROP_READ) == 0) {
		bluetoothd_unlock();
		return (kIOReturnUnsupported);
	}
	if (ctx->waiting || inst->h->get_inst >= 0) {
		bluetoothd_unlock();
		return (kIOReturnBusy);
	}
	ctx->waiting = true;
	ctx->done = false;
	inst->h->get_inst = (int)(inst - inst->h->inst);
	if (gatt_read(inst->h->c, r->handle, get_done,
	    (void *)inst->h->token) != 0) {
		inst->h->get_inst = -1;
		ctx->waiting = false;
		bluetoothd_unlock();
		return (kIOReturnIOError);
	}
	bluetoothd_unlock();

	rc = dispatch_semaphore_wait(ctx->sem, dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)HOGP_GET_TIMEOUT_MS * 1000000));

	bluetoothd_lock();
	ctx->waiting = false;
	if (!ctx->done) {
		ret = ctx->inst == NULL ? kIOReturnOffline : kIOReturnTimeout;
	} else {
		if (rc != 0)	/* signalled between the timeout and the lock */
			(void)dispatch_semaphore_wait(ctx->sem,
			    DISPATCH_TIME_NOW);
		if (ctx->err != 0) {
			ret = kIOReturnIOError;
		} else {
			if (ctx->inst != NULL && ctx->inst->uses_ids &&
			    *len > 0) {
				report[0] = (uint8_t)id;
				off = 1;
			}
			n = ctx->len;
			if (n > (size_t)*len - off)
				n = (size_t)*len - off;
			(void)memcpy(report + off, ctx->buf, n);
			*len = (CFIndex)(off + n);
			ret = kIOReturnSuccess;
		}
	}
	ctx->done = false;
	bluetoothd_unlock();
	return (ret);
}

/* ---- creating the devices ----------------------------------------------------------- */

static void
dict_int(CFMutableDictionaryRef d, const char *key, int v)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key,
	    kCFStringEncodingUTF8);
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);

	if (k != NULL && n != NULL)
		CFDictionarySetValue(d, k, n);
	if (k != NULL)
		CFRelease(k);
	if (n != NULL)
		CFRelease(n);
}

static void
dict_str(CFMutableDictionaryRef d, const char *key, const char *v)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key,
	    kCFStringEncodingUTF8);
	CFStringRef s = CFStringCreateWithCString(NULL, v,
	    kCFStringEncodingUTF8);

	if (k != NULL && s != NULL)
		CFDictionarySetValue(d, k, s);
	if (k != NULL)
		CFRelease(k);
	if (s != NULL)
		CFRelease(s);
}

static void
create_device(struct hogp *h, struct hogp_inst *inst)
{
	struct le_conn *c = h->c;
	CFMutableDictionaryRef props;
	CFDataRef desc;
	CFStringRef k;
	IOHIDUserDeviceRef dev;
	struct hogp_ctx *ctx;
	char addr[BT_ADDR_STRLEN];

	if (g_hid_queue == NULL)
		g_hid_queue = dispatch_queue_create(
		    "org.nextbsd.bluetoothd.hid", NULL);
	if (g_hid_queue == NULL)
		return;

	props = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (props == NULL)
		return;
	desc = CFDataCreate(NULL, inst->map, (CFIndex)inst->map_len);
	k = CFStringCreateWithCString(NULL, kIOHIDReportDescriptorKey,
	    kCFStringEncodingUTF8);
	if (desc != NULL && k != NULL)
		CFDictionarySetValue(props, k, desc);
	if (desc != NULL)
		CFRelease(desc);
	if (k != NULL)
		CFRelease(k);
	if (h->have_pnp) {
		dict_int(props, kIOHIDVendorIDKey, h->pnp.vendor_id);
		dict_int(props, kIOHIDProductIDKey, h->pnp.product_id);
		dict_int(props, kIOHIDVersionNumberKey, h->pnp.version);
	}
	dict_str(props, kIOHIDTransportKey,
	    kIOHIDTransportBluetoothLowEnergyValue);
	dict_str(props, kIOHIDProductKey, h->name[0] != '\0' ? h->name :
	    "Bluetooth LE device");
	dict_str(props, kIOHIDSerialNumberKey, bt_addr_ntoa(&c->identity,
	    BT_ADDR_FMT_DISPLAY, addr, sizeof(addr)));

	dev = IOHIDUserDeviceCreateWithProperties(NULL, props, 0);
	CFRelease(props);
	if (dev == NULL) {
		le_log(c->e, "%s: cannot create the HID device "
		    "(IOHIDUserDevice.kext?)", addr);
		return;
	}
	ctx = calloc(1, sizeof(*ctx));
	if (ctx != NULL)
		ctx->sem = dispatch_semaphore_create(0);
	if (ctx == NULL || ctx->sem == NULL) {
		free(ctx);
		CFRelease(dev);
		return;
	}
	ctx->inst = inst;
	ctx->dev = dev;

	IOHIDUserDeviceSetDispatchQueue(dev, g_hid_queue);
	IOHIDUserDeviceRegisterGetReportBlock(dev, ^IOReturn(
	    IOHIDReportType type, uint32_t id, uint8_t *report,
	    CFIndex *len) {
		return (hid_get(ctx, type, id, report, len));
	});
	IOHIDUserDeviceRegisterSetReportBlock(dev, ^IOReturn(
	    IOHIDReportType type, uint32_t id, const uint8_t *report,
	    CFIndex len) {
		return (hid_set(ctx, type, id, report, len));
	});
	IOHIDUserDeviceSetCancelHandler(dev, ^{
		CFRelease(ctx->dev);
		dispatch_release(ctx->sem);
		free(ctx);
	});
	inst->dev = dev;
	inst->ctx = ctx;
	/* Handlers first: the HID drivers ask for reports while attaching. */
	IOHIDUserDeviceActivate(dev);
	le_log(c->e, "%s: HID device up (%zu-byte report map%s)", addr,
	    inst->map_len, inst->uses_ids ? ", numbered reports" : "");
}

/* Keep what was read in the bond, so the next connection skips it. */
static void
cache_in_bond(struct hogp *h)
{
	struct le_conn *c = h->c;
	struct bt_event ev;
	struct bt_bond *b;

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return;
	b->addr = c->identity;
	b->transports = BT_TRANSPORT_LE;
	b->profiles = BT_PROFILE_HOGP;
	if (h->map_new && h->ninst > 0 && h->inst[0].map_len > 0) {
		b->flags |= BT_BOND_HID_DESC;
		b->hid_desc_len = (uint16_t)h->inst[0].map_len;
		(void)memcpy(b->hid_desc, h->inst[0].map, h->inst[0].map_len);
	}
	if (h->pnp_new) {
		b->flags |= BT_BOND_PNP_ID;
		b->pnp = h->pnp;
	}
	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_BOND_UPDATED;
	ev.peer = c->identity;
	ev.transport = BT_TRANSPORT_LE;
	ev.u.bond = b;
	le_raise(c->e, &ev);
	free(b);
}

/* ---- the setup sequence --------------------------------------------------------------- */

static void
ref_done(struct le_conn *c, void *arg, uint8_t err, const uint8_t *pdu,
    size_t len)
{
	struct hogp *h = session(c, arg);

	if (h == NULL)
		return;
	if (err == 0 && len >= 3) {
		h->rep[h->idx].id = pdu[1];
		h->rep[h->idx].type = pdu[2];
	}
	h->idx++;
	step(h);
}

static void
map_done(struct le_conn *c, void *arg, uint8_t err, const uint8_t *val,
    size_t len)
{
	struct hogp *h = session(c, arg);
	struct hogp_inst *inst;

	if (h == NULL)
		return;
	inst = &h->inst[h->idx];
	if (err != 0) {
		le_log(c->e, "reading the report map failed (0x%02x)", err);
	} else {
		if (len > sizeof(inst->map))
			len = sizeof(inst->map);
		(void)memcpy(inst->map, val, len);
		inst->map_len = len;
		if (h->idx == 0)
			h->map_new = true;
	}
	h->idx++;
	step(h);
}

static void
pnp_done(struct le_conn *c, void *arg, uint8_t err, const uint8_t *pdu,
    size_t len)
{
	struct hogp *h = session(c, arg);

	if (h == NULL)
		return;
	if (err == 0 && len >= 8) {
		h->pnp.vendor_id_source = pdu[1];
		h->pnp.vendor_id = get16(pdu + 2);
		h->pnp.product_id = get16(pdu + 4);
		h->pnp.version = get16(pdu + 6);
		h->have_pnp = true;
		h->pnp_new = true;
	}
	h->step = S_SUBSCRIBE;
	h->idx = 0;
	step(h);
}

static void
sub_done(struct le_conn *c, void *arg, uint8_t err, const uint8_t *pdu,
    size_t len)
{
	struct hogp *h = session(c, arg);

	(void)pdu;
	(void)len;
	if (h == NULL)
		return;
	if (err != 0)
		le_log(c->e, "enabling notifications on handle 0x%04x "
		    "failed (0x%02x)", h->rep[h->idx].cccd, err);
	h->idx++;
	step(h);
}

static void
battery_done(struct le_conn *c, void *arg, uint8_t err, const uint8_t *pdu,
    size_t len)
{
	struct hogp *h = session(c, arg);

	if (h == NULL)
		return;
	if (err == 0 && len >= 2) {
		c->battery = pdu[1];
		le_log(c->e, "battery %d%%", c->battery);
	}
	h->step = S_BATTERY_SUB;
	step(h);
}

static void
battery_sub_done(struct le_conn *c, void *arg, uint8_t err,
    const uint8_t *pdu, size_t len)
{
	struct hogp *h = session(c, arg);

	(void)err;
	(void)pdu;
	(void)len;
	if (h == NULL)
		return;
	h->step = S_CREATE;
	step(h);
}

static void
step(struct hogp *h)
{
	struct le_conn *c = h->c;
	void *tok = (void *)h->token;
	struct gatt_chr *ch;
	int i;

	for (;;) {
		switch (h->step) {
		case S_REFS:
			while (h->idx < h->nrep && h->rep[h->idx].ref == 0)
				h->idx++;
			if (h->idx < h->nrep) {
				if (gatt_read(c, h->rep[h->idx].ref, ref_done,
				    tok) == 0)
					return;
				h->idx++;
				continue;
			}
			/* No reference: a notifying report is input, ID 0. */
			for (i = 0; i < h->nrep; i++) {
				if (h->rep[i].type == 0 && (h->rep[i].props &
				    GATT_PROP_NOTIFY) != 0)
					h->rep[i].type = RT_INPUT;
			}
			h->step = S_MAPS;
			h->idx = 0;
			continue;

		case S_MAPS:
			while (h->idx < h->ninst &&
			    (h->inst[h->idx].map_len > 0 ||
			    h->inst[h->idx].map_handle == 0))
				h->idx++;	/* cached, or no map */
			if (h->idx < h->ninst) {
				if (gatt_read_long(c,
				    h->inst[h->idx].map_handle, map_done,
				    tok) == 0)
					return;
				h->idx++;
				continue;
			}
			h->step = S_PNP;
			continue;

		case S_PNP:
			/* Report mode, where the device lets us say so. */
			for (ch = NULL; (ch = gatt_find(c, GATT_SVC_HID,
			    GATT_CHR_PROTOCOL_MODE, ch)) != NULL;) {
				uint8_t mode = 1;

				if ((ch->props & GATT_PROP_WRITE_NR) != 0)
					(void)gatt_write_cmd(c, ch->value,
					    &mode, 1);
			}
			/* pnp_done moves on to S_SUBSCRIBE itself. */
			h->step = S_SUBSCRIBE;
			h->idx = 0;
			if (!h->have_pnp && (ch = gatt_find(c,
			    GATT_SVC_DEVICE_INFO, GATT_CHR_PNP_ID,
			    NULL)) != NULL &&
			    gatt_read(c, ch->value, pnp_done, tok) == 0)
				return;
			continue;

		case S_SUBSCRIBE:
			while (h->idx < h->nrep &&
			    (h->rep[h->idx].type != RT_INPUT ||
			    h->rep[h->idx].cccd == 0))
				h->idx++;
			if (h->idx < h->nrep) {
				struct gatt_chr tmp;

				(void)memset(&tmp, 0, sizeof(tmp));
				tmp.cccd = h->rep[h->idx].cccd;
				tmp.props = h->rep[h->idx].props;
				if (gatt_subscribe(c, &tmp, sub_done, tok) == 0)
					return;
				h->idx++;
				continue;
			}
			h->step = S_BATTERY;
			continue;

		case S_BATTERY:
			h->step = S_CREATE;
			ch = gatt_find(c, GATT_SVC_BATTERY,
			    GATT_CHR_BATTERY_LEVEL, NULL);
			if (ch == NULL)
				continue;
			c->battery_handle = ch->value;
			if ((ch->props & GATT_PROP_READ) != 0) {
				h->step = S_BATTERY;
				if (gatt_read(c, ch->value, battery_done,
				    tok) == 0)
					return;
			}
			h->step = S_BATTERY_SUB;
			continue;

		case S_BATTERY_SUB:
			h->step = S_CREATE;
			ch = gatt_find(c, GATT_SVC_BATTERY,
			    GATT_CHR_BATTERY_LEVEL, NULL);
			if (ch != NULL && ch->cccd != 0 &&
			    gatt_subscribe(c, ch, battery_sub_done, tok) == 0) {
				h->step = S_BATTERY_SUB;
				return;
			}
			continue;

		case S_CREATE:
			h->step = S_DONE;
			for (i = 0; i < h->ninst; i++) {
				struct hogp_inst *inst = &h->inst[i];

				if (inst->map_len == 0)
					continue;
				inst->uses_ids = map_uses_ids(inst->map,
				    inst->map_len);
				create_device(h, inst);
			}
			if (h->ninst > 0)
				cache_in_bond(h);
			return;

		default:
			return;
		}
	}
}

static void
discovered(struct le_conn *c, bool ok)
{
	struct hogp *h = c->hogp;
	struct bt_bond *b;
	struct gatt_db *db = &c->db;
	int i, j;

	if (h == NULL)
		return;
	if (!ok) {
		le_log(c->e, "GATT discovery failed; no HID device");
		return;
	}

	for (i = 0; i < db->nsvc && h->ninst < HOGP_INST_MAX; i++) {
		struct hogp_inst *inst;

		if (db->svc[i].uuid != GATT_SVC_HID)
			continue;
		inst = &h->inst[h->ninst++];
		inst->h = h;
		inst->svc = i;
	}
	if (h->ninst == 0) {
		le_log(c->e, "no HID service; nothing to do over GATT");
		h->step = S_BATTERY;
		step(h);
		return;
	}
	for (i = 0; i < db->nchr; i++) {
		struct gatt_chr *ch = &db->chr[i];

		for (j = 0; j < h->ninst; j++) {
			if (h->inst[j].svc == ch->svc)
				break;
		}
		if (j == h->ninst)
			continue;
		if (ch->uuid == GATT_CHR_REPORT_MAP) {
			h->inst[j].map_handle = ch->value;
		} else if (ch->uuid == GATT_CHR_REPORT &&
		    h->nrep < HOGP_REPORT_MAX) {
			struct hogp_report *r = &h->rep[h->nrep++];

			r->inst = j;
			r->handle = ch->value;
			r->cccd = ch->cccd;
			r->ref = ch->report_ref;
			r->props = ch->props;
		}
	}

	/* What an earlier connection cached. */
	b = calloc(1, sizeof(*b));
	if (b != NULL && c->e->host.bond_lookup != NULL &&
	    c->e->host.bond_lookup(c->e->host.ctx, &c->identity, b)) {
		(void)strlcpy(h->name, b->name, sizeof(h->name));
		if ((b->flags & BT_BOND_HID_DESC) != 0 && b->hid_desc_len > 0) {
			(void)memcpy(h->inst[0].map, b->hid_desc,
			    b->hid_desc_len);
			h->inst[0].map_len = b->hid_desc_len;
		}
		if ((b->flags & BT_BOND_PNP_ID) != 0) {
			h->pnp = b->pnp;
			h->have_pnp = true;
		}
	}
	if (b != NULL) {
		explicit_bzero(b, sizeof(*b));
		free(b);
	}
	le_log(c->e, "%d HID service(s), %d report(s)%s", h->ninst, h->nrep,
	    h->inst[0].map_len > 0 ? ", report map cached" : "");
	h->step = S_REFS;
	h->idx = 0;
	step(h);
}

/* ---- the engine's side ------------------------------------------------------------------ */

void
hogp_start(struct le_conn *c)
{
	struct hogp *h;

	if (c->hogp != NULL)
		return;
	h = calloc(1, sizeof(*h));
	if (h == NULL)
		return;
	h->c = c;
	h->token = ++g_token;
	h->get_inst = -1;
	c->hogp = h;
	gatt_discover(c, discovered);
}

void
hogp_notify(struct le_conn *c, uint16_t handle, const uint8_t *v, size_t len)
{
	struct hogp *h = c->hogp;
	uint8_t buf[GATT_VALUE_MAX + 1];
	int i;

	if (handle != 0 && handle == c->battery_handle) {
		if (len >= 1 && c->battery != v[0]) {
			c->battery = v[0];
			le_log(c->e, "battery %d%%", c->battery);
		}
		return;
	}
	if (h == NULL || len > GATT_VALUE_MAX)
		return;
	for (i = 0; i < h->nrep; i++) {
		struct hogp_report *r = &h->rep[i];
		struct hogp_inst *inst;
		size_t off = 0;

		if (r->handle != handle || r->type != RT_INPUT)
			continue;
		inst = &h->inst[r->inst];
		if (inst->dev == NULL)
			return;		/* still setting up */
		if (inst->uses_ids && r->id != 0)
			buf[off++] = r->id;
		(void)memcpy(buf + off, v, len);
		(void)IOHIDUserDeviceHandleReport(inst->dev, buf,
		    (CFIndex)(off + len));
		return;
	}
}

void
hogp_stop(struct le_conn *c)
{
	struct hogp *h = c->hogp;
	int i;

	if (h == NULL)
		return;
	for (i = 0; i < h->ninst; i++) {
		struct hogp_inst *inst = &h->inst[i];

		if (inst->ctx != NULL) {
			inst->ctx->inst = NULL;
			if (inst->ctx->waiting)
				dispatch_semaphore_signal(inst->ctx->sem);
		}
		if (inst->dev != NULL) {
			/* The cancel handler releases it and the context. */
			IOHIDUserDeviceCancel(inst->dev);
			inst->dev = NULL;
		}
	}
	c->hogp = NULL;
	c->battery_handle = 0;
	free(h);
}
