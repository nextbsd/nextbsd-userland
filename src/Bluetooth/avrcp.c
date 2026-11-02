/*
 * avrcp.c — AVRCP target for the keys, controller for absolute volume. See
 * avrcp.h.
 *
 * Framing (AVCTP 1.4, §6.1.1; AV/C Digital Interface Command Set):
 *
 *   AVCTP  [label<<4 | packet type<<2 | C/R<<1 | IPID] [PID 0x110E, BE]
 *   AV/C   [ctype or response] [subunit type<<3 | id] [opcode] operands
 *
 * The panel subunit is type 9, id 0 (0x48); the unit is 0xFF. AVRCP's own
 * PDUs ride VENDOR DEPENDENT with the Bluetooth SIG company id 0x001958.
 * Everything we send or expect fits in one AVCTP packet.
 */
#include "avrcp.h"
#include "audio.h"
#include "btcf.h"
#include "btstore.h"
#include "hci_compat.h"
#include "sdp_server.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDUserDevice.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	AVRCP_PID		0x110e

/* AVCTP header bits. */
#define	AVCTP_CR		0x02	/* response */
#define	AVCTP_IPID		0x01	/* invalid profile identifier */
#define	AVCTP_PKT_MASK		0x0c

/* AV/C command types and response codes. */
#define	AVC_CONTROL		0x00
#define	AVC_STATUS		0x01
#define	AVC_NOTIFY		0x03
#define	AVC_NOT_IMPLEMENTED	0x08
#define	AVC_ACCEPTED		0x09
#define	AVC_REJECTED		0x0a
#define	AVC_STABLE		0x0c
#define	AVC_CHANGED		0x0d
#define	AVC_INTERIM		0x0f

#define	AVC_SUBUNIT_PANEL	0x48
#define	AVC_SUBUNIT_UNIT	0xff

#define	AVC_OP_VENDOR		0x00
#define	AVC_OP_UNIT_INFO	0x30
#define	AVC_OP_SUBUNIT_INFO	0x31
#define	AVC_OP_PASS_THROUGH	0x7c

/* PASS THROUGH operation ids. */
#define	PT_VOLUME_UP		0x41
#define	PT_VOLUME_DOWN		0x42
#define	PT_MUTE			0x43
#define	PT_PLAY			0x44
#define	PT_STOP			0x45
#define	PT_PAUSE		0x46
#define	PT_REWIND		0x48
#define	PT_FAST_FORWARD		0x49
#define	PT_FORWARD		0x4b
#define	PT_BACKWARD		0x4c
#define	PT_RELEASED		0x80

/* AVRCP PDUs (VENDOR DEPENDENT) and their parameters. */
#define	PDU_GET_CAPABILITIES	0x10
#define	PDU_REGISTER_NOTIFY	0x31
#define	PDU_SET_ABS_VOLUME	0x50
#define	CAP_COMPANY_ID		0x02
#define	CAP_EVENTS		0x03
#define	EVENT_VOLUME_CHANGED	0x0d

#define	AVRCP_BUF		512

/*
 * A held button is repeated by the controller at least every two seconds
 * (AVRCP 1.6, §4.6.1); one not repeated or released by then is let go, so a
 * lost release never leaves volume-up held down.
 */
#define	KEY_HOLD_MS		2200

/*
 * The Consumer Control collection: nine one-bit buttons and seven bits of
 * padding, no report id. Bit n of the report is usage n below.
 */
static const uint8_t consumer_desc[] = {
	0x05, 0x0c,		/* Usage Page (Consumer) */
	0x09, 0x01,		/* Usage (Consumer Control) */
	0xa1, 0x01,		/* Collection (Application) */
	0x15, 0x00, 0x25, 0x01,	/*   Logical 0..1 */
	0x75, 0x01, 0x95, 0x09,	/*   9 x 1 bit */
	0x09, 0xcd,		/*   Play/Pause */
	0x09, 0xb5,		/*   Scan Next Track */
	0x09, 0xb6,		/*   Scan Previous Track */
	0x09, 0xb7,		/*   Stop */
	0x09, 0xe9,		/*   Volume Increment */
	0x09, 0xea,		/*   Volume Decrement */
	0x09, 0xe2,		/*   Mute */
	0x09, 0xb3,		/*   Fast Forward */
	0x09, 0xb4,		/*   Rewind */
	0x81, 0x02,		/*   Input (Data, Variable, Absolute) */
	0x95, 0x07, 0x81, 0x03,	/*   7 bits of padding */
	0xc0			/* End Collection */
};

/* PASS THROUGH operation -> report bit; -1 for one we do not deliver. */
static int
key_bit(uint8_t op)
{
	switch (op) {
	case PT_PLAY:
	case PT_PAUSE:		return (0);
	case PT_FORWARD:	return (1);
	case PT_BACKWARD:	return (2);
	case PT_STOP:		return (3);
	case PT_VOLUME_UP:	return (4);
	case PT_VOLUME_DOWN:	return (5);
	case PT_MUTE:		return (6);
	case PT_FAST_FORWARD:	return (7);
	case PT_REWIND:		return (8);
	default:		return (-1);
	}
}

struct avrcp_sess {
	bool			used;
	bool			up;		/* channel open */
	bool			connecting;	/* our connect in flight */
	bool			cancelled;	/* ...and no longer wanted */
	struct bt_addr		peer;
	char			addr[BT_ADDR_STRLEN];
	int			fd;
	dispatch_source_t	src;
	IOHIDUserDeviceRef	hid;
	uint16_t		keys;		/* bits held down */
	uint32_t		press_gen;	/* bumped on every press */
	uint8_t			label;		/* our next transaction */
	bool			vol_supported;
};

static struct avrcp_sess	g_sess[AVRCP_MAX];
static int			g_listen = -1;
static dispatch_source_t	g_listen_src;
static dispatch_queue_t		g_hidq;		/* the HID devices' queue */

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[avrcp] ");
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

static struct avrcp_sess *
find(const struct bt_addr *peer)
{
	int i;

	for (i = 0; i < AVRCP_MAX; i++) {
		if (g_sess[i].used && same(&g_sess[i].peer, peer))
			return (&g_sess[i]);
	}
	return (NULL);
}

static struct avrcp_sess *
add(const struct bt_addr *peer)
{
	struct avrcp_sess *s = find(peer);
	int i;

	for (i = 0; s == NULL && i < AVRCP_MAX; i++) {
		if (!g_sess[i].used)
			s = &g_sess[i];
	}
	if (s == NULL || s->used)
		return (s);
	(void)memset(s, 0, sizeof(*s));
	s->used = true;
	s->fd = -1;
	s->peer = *peer;
	s->peer.type = BT_ADDR_PUBLIC;
	(void)bt_addr_ntoa(&s->peer, BT_ADDR_FMT_DISPLAY, s->addr,
	    sizeof(s->addr));
	return (s);
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	return (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0);
}

static void
fill_sockaddr(struct sockaddr_l2cap *sa, const void *bdaddr, uint16_t psm)
{
	(void)memset(sa, 0, sizeof(*sa));
	sa->l2cap_len = sizeof(*sa);
	sa->l2cap_family = AF_BLUETOOTH;
	sa->l2cap_psm = htole16(psm);
	(void)memcpy(&sa->l2cap_bdaddr, bdaddr, sizeof(bdaddr_t));
	sa->l2cap_bdaddr_type = BDADDR_BREDR;
}

/* ---- the media keys ------------------------------------------------------ */

static void
set_num(CFMutableDictionaryRef d, const char *key, long long v)
{
	CFStringRef k = btcf_str(key);

	if (k != NULL) {
		btcf_set_int(d, k, v);
		CFRelease(k);
	}
}

static void
set_str(CFMutableDictionaryRef d, const char *key, const char *v)
{
	CFStringRef k = btcf_str(key);

	if (k != NULL) {
		btcf_set_str(d, k, v);
		CFRelease(k);
	}
}

static void
hid_create(struct avrcp_sess *s)
{
	CFMutableDictionaryRef props;
	CFDataRef data;
	CFStringRef key;
	IOHIDUserDeviceRef dev;
	struct bt_bond *b;
	char serial[BT_ADDR_STRLEN], product[96];

	if (g_hidq == NULL)
		g_hidq = dispatch_queue_create(
		    "org.nextbsd.bluetoothd.avrcp.hid", NULL);
	props = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (props == NULL || g_hidq == NULL) {
		if (props != NULL)
			CFRelease(props);
		return;
	}
	data = CFDataCreate(NULL, consumer_desc, sizeof(consumer_desc));
	key = btcf_str(kIOHIDReportDescriptorKey);
	if (data != NULL && key != NULL)
		CFDictionarySetValue(props, key, data);
	if (data != NULL)
		CFRelease(data);
	if (key != NULL)
		CFRelease(key);

	(void)snprintf(product, sizeof(product), "Bluetooth headset");
	b = calloc(1, sizeof(*b));
	if (b != NULL && bt_store_bond_find(&s->peer, false, b)) {
		if (b->name[0] != '\0')
			(void)snprintf(product, sizeof(product), "%.*s",
			    (int)sizeof(product) - 1, b->name);
		if ((b->flags & BT_BOND_PNP_ID) != 0) {
			set_num(props, kIOHIDVendorIDKey, b->pnp.vendor_id);
			set_num(props, kIOHIDProductIDKey, b->pnp.product_id);
			set_num(props, kIOHIDVersionNumberKey,
			    b->pnp.version);
		}
	}
	free(b);
	set_str(props, kIOHIDTransportKey, kIOHIDTransportBluetoothValue);
	set_str(props, kIOHIDSerialNumberKey, bt_addr_ntoa(&s->peer,
	    BT_ADDR_FMT_KEY, serial, sizeof(serial)));
	set_str(props, kIOHIDProductKey, product);

	dev = IOHIDUserDeviceCreateWithProperties(NULL, props, 0);
	CFRelease(props);
	if (dev == NULL) {
		xlog("%s: cannot create the media-key device (is "
		    "IOHIDUserDevice.kext loadable?)", s->addr);
		return;
	}
	IOHIDUserDeviceSetDispatchQueue(dev, g_hidq);
	IOHIDUserDeviceSetCancelHandler(dev, ^{
		CFRelease(dev);
	});
	IOHIDUserDeviceActivate(dev);
	s->hid = dev;
	s->keys = 0;
}

static void
hid_report(struct avrcp_sess *s)
{
	uint8_t r[2];

	if (s->hid == NULL)
		return;
	r[0] = (uint8_t)(s->keys & 0xff);
	r[1] = (uint8_t)((s->keys >> 8) & 0x01);
	(void)IOHIDUserDeviceHandleReport(s->hid, r, sizeof(r));
}

static void
hid_destroy(struct avrcp_sess *s)
{
	if (s->hid == NULL)
		return;
	if (s->keys != 0) {
		s->keys = 0;
		hid_report(s);		/* nothing stays held */
	}
	IOHIDUserDeviceCancel(s->hid);	/* the cancel handler releases it */
	s->hid = NULL;
}

struct key_tick {
	struct bt_addr	peer;
	uint32_t	gen;
};

static void
key_timeout(void *arg)
{
	struct key_tick *t = arg;
	struct avrcp_sess *s = find(&t->peer);

	if (s != NULL && s->up && s->press_gen == t->gen && s->keys != 0) {
		s->keys = 0;
		hid_report(s);
	}
	free(t);
}

static void
key_pressed(struct avrcp_sess *s)
{
	struct key_tick *t = malloc(sizeof(*t));

	s->press_gen++;
	if (t == NULL)
		return;
	t->peer = s->peer;
	t->gen = s->press_gen;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)KEY_HOLD_MS * 1000000), audio_queue(), t, key_timeout);
}

/* ---- sending ------------------------------------------------------------- */

static int
send_frame(struct avrcp_sess *s, uint8_t label, bool response,
    const uint8_t *frame, size_t len)
{
	uint8_t buf[AVRCP_BUF];

	if (s->fd < 0 || len + 3 > sizeof(buf))
		return (-1);
	buf[0] = (uint8_t)((label << 4) | (response ? AVCTP_CR : 0));
	buf[1] = AVRCP_PID >> 8;
	buf[2] = AVRCP_PID & 0xff;
	(void)memcpy(buf + 3, frame, len);
	return (send(s->fd, buf, len + 3, 0) == (ssize_t)(len + 3) ? 0 : -1);
}

/* A VENDOR DEPENDENT AVRCP PDU. */
static int
send_pdu(struct avrcp_sess *s, uint8_t label, bool response, uint8_t ctype,
    uint8_t pdu, const uint8_t *params, size_t plen)
{
	uint8_t f[AVRCP_BUF];

	if (plen + 10 > sizeof(f))
		return (-1);
	f[0] = ctype;
	f[1] = AVC_SUBUNIT_PANEL;
	f[2] = AVC_OP_VENDOR;
	f[3] = 0x00;			/* Bluetooth SIG: 0x001958 */
	f[4] = 0x19;
	f[5] = 0x58;
	f[6] = pdu;
	f[7] = 0x00;			/* single packet */
	f[8] = (uint8_t)(plen >> 8);
	f[9] = (uint8_t)(plen & 0xff);
	if (plen > 0)
		(void)memcpy(f + 10, params, plen);
	return (send_frame(s, label, response, f, plen + 10));
}

/* As CT: follow the headset's volume. */
static void
register_volume(struct avrcp_sess *s)
{
	uint8_t p[5] = { EVENT_VOLUME_CHANGED, 0, 0, 0, 0 };

	(void)send_pdu(s, s->label, false, AVC_NOTIFY, PDU_REGISTER_NOTIFY,
	    p, sizeof(p));
	s->label = (s->label + 1) & 0x0f;
}

/* ---- receiving ----------------------------------------------------------- */

/* A command from the headset's controller: we are the target. */
static void
command(struct avrcp_sess *s, uint8_t label, uint8_t *f, size_t len)
{
	uint8_t p[8];
	int bit;

	if (len < 3)
		return;
	switch (f[2]) {
	case AVC_OP_UNIT_INFO:
	case AVC_OP_SUBUNIT_INFO:
		f[0] = AVC_STABLE;
		f[1] = AVC_SUBUNIT_UNIT;
		p[0] = 0x07;
		p[1] = AVC_SUBUNIT_PANEL;	/* a panel, id 0 */
		p[2] = p[3] = p[4] = 0xff;	/* no company id */
		(void)memcpy(f + 3, p, 5);
		(void)send_frame(s, label, true, f, 8);
		return;

	case AVC_OP_PASS_THROUGH:
		if (len < 5) {
			f[0] = AVC_REJECTED;
			(void)send_frame(s, label, true, f, len);
			return;
		}
		bit = key_bit(f[3] & 0x7f);
		f[0] = (bit < 0) ? AVC_NOT_IMPLEMENTED : AVC_ACCEPTED;
		(void)send_frame(s, label, true, f, len);
		if (bit < 0)
			return;
		if ((f[3] & PT_RELEASED) != 0)
			s->keys &= (uint16_t)~(1U << bit);
		else {
			s->keys |= (uint16_t)(1U << bit);
			key_pressed(s);
		}
		hid_report(s);
		return;

	case AVC_OP_VENDOR:
		if (len >= 11 && f[3] == 0x00 && f[4] == 0x19 &&
		    f[5] == 0x58 && f[6] == PDU_GET_CAPABILITIES) {
			if (f[10] == CAP_COMPANY_ID) {
				p[0] = CAP_COMPANY_ID;
				p[1] = 1;
				p[2] = 0x00;
				p[3] = 0x19;
				p[4] = 0x58;
				(void)send_pdu(s, label, true, AVC_STABLE,
				    PDU_GET_CAPABILITIES, p, 5);
				return;
			}
			if (f[10] == CAP_EVENTS) {
				p[0] = CAP_EVENTS;
				p[1] = 0;	/* no events to report */
				(void)send_pdu(s, label, true, AVC_STABLE,
				    PDU_GET_CAPABILITIES, p, 2);
				return;
			}
		}
		break;
	default:
		break;
	}
	/* Metadata, browsing and the rest: not here, but keep the link. */
	f[0] = AVC_NOT_IMPLEMENTED;
	(void)send_frame(s, label, true, f, len);
}

/* A response to one of our commands: we are the controller. */
static void
response(struct avrcp_sess *s, const uint8_t *f, size_t len)
{
	uint8_t code;

	if (len < 10 || f[2] != AVC_OP_VENDOR)
		return;
	code = f[0] & 0x0f;
	switch (f[6]) {
	case PDU_REGISTER_NOTIFY:
		if (code == AVC_REJECTED || code == AVC_NOT_IMPLEMENTED) {
			if (s->vol_supported)
				xlog("%s: volume notifications refused",
				    s->addr);
			s->vol_supported = false;
			return;
		}
		if (len < 12 || f[10] != EVENT_VOLUME_CHANGED)
			return;
		if (!s->vol_supported)
			xlog("%s: absolute volume %d", s->addr, f[11] & 0x7f);
		s->vol_supported = true;
		audio_volume_changed(&s->peer, f[11] & 0x7f);
		if (code == AVC_CHANGED)
			register_volume(s);	/* one CHANGED per register */
		return;
	case PDU_SET_ABS_VOLUME:
		if (code == AVC_ACCEPTED && len >= 11)
			audio_volume_changed(&s->peer, f[10] & 0x7f);
		return;
	default:
		return;
	}
}

static void	sess_close(struct avrcp_sess *s, const char *why);

static void
readable(void *arg)
{
	struct avrcp_sess *s = arg;
	uint8_t buf[AVRCP_BUF];
	ssize_t n;
	uint8_t label;

	if (!s->used || !s->up)
		return;
	for (;;) {
		n = recv(s->fd, buf, sizeof(buf), 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (n <= 0) {
			sess_close(s, "the headset closed AVRCP");
			return;
		}
		if (n < 3 || (buf[0] & AVCTP_PKT_MASK) != 0)
			continue;	/* fragments: nothing we use */
		label = buf[0] >> 4;
		if (((buf[1] << 8) | buf[2]) != AVRCP_PID) {
			/* Not AVRCP: say so, as AVCTP requires. */
			if ((buf[0] & AVCTP_CR) == 0) {
				buf[0] = (uint8_t)((label << 4) | AVCTP_CR |
				    AVCTP_IPID);
				(void)send(s->fd, buf, 3, 0);
			}
			continue;
		}
		if ((buf[0] & AVCTP_CR) != 0)
			response(s, buf + 3, (size_t)n - 3);
		else
			command(s, label, buf + 3, (size_t)n - 3);
	}
}

/* ---- sessions -------------------------------------------------------------- */

static void
fd_cancelled(void *arg)
{
	(void)close((int)(intptr_t)arg);
}

static void
sess_close(struct avrcp_sess *s, const char *why)
{
	bool was_up = s->up;

	if (!s->used)
		return;
	if (was_up)
		xlog("%s: AVRCP closed: %s", s->addr, why);
	hid_destroy(s);
	if (s->src != NULL) {
		dispatch_source_cancel(s->src);	/* closes s->fd */
		dispatch_release(s->src);
		s->src = NULL;
	} else if (s->fd >= 0)
		(void)close(s->fd);
	s->fd = -1;
	s->up = false;
	if (s->connecting) {
		s->cancelled = true;	/* connected() frees it */
		return;
	}
	s->used = false;
	if (was_up)
		audio_avrcp_changed(&s->peer, false);
}

/* Take channel `fd` as the session with `peer`. */
static void
adopt(const struct bt_addr *peer, int fd, bool incoming)
{
	struct avrcp_sess *s = add(peer);

	if (s == NULL) {
		(void)close(fd);
		return;
	}
	if (s->up) {
		/* Both sides connected at once: keep the first. */
		(void)close(fd);
		return;
	}
	if (set_nonblock(fd) != 0) {
		(void)close(fd);
		if (!s->connecting)
			s->used = false;
		return;
	}
	s->fd = fd;
	s->src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, audio_queue());
	if (s->src == NULL) {
		(void)close(fd);
		s->fd = -1;
		if (!s->connecting)
			s->used = false;
		return;
	}
	dispatch_set_context(s->src, s);
	dispatch_source_set_event_handler_f(s->src, readable);
	dispatch_source_set_cancel_handler(s->src, ^{
		(void)close(fd);
	});
	s->up = true;
	xlog("%s: AVRCP up (%s)", s->addr, incoming ? "the headset opened it" :
	    "we opened it");
	hid_create(s);
	dispatch_resume(s->src);
	audio_avrcp_changed(&s->peer, true);
	register_volume(s);
}

/* Is `peer` paired? Only a paired device gets a session. */
static bool
paired(const struct bt_addr *peer)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct bt_addr p = *peer;
	bool yes;

	if (b == NULL)
		return (false);
	p.type = BT_ADDR_PUBLIC;
	yes = bt_store_bond_find(&p, false, b);
	free(b);
	return (yes);
}

static void
listen_readable(void *arg)
{
	struct sockaddr_l2cap sa;
	struct bt_addr peer;
	char addr[BT_ADDR_STRLEN];
	socklen_t len = sizeof(sa);
	int fd;

	if (g_listen != (int)(intptr_t)arg)
		return;
	fd = accept(g_listen, (struct sockaddr *)&sa, &len);
	if (fd < 0)
		return;
	(void)memset(&peer, 0, sizeof(peer));
	(void)memcpy(peer.b, &sa.l2cap_bdaddr, BT_ADDR_LEN);
	peer.type = BT_ADDR_PUBLIC;
	if (!paired(&peer)) {
		xlog("%s: AVRCP from a device that is not paired; refused",
		    bt_addr_ntoa(&peer, BT_ADDR_FMT_DISPLAY, addr,
		    sizeof(addr)));
		(void)close(fd);
		return;
	}
	adopt(&peer, fd, true);
}

/* ---- connecting out (a global queue: connect blocks) ----------------------- */

struct conn_job {
	struct bt_addr	peer;
	int		fd;
	int		err;
};

static void
connected(void *arg)
{
	struct conn_job *j = arg;
	struct avrcp_sess *s = find(&j->peer);
	char addr[BT_ADDR_STRLEN];

	if (s != NULL) {
		s->connecting = false;
		if (s->cancelled) {
			s->cancelled = false;
			if (j->fd >= 0)
				(void)close(j->fd);
			if (!s->up)
				s->used = false;
			free(j);
			return;
		}
	}
	if (j->fd < 0) {
		xlog("%s: cannot open AVRCP: %s", bt_addr_ntoa(&j->peer,
		    BT_ADDR_FMT_DISPLAY, addr, sizeof(addr)),
		    strerror(j->err));
		if (s != NULL && !s->up)
			s->used = false;
	} else if (s == NULL)
		(void)close(j->fd);	/* disconnected meanwhile */
	else
		adopt(&j->peer, j->fd, false);
	free(j);
}

static void
connect_job(void *arg)
{
	struct conn_job *j = arg;
	struct sockaddr_l2cap sa;
	struct timeval tv = { AVRCP_CONNECT_MS / 1000, 0 };
	int fd;

	j->fd = -1;
	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0) {
		j->err = errno;
		goto done;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	fill_sockaddr(&sa, NG_HCI_BDADDR_ANY, 0);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		j->err = errno;
		(void)close(fd);
		goto done;
	}
	fill_sockaddr(&sa, j->peer.b, AVCTP_PSM);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		j->err = errno;
		(void)close(fd);
		goto done;
	}
	j->fd = fd;
done:
	dispatch_async_f(audio_queue(), j, connected);
}

void
avrcp_connect(const struct bt_addr *peer)
{
	struct avrcp_sess *s = add(peer);
	struct conn_job *j;

	if (s == NULL || s->up || s->connecting)
		return;
	j = calloc(1, sizeof(*j));
	if (j == NULL) {
		s->used = false;
		return;
	}
	j->peer = s->peer;
	s->connecting = true;
	s->cancelled = false;
	xlog("%s: opening AVRCP", s->addr);
	dispatch_async_f(dispatch_get_global_queue(
	    DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), j, connect_job);
}

void
avrcp_disconnect(const struct bt_addr *peer)
{
	struct avrcp_sess *s = find(peer);

	if (s != NULL)
		sess_close(s, "the link is down");
}

bt_status_t
avrcp_set_volume(const struct bt_addr *peer, int volume)
{
	struct avrcp_sess *s = find(peer);
	uint8_t v = (uint8_t)(volume & 0x7f);

	if (s == NULL || !s->up)
		return (bt_status_not_connected_e);
	if (!s->vol_supported)
		return (bt_status_operation_not_supported_e);
	if (send_pdu(s, s->label, false, AVC_CONTROL, PDU_SET_ABS_VOLUME,
	    &v, 1) != 0)
		return (bt_status_internal_error_e);
	s->label = (s->label + 1) & 0x0f;
	return (bt_status_success_e);
}

/* ---- start --------------------------------------------------------------- */

static void
start_q(void *arg)
{
	struct sockaddr_l2cap sa;
	int fd;

	(void)arg;
	/* We are an A2DP source with an AVRCP target and controller. */
	(void)sdp_server_set_service(SDP_LOCAL_A2DP_SOURCE, true, 0);
	(void)sdp_server_set_service(SDP_LOCAL_AVRCP_TARGET, true, 0);
	(void)sdp_server_set_service(SDP_LOCAL_AVRCP_CONTROLLER, true, 0);

	if (g_listen >= 0)
		return;
	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		return;		/* the L2CAP sockets are not loaded yet */
	fill_sockaddr(&sa, NG_HCI_BDADDR_ANY, AVCTP_PSM);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(fd, 4) != 0 || set_nonblock(fd) != 0) {
		xlog("cannot listen on PSM 0x%02x: %s", AVCTP_PSM,
		    strerror(errno));
		(void)close(fd);
		return;
	}
	g_listen_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, audio_queue());
	if (g_listen_src == NULL) {
		(void)close(fd);
		return;
	}
	g_listen = fd;
	dispatch_set_context(g_listen_src, (void *)(intptr_t)fd);
	dispatch_source_set_event_handler_f(g_listen_src, listen_readable);
	dispatch_source_set_cancel_handler_f(g_listen_src, fd_cancelled);
	dispatch_resume(g_listen_src);
	xlog("listening on PSM 0x%02x (AVRCP)", AVCTP_PSM);
}

void
avrcp_start(void)
{
	dispatch_async_f(audio_queue(), NULL, start_q);
}
