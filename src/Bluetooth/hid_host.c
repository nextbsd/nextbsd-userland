/*
 * hid_host.c — the Classic HID host. See hid_host.h.
 *
 * The HIDP framing is the Bluetooth HID Profile's (v1.1, §3.1): one header
 * octet, transaction type in the high nibble and its parameter in the low,
 * then the payload. bthidd (usr.sbin/bluetooth/bthidd) opened the same two
 * channels; what it did with the reports (parse them with libusbhid, then
 * fake AT scancodes into vkbd and moves into sysmouse) is gone: the reports
 * go to the kernel's HID drivers unchanged, through IOHIDUserDevice, as they
 * do from a USB HID device.
 */
#include "hid_host.h"
#include "adapter.h"
#include "bluetoothd.h"
#include "btcf.h"
#include "btstore.h"
#include "connections.h"
#include "hci_compat.h"
#include "sdp_client.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDUserDevice.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* HIDP transaction types (high nibble). */
#define	HIDP_HANDSHAKE		0x00
#define	HIDP_HID_CONTROL	0x10
#define	HIDP_GET_REPORT		0x40
#define	HIDP_SET_REPORT		0x50
#define	HIDP_SET_PROTOCOL	0x70
#define	HIDP_DATA		0xa0
#define	HIDP_TYPE(h)		((h) & 0xf0)
#define	HIDP_PARAM(h)		((h) & 0x0f)

/* Report types, the parameter of GET/SET_REPORT and DATA. */
#define	HIDP_REPORT_INPUT	0x01
#define	HIDP_REPORT_OUTPUT	0x02
#define	HIDP_REPORT_FEATURE	0x03

/* HANDSHAKE results. */
#define	HIDP_HS_SUCCESSFUL		0x00
#define	HIDP_HS_NOT_READY		0x01
#define	HIDP_HS_INVALID_REPORT_ID	0x02
#define	HIDP_HS_UNSUPPORTED_REQUEST	0x03
#define	HIDP_HS_INVALID_PARAMETER	0x04

/* HID_CONTROL operations. */
#define	HIDP_CTRL_VIRTUAL_CABLE_UNPLUG	0x05

/* SET_PROTOCOL parameter. */
#define	HIDP_PROTOCOL_BOOT	0x00

/* L2CAP's default MTU is 672; read with room to spare. */
#define	HID_BUF			1024

#define	HID_CONNECT_MS		20000	/* page + L2CAP, per channel */
#define	HID_WAIT_INTR_MS	10000	/* control accepted, interrupt due */
#define	HID_WAIT_DESC_MS	15000	/* waiting for the SDP query */
#define	HID_REPLY_MS		1500	/* a GET/SET_REPORT answer */
#define	HID_REQUEST_TIMEOUT_MS	2000	/* the kernel's, a little longer */

/* Boot protocol report ids and lengths (HID Profile, §3.3.2). */
#define	BOOT_ID_KEYBOARD	1
#define	BOOT_ID_MOUSE		2
#define	BOOT_KEYBOARD_LEN	9	/* id + modifiers, reserved, 6 keys */
#define	BOOT_MOUSE_LEN		4	/* id + buttons, x, y */

/*
 * The descriptors a boot-protocol device gets when its own is not known:
 * the HID 1.11 Appendix B boot keyboard and mouse, numbered with the ids the
 * HID Profile gives boot reports.
 */
static const uint8_t boot_keyboard_desc[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, BOOT_ID_KEYBOARD,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,		/* modifiers */
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,		/* reserved */
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
	0x91, 0x02,					/* LEDs */
	0x95, 0x01, 0x75, 0x03, 0x91, 0x01,		/* LED padding */
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,	/* keys */
	0xc0
};

static const uint8_t boot_mouse_desc[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, BOOT_ID_MOUSE,
	0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x03, 0x75, 0x01, 0x81, 0x02,		/* buttons */
	0x95, 0x01, 0x75, 0x05, 0x81, 0x01,		/* padding */
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
	0x75, 0x08, 0x95, 0x02, 0x81, 0x06,		/* x, y */
	0xc0, 0xc0
};

/* Class of Device: major class Peripheral, minor keyboard / pointing bits. */
#define	COD_MAJOR(c)		(((c) >> 8) & 0x1f)
#define	COD_MAJOR_PERIPHERAL	0x05
#define	COD_KEYBOARD		0x40
#define	COD_POINTING		0x80

/*
 * What the device's get/set-report handlers share with the session. The
 * handlers run on the device's queue and wait on `cv` for the answer the
 * session queue reads off the control channel. `ctl` and `intr` are the
 * session's sockets, set to -1 (under `lock`) before they are closed, so a
 * handler never writes to a closed or reused descriptor.
 */
struct hid_dev {
	pthread_mutex_t		lock;
	pthread_cond_t		cv;
	int			ctl;
	int			intr;
	bool			waiting;
	bool			got;
	uint8_t			reply[HID_BUF];
	size_t			reply_len;
	char			addr[BT_ADDR_STRLEN];
	IOHIDUserDeviceRef	dev;
	dispatch_queue_t	q;
	int			refs;	/* the session's and the device's */
};

typedef enum {
	HS_WAIT_INTR = 0,	/* control accepted, waiting for interrupt */
	HS_WAIT_DESC,		/* both up, no report descriptor yet */
	HS_OPEN			/* the IOHIDUserDevice is live */
} hs_state_t;

/* What to do once a session's sockets are closed. */
typedef enum {
	AFTER_NOTHING = 0,
	AFTER_ACL_DISCONNECT,	/* bt_disconnect */
	AFTER_FORGET		/* the device unplugged its virtual cable */
} hs_after_t;

struct hid_session {
	struct hid_session	*next;
	struct bt_addr		peer;
	char			addr[BT_ADDR_STRLEN];	/* for the logs */
	hs_state_t		state;
	uint32_t		gen;		/* matches its timers */
	int			ctl;
	int			intr;
	dispatch_source_t	ctl_src;
	dispatch_source_t	intr_src;
	int			refs;		/* live sources */
	struct hid_dev		*hd;
	bool			boot;		/* boot protocol, own descriptor */
	bool			sdp_asked;
	hs_after_t		after;
};

/* A host-initiated connect, on its own thread until both channels are up. */
struct hid_pending {
	struct hid_pending	*next;
	struct bt_addr		local;
	bool			have_local;
	struct bt_addr		peer;
	bool			cancelled;	/* disconnect, or the peer won */
	bool			then_acl;
	int			ctl;
	int			intr;
	int			err;
};

/* A timer: which session, and which incarnation of it. */
struct hid_tick {
	struct bt_addr		peer;
	uint32_t		gen;
};

static dispatch_queue_t		g_q;
static pthread_mutex_t		g_hlock = PTHREAD_MUTEX_INITIALIZER;
static struct hid_session	*g_sessions;	/* under g_hlock */
static struct hid_pending	*g_pending;	/* under g_hlock */
static uint32_t			g_gen;
static int			g_listen[2] = { -1, -1 };
static dispatch_source_t	g_listen_src[2];

static void	session_close(struct hid_session *s, hs_after_t after,
		    const char *why);
static void	intr_wait_timeout(void *arg);

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[hid] ");
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

/* Lookups; g_hlock held. */
static struct hid_session *
find_session(const struct bt_addr *peer)
{
	struct hid_session *s;

	for (s = g_sessions; s != NULL; s = s->next) {
		if (same(&s->peer, peer))
			return (s);
	}
	return (NULL);
}

static struct hid_pending *
find_pending(const struct bt_addr *peer)
{
	struct hid_pending *p;

	for (p = g_pending; p != NULL; p = p->next) {
		if (same(&p->peer, peer) && !p->cancelled)
			return (p);
	}
	return (NULL);
}

/* The bond of `peer`, metadata only, on the heap; NULL if not paired. */
static struct bt_bond *
bond_copy(const struct bt_addr *peer)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct bt_addr p = *peer;

	if (b == NULL)
		return (NULL);
	p.type = BT_ADDR_PUBLIC;
	if (!bt_store_bond_find(&p, false, b)) {
		free(b);
		return (NULL);
	}
	return (b);
}

/* Tell the policy layer the HID profile is up or down. Lock NOT held. */
static void
report_profile(const struct bt_addr *peer, bool up)
{
	bluetoothd_lock();
	conn_profile_locked(peer, BT_PROFILE_HID, up);
	bluetoothd_unlock();
	bluetoothd_wake();
}

static void
fill_sockaddr(struct sockaddr_l2cap *sa, const uint8_t *bdaddr, uint16_t psm)
{
	(void)memset(sa, 0, sizeof(*sa));
	sa->l2cap_len = sizeof(*sa);
	sa->l2cap_family = AF_BLUETOOTH;
	sa->l2cap_psm = htole16(psm);
	(void)memcpy(&sa->l2cap_bdaddr, bdaddr, sizeof(bdaddr_t));
	sa->l2cap_bdaddr_type = BDADDR_BREDR;
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	return (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0);
}

static void
tick_after(struct hid_session *s, int ms, dispatch_function_t fn)
{
	struct hid_tick *t = malloc(sizeof(*t));

	if (t == NULL)
		return;
	t->peer = s->peer;
	t->gen = s->gen;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)ms * 1000000), g_q, t, fn);
}

/* The session a timer is for, if it is still the same one. */
static struct hid_session *
tick_session(struct hid_tick *t)
{
	struct hid_session *s;

	(void)pthread_mutex_lock(&g_hlock);
	s = find_session(&t->peer);
	if (s != NULL && s->gen != t->gen)
		s = NULL;
	(void)pthread_mutex_unlock(&g_hlock);
	free(t);
	return (s);
}

/* ---- the device's report handlers (its own queue) ------------------------- */

static uint8_t
hidp_report_type(IOHIDReportType type)
{
	switch (type) {
	case kIOHIDReportTypeInput:	return (HIDP_REPORT_INPUT);
	case kIOHIDReportTypeOutput:	return (HIDP_REPORT_OUTPUT);
	default:			return (HIDP_REPORT_FEATURE);
	}
}

static IOReturn
handshake_ioreturn(uint8_t result)
{
	switch (result) {
	case HIDP_HS_SUCCESSFUL:		return (kIOReturnSuccess);
	case HIDP_HS_NOT_READY:			return (kIOReturnNotReady);
	case HIDP_HS_INVALID_REPORT_ID:
	case HIDP_HS_INVALID_PARAMETER:		return (kIOReturnBadArgument);
	case HIDP_HS_UNSUPPORTED_REQUEST:	return (kIOReturnUnsupported);
	default:				return (kIOReturnError);
	}
}

/*
 * Send one request on the control channel and wait for its answer: a
 * HANDSHAKE, or for GET_REPORT the DATA. hd->lock held; returns with it held.
 */
static IOReturn
transact(struct hid_dev *hd, const uint8_t *msg, size_t len)
{
	struct timespec dl;

	if (hd->ctl < 0)
		return (kIOReturnOffline);
	hd->waiting = true;
	hd->got = false;
	hd->reply_len = 0;
	if (send(hd->ctl, msg, len, 0) < 0) {
		hd->waiting = false;
		return (kIOReturnIOError);
	}
	(void)clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += HID_REPLY_MS / 1000;
	dl.tv_nsec += (long)(HID_REPLY_MS % 1000) * 1000000;
	if (dl.tv_nsec >= 1000000000) {
		dl.tv_sec++;
		dl.tv_nsec -= 1000000000;
	}
	while (!hd->got && hd->ctl >= 0) {
		if (pthread_cond_timedwait(&hd->cv, &hd->lock, &dl) ==
		    ETIMEDOUT)
			break;
	}
	hd->waiting = false;
	if (!hd->got)
		return (hd->ctl < 0 ? kIOReturnOffline : kIOReturnTimeout);
	return (kIOReturnSuccess);
}

static IOReturn
get_report(void *refcon, IOHIDReportType type, uint32_t reportID,
    uint8_t *report, CFIndex *reportLength)
{
	struct hid_dev *hd = refcon;
	uint8_t msg[2];
	size_t mlen = 1, n;
	IOReturn ret;

	msg[0] = HIDP_GET_REPORT | hidp_report_type(type);
	if (reportID != 0)
		msg[mlen++] = (uint8_t)reportID;
	(void)pthread_mutex_lock(&hd->lock);
	ret = transact(hd, msg, mlen);
	if (ret == kIOReturnSuccess && HIDP_TYPE(hd->reply[0]) ==
	    HIDP_HANDSHAKE) {
		/* An error; a "success" with no report is one too. */
		ret = handshake_ioreturn(HIDP_PARAM(hd->reply[0]));
		if (ret == kIOReturnSuccess)
			ret = kIOReturnError;
	} else if (ret == kIOReturnSuccess &&
	    HIDP_TYPE(hd->reply[0]) != HIDP_DATA)
		ret = kIOReturnError;
	if (ret == kIOReturnSuccess) {
		n = hd->reply_len - 1;
		if ((CFIndex)n > *reportLength)
			n = (size_t)*reportLength;
		(void)memcpy(report, hd->reply + 1, n);
		*reportLength = (CFIndex)n;
	} else
		*reportLength = 0;
	(void)pthread_mutex_unlock(&hd->lock);
	return (ret);
}

static IOReturn
set_report(void *refcon, IOHIDReportType type, uint32_t reportID,
    uint8_t *report, CFIndex reportLength)
{
	struct hid_dev *hd = refcon;
	uint8_t msg[HID_BUF];
	IOReturn ret;

	(void)reportID;		/* already the first byte of a numbered report */
	if (reportLength < 0 || (size_t)reportLength + 1 > sizeof(msg))
		return (kIOReturnBadArgument);
	(void)memcpy(msg + 1, report, (size_t)reportLength);

	(void)pthread_mutex_lock(&hd->lock);
	if (type == kIOHIDReportTypeOutput) {
		/* LEDs and the like: a DATA frame on interrupt, no answer. */
		msg[0] = HIDP_DATA | HIDP_REPORT_OUTPUT;
		if (hd->intr < 0)
			ret = kIOReturnOffline;
		else if (send(hd->intr, msg, (size_t)reportLength + 1, 0) < 0)
			ret = kIOReturnIOError;
		else
			ret = kIOReturnSuccess;
		(void)pthread_mutex_unlock(&hd->lock);
		return (ret);
	}
	msg[0] = HIDP_SET_REPORT | hidp_report_type(type);
	ret = transact(hd, msg, (size_t)reportLength + 1);
	if (ret == kIOReturnSuccess)
		ret = HIDP_TYPE(hd->reply[0]) == HIDP_HANDSHAKE ?
		    handshake_ioreturn(HIDP_PARAM(hd->reply[0])) :
		    kIOReturnError;
	(void)pthread_mutex_unlock(&hd->lock);
	return (ret);
}

static void
hid_dev_free(struct hid_dev *hd)
{
	(void)pthread_cond_destroy(&hd->cv);
	(void)pthread_mutex_destroy(&hd->lock);
	free(hd);
}

/*
 * Drop one of the two references: the session's (it closed) or the
 * device's (its cancel handler ran; the device may also cancel itself, if
 * the kext goes away). The last one releases the device and its queue.
 */
static void
hid_dev_put(struct hid_dev *hd)
{
	dispatch_queue_t q;
	int left;

	(void)pthread_mutex_lock(&hd->lock);
	left = --hd->refs;
	(void)pthread_mutex_unlock(&hd->lock);
	if (left > 0)
		return;
	q = hd->q;
	CFRelease(hd->dev);
	hid_dev_free(hd);
	dispatch_release(q);
}

/* ---- the IOHIDUserDevice -------------------------------------------------- */

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

/*
 * Create and activate the device for session `s` from `desc`. The queue and
 * both handlers are in place before Activate: the drivers ask for reports
 * while they attach. Session queue.
 */
static bool
create_device(struct hid_session *s, const struct bt_bond *b,
    const uint8_t *desc, size_t desc_len)
{
	CFMutableDictionaryRef props;
	CFDataRef data;
	CFStringRef key;
	struct hid_dev *hd;
	IOHIDUserDeviceRef dev;
	char serial[BT_ADDR_STRLEN];

	props = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (props == NULL)
		return (false);
	data = CFDataCreate(NULL, desc, (CFIndex)desc_len);
	key = btcf_str(kIOHIDReportDescriptorKey);
	if (data != NULL && key != NULL)
		CFDictionarySetValue(props, key, data);
	if (data != NULL)
		CFRelease(data);
	if (key != NULL)
		CFRelease(key);
	if ((b->flags & BT_BOND_PNP_ID) != 0) {
		set_num(props, kIOHIDVendorIDKey, b->pnp.vendor_id);
		set_num(props, kIOHIDVendorIDSourceKey,
		    b->pnp.vendor_id_source);
		set_num(props, kIOHIDProductIDKey, b->pnp.product_id);
		set_num(props, kIOHIDVersionNumberKey, b->pnp.version);
	}
	if ((b->flags & BT_BOND_HID_ATTRS) != 0)
		set_num(props, kIOHIDCountryCodeKey, b->hid_country_code);
	set_str(props, kIOHIDTransportKey, kIOHIDTransportBluetoothValue);
	set_str(props, kIOHIDSerialNumberKey, bt_addr_ntoa(&b->addr,
	    BT_ADDR_FMT_KEY, serial, sizeof(serial)));
	set_str(props, kIOHIDProductKey, b->name[0] != '\0' ? b->name :
	    "Bluetooth HID device");
	set_num(props, kIOHIDRequestTimeoutKey, HID_REQUEST_TIMEOUT_MS);

	dev = IOHIDUserDeviceCreateWithProperties(NULL, props, 0);
	CFRelease(props);
	if (dev == NULL) {
		xlog("%s: cannot create the HID device (is "
		    "IOHIDUserDevice.kext loadable?)", s->addr);
		return (false);
	}

	hd = calloc(1, sizeof(*hd));
	if (hd == NULL) {
		CFRelease(dev);
		return (false);
	}
	(void)pthread_mutex_init(&hd->lock, NULL);
	(void)pthread_cond_init(&hd->cv, NULL);
	hd->ctl = s->ctl;
	hd->intr = s->intr;
	hd->dev = dev;
	hd->refs = 2;
	(void)strlcpy(hd->addr, s->addr, sizeof(hd->addr));
	hd->q = dispatch_queue_create("org.nextbsd.bluetoothd.hid.device",
	    NULL);
	if (hd->q == NULL) {
		CFRelease(dev);
		hid_dev_free(hd);
		return (false);
	}

	IOHIDUserDeviceSetDispatchQueue(dev, hd->q);
	IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback(dev,
	    get_report, hd);
	IOHIDUserDeviceRegisterSetReportCallback(dev, set_report, hd);
	IOHIDUserDeviceSetCancelHandler(dev, ^{
		/* No report handler can run any more. */
		hid_dev_put(hd);
	});
	IOHIDUserDeviceActivate(dev);

	s->hd = hd;
	s->state = HS_OPEN;
	return (true);
}

/*
 * The first session of a bond is remembered, so later power-ups leave a
 * device that reconnects itself (HIDReconnectInitiate) to do so.
 */
static void
mark_established(const struct bt_addr *peer)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct bt_addr p = *peer;

	if (b == NULL)
		return;
	p.type = BT_ADDR_PUBLIC;
	bluetoothd_lock();
	if (bt_store_bond_find(&p, true, b) &&
	    (b->hid_flags & BT_HID_SESSION_ESTABLISHED) == 0) {
		b->hid_flags |= BT_HID_SESSION_ESTABLISHED;
		b->last_connected = time(NULL);
		(void)bt_store_bond_save(b);
	}
	bluetoothd_unlock();
	(void)memset(b, 0, sizeof(*b));
	free(b);
}

/* Can a device with this bond be driven in boot protocol? */
static bool
bootable(const struct bt_bond *b)
{
	uint32_t c = b->class_of_device;

	if ((b->flags & BT_BOND_HID_ATTRS) != 0 &&
	    (b->hid_flags & BT_HID_BOOT_DEVICE) != 0)
		return (true);
	return (COD_MAJOR(c) == COD_MAJOR_PERIPHERAL &&
	    (c & (COD_KEYBOARD | COD_POINTING)) != 0);
}

/* Switch to boot protocol and drive the device with a built-in descriptor. */
static bool
open_boot(struct hid_session *s, const struct bt_bond *b)
{
	uint8_t desc[sizeof(boot_keyboard_desc) + sizeof(boot_mouse_desc)];
	uint8_t msg = HIDP_SET_PROTOCOL | HIDP_PROTOCOL_BOOT;
	uint32_t c = b->class_of_device;
	bool kbd, mouse;
	size_t len = 0;

	kbd = (c & COD_KEYBOARD) != 0;
	mouse = (c & COD_POINTING) != 0;
	if (!kbd && !mouse)
		kbd = mouse = true;	/* a boot device of unknown kind */
	if (kbd) {
		(void)memcpy(desc + len, boot_keyboard_desc,
		    sizeof(boot_keyboard_desc));
		len += sizeof(boot_keyboard_desc);
	}
	if (mouse) {
		(void)memcpy(desc + len, boot_mouse_desc,
		    sizeof(boot_mouse_desc));
		len += sizeof(boot_mouse_desc);
	}
	/* The HANDSHAKE is read (and a refusal logged) by the control reader. */
	if (send(s->ctl, &msg, 1, 0) < 0)
		xlog("%s: SET_PROTOCOL(boot): %s", s->addr, strerror(errno));
	s->boot = true;
	xlog("%s: no report descriptor; using the boot protocol (%s%s%s)",
	    s->addr, kbd ? "keyboard" : "", kbd && mouse ? ", " : "",
	    mouse ? "mouse" : "");
	return (create_device(s, b, desc, len));
}

static void
desc_timeout(void *arg)
{
	struct hid_session *s = tick_session(arg);
	struct bt_bond *b;

	if (s == NULL || s->state != HS_WAIT_DESC)
		return;
	b = bond_copy(&s->peer);
	if (b != NULL && bootable(b) && open_boot(s, b)) {
		free(b);
		report_profile(&s->peer, true);
		mark_established(&s->peer);
		return;
	}
	free(b);
	session_close(s, AFTER_NOTHING, "no report descriptor");
}

/* Both channels are up: make the device, or wait for its descriptor. */
static void
session_up(struct hid_session *s)
{
	struct bt_bond *b = bond_copy(&s->peer);

	if (b == NULL) {
		session_close(s, AFTER_NOTHING, "no longer paired");
		return;
	}
	if ((b->flags & BT_BOND_HID_DESC) != 0 && b->hid_desc_len > 0) {
		if (!create_device(s, b, b->hid_desc, b->hid_desc_len)) {
			free(b);
			session_close(s, AFTER_NOTHING, "no HID device");
			return;
		}
		free(b);
		xlog("%s: HID session up", s->addr);
		report_profile(&s->peer, true);
		mark_established(&s->peer);
		return;
	}
	free(b);
	/* Too early: the pair-time SDP query has not answered yet. */
	s->state = HS_WAIT_DESC;
	if (!s->sdp_asked) {
		s->sdp_asked = true;
		(void)sdp_client_refresh_bond(&s->peer);
	}
	xlog("%s: waiting for the report descriptor", s->addr);
	tick_after(s, HID_WAIT_DESC_MS, desc_timeout);
}

/* ---- the channels (session queue) ----------------------------------------- */

static void
forget_device(const struct bt_addr *peer)
{
	struct bt_addr p = *peer;

	p.type = BT_ADDR_PUBLIC;
	bluetoothd_lock();
	(void)adapter_unpair_locked(&p);
	bluetoothd_unlock();
}

/* A source is gone; the last one closes the session for good. */
static void
source_released(struct hid_session *s)
{
	struct bt_addr peer = s->peer;
	hs_after_t after = s->after;

	if (--s->refs > 0)
		return;
	free(s);
	if (after == AFTER_ACL_DISCONNECT) {
		bluetoothd_lock();
		conn_acl_disconnect_locked(&peer);
		bluetoothd_unlock();
	} else if (after == AFTER_FORGET)
		forget_device(&peer);
}

static void
intr_cancelled(void *arg)
{
	struct hid_session *s = arg;

	if (s->intr >= 0)
		(void)close(s->intr);
	s->intr = -1;
	source_released(s);
}

static void
ctl_cancelled(void *arg)
{
	struct hid_session *s = arg;

	if (s->ctl >= 0)
		(void)close(s->ctl);
	s->ctl = -1;
	source_released(s);
}

/*
 * End a session: take it off the list (no one else can reach its sockets
 * now), stop its handlers using them, cancel its device, then close interrupt
 * and control in that order.
 */
static void
session_close(struct hid_session *s, hs_after_t after, const char *why)
{
	struct hid_session **pp;

	(void)pthread_mutex_lock(&g_hlock);
	for (pp = &g_sessions; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_hlock);

	xlog("%s: HID session closed (%s)", s->addr, why);
	if (s->hd != NULL) {
		(void)pthread_mutex_lock(&s->hd->lock);
		s->hd->ctl = s->hd->intr = -1;
		(void)pthread_cond_broadcast(&s->hd->cv);
		(void)pthread_mutex_unlock(&s->hd->lock);
		/* The kernel device goes, and with it any key held down. */
		IOHIDUserDeviceCancel(s->hd->dev);
		hid_dev_put(s->hd);
		s->hd = NULL;
	}
	if (s->after == AFTER_NOTHING)
		s->after = after;

	/* Also ends "connecting" for a session that never came up. */
	report_profile(&s->peer, false);

	/* Sockets with no source are closed here, the rest by their source. */
	if (s->intr_src == NULL && s->intr >= 0) {
		(void)close(s->intr);
		s->intr = -1;
	}
	if (s->ctl_src == NULL && s->ctl >= 0) {
		(void)close(s->ctl);
		s->ctl = -1;
	}
	if (s->refs == 0) {
		s->refs = 1;
		source_released(s);
		return;
	}
	if (s->intr_src != NULL) {
		dispatch_source_cancel(s->intr_src);
		dispatch_release(s->intr_src);
		s->intr_src = NULL;
	}
	if (s->ctl_src != NULL) {
		dispatch_source_cancel(s->ctl_src);
		dispatch_release(s->ctl_src);
		s->ctl_src = NULL;
	}
}

static void
ctl_readable(void *arg)
{
	struct hid_session *s = arg;
	uint8_t buf[HID_BUF];
	ssize_t n;

	n = recv(s->ctl, buf, sizeof(buf), 0);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		session_close(s, AFTER_NOTHING, n == 0 ?
		    "control channel closed" : strerror(errno));
		return;
	}

	switch (HIDP_TYPE(buf[0])) {
	case HIDP_HANDSHAKE:
	case HIDP_DATA:
		if (s->hd != NULL) {
			struct hid_dev *hd = s->hd;

			(void)pthread_mutex_lock(&hd->lock);
			if (hd->waiting && !hd->got) {
				(void)memcpy(hd->reply, buf, (size_t)n);
				hd->reply_len = (size_t)n;
				hd->got = true;
				(void)pthread_cond_broadcast(&hd->cv);
			}
			(void)pthread_mutex_unlock(&hd->lock);
		}
		if (HIDP_TYPE(buf[0]) == HIDP_HANDSHAKE &&
		    HIDP_PARAM(buf[0]) != HIDP_HS_SUCCESSFUL && s->boot)
			xlog("%s: HANDSHAKE 0x%x", s->addr,
			    HIDP_PARAM(buf[0]));
		break;
	case HIDP_HID_CONTROL:
		if (HIDP_PARAM(buf[0]) == HIDP_CTRL_VIRTUAL_CABLE_UNPLUG) {
			xlog("%s: the device unplugged its virtual cable; "
			    "forgetting it", s->addr);
			session_close(s, AFTER_FORGET, "virtual cable unplug");
		}
		/* SUSPEND / EXIT_SUSPEND and the rest need nothing. */
		break;
	default: {
		/* A request from the device: the host supports none. */
		uint8_t hs = HIDP_HANDSHAKE | HIDP_HS_UNSUPPORTED_REQUEST;

		(void)send(s->ctl, &hs, 1, 0);
		break;
	}
	}
}

static void
intr_readable(void *arg)
{
	struct hid_session *s = arg;
	uint8_t buf[HID_BUF];
	ssize_t n;
	size_t len;

	n = recv(s->intr, buf, sizeof(buf), 0);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		session_close(s, AFTER_NOTHING, n == 0 ?
		    "interrupt channel closed" : strerror(errno));
		return;
	}
	if (buf[0] != (HIDP_DATA | HIDP_REPORT_INPUT) || n < 2 ||
	    s->hd == NULL)
		return;
	len = (size_t)n - 1;
	if (s->boot) {
		/* Boot reports may carry more than the boot layout. */
		if (buf[1] == BOOT_ID_KEYBOARD && len > BOOT_KEYBOARD_LEN)
			len = BOOT_KEYBOARD_LEN;
		else if (buf[1] == BOOT_ID_MOUSE && len > BOOT_MOUSE_LEN)
			len = BOOT_MOUSE_LEN;
	}
	(void)IOHIDUserDeviceHandleReport(s->hd->dev, buf + 1, (CFIndex)len);
}

static dispatch_source_t
start_source(struct hid_session *s, int fd, dispatch_function_t readable,
    dispatch_function_t cancelled)
{
	dispatch_source_t src;

	src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, g_q);
	if (src == NULL)
		return (NULL);
	dispatch_set_context(src, s);
	dispatch_source_set_event_handler_f(src, readable);
	dispatch_source_set_cancel_handler_f(src, cancelled);
	s->refs++;
	dispatch_resume(src);
	return (src);
}

/* A new session on the list; g_hlock NOT held. Session queue. */
static struct hid_session *
session_new(const struct bt_addr *peer, int ctl, int intr)
{
	struct hid_session *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return (NULL);
	s->peer = *peer;
	s->peer.type = BT_ADDR_PUBLIC;
	(void)bt_addr_ntoa(&s->peer, BT_ADDR_FMT_DISPLAY, s->addr,
	    sizeof(s->addr));
	s->ctl = ctl;
	s->intr = intr;
	s->state = HS_WAIT_INTR;
	s->gen = ++g_gen;

	(void)pthread_mutex_lock(&g_hlock);
	s->next = g_sessions;
	g_sessions = s;
	(void)pthread_mutex_unlock(&g_hlock);

	s->ctl_src = start_source(s, ctl, ctl_readable, ctl_cancelled);
	if (intr >= 0)
		s->intr_src = start_source(s, intr, intr_readable,
		    intr_cancelled);
	if (s->ctl_src == NULL || (intr >= 0 && s->intr_src == NULL)) {
		session_close(s, AFTER_NOTHING, "no dispatch source");
		return (NULL);
	}
	return (s);
}

/* ---- host-initiated connect (its own thread) ------------------------------ */

/* Open one channel to `peer`; blocks for at most HID_CONNECT_MS. */
static int
l2cap_connect(const struct hid_pending *p, uint16_t psm, int *out)
{
	struct sockaddr_l2cap sa;
	struct pollfd pfd;
	socklen_t len;
	int fd, err = 0, rc;

	*out = -1;
	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		return (errno);
	fill_sockaddr(&sa, p->have_local ? p->local.b :
	    (const uint8_t *)NG_HCI_BDADDR_ANY, 0);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    set_nonblock(fd) != 0) {
		err = errno;
		goto fail;
	}
	fill_sockaddr(&sa, p->peer.b, psm);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		if (errno != EINPROGRESS) {
			err = errno;
			goto fail;
		}
		pfd.fd = fd;
		pfd.events = POLLOUT;
		do {
			pfd.revents = 0;
			rc = poll(&pfd, 1, HID_CONNECT_MS);
		} while (rc < 0 && errno == EINTR);
		if (rc <= 0) {
			err = rc == 0 ? ETIMEDOUT : errno;
			goto fail;
		}
		len = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
			err = errno;
		if (err != 0)
			goto fail;
	}
	*out = fd;
	return (0);
fail:
	(void)close(fd);
	return (err != 0 ? err : EIO);
}

/* The connect thread is done: adopt its channels. Session queue. */
static void
adopt_pending(void *arg)
{
	struct hid_pending *p = arg, **pp;
	struct hid_session *s;
	char addr[BT_ADDR_STRLEN];
	bool cancelled, superseded;

	(void)bt_addr_ntoa(&p->peer, BT_ADDR_FMT_DISPLAY, addr, sizeof(addr));
	(void)pthread_mutex_lock(&g_hlock);
	for (pp = &g_pending; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == p) {
			*pp = p->next;
			break;
		}
	}
	cancelled = p->cancelled;
	superseded = (find_session(&p->peer) != NULL);
	(void)pthread_mutex_unlock(&g_hlock);

	if (p->err != 0 || cancelled || superseded) {
		if (p->intr >= 0)
			(void)close(p->intr);
		if (p->ctl >= 0)
			(void)close(p->ctl);
		if (p->err != 0)
			xlog("%s: HID connect failed: %s", addr,
			    strerror(p->err));
		if (!superseded)
			report_profile(&p->peer, false);
		if (cancelled && p->then_acl) {
			bluetoothd_lock();
			conn_acl_disconnect_locked(&p->peer);
			bluetoothd_unlock();
		}
		free(p);
		return;
	}
	s = session_new(&p->peer, p->ctl, p->intr);
	free(p);
	if (s != NULL)
		session_up(s);
}

static void *
connect_thread(void *arg)
{
	struct hid_pending *p = arg;

	p->err = l2cap_connect(p, HID_PSM_CONTROL, &p->ctl);
	if (p->err == 0) {
		p->err = l2cap_connect(p, HID_PSM_INTERRUPT, &p->intr);
		if (p->err != 0) {
			(void)close(p->ctl);
			p->ctl = -1;
		}
	}
	dispatch_async_f(g_q, p, adopt_pending);
	return (NULL);
}

/* ---- device-initiated connect (listeners, session queue) ------------------ */

static void
accepted(int psm, int fd, const struct bt_addr *peer)
{
	struct hid_session *s;
	struct hid_pending *p;
	struct bt_bond *b;
	char addr[BT_ADDR_STRLEN];

	(void)bt_addr_ntoa(peer, BT_ADDR_FMT_DISPLAY, addr, sizeof(addr));
	b = bond_copy(peer);
	if (b == NULL) {
		xlog("%s: refusing a HID channel: not paired", addr);
		(void)close(fd);
		return;
	}
	free(b);

	(void)pthread_mutex_lock(&g_hlock);
	s = find_session(peer);
	/* The device paged us while we paged it: its channels win. */
	if (psm == HID_PSM_CONTROL && (p = find_pending(peer)) != NULL)
		p->cancelled = true;
	(void)pthread_mutex_unlock(&g_hlock);

	if (psm == HID_PSM_CONTROL) {
		/*
		 * A device opens one control channel at a time: a new one
		 * means the old session is dead and we have not noticed yet
		 * (it went out of range, or restarted).
		 */
		if (s != NULL)
			session_close(s, AFTER_NOTHING, "replaced");
		s = session_new(peer, fd, -1);
		if (s != NULL)
			tick_after(s, HID_WAIT_INTR_MS, intr_wait_timeout);
		return;
	}

	/* Interrupt: only after its control channel. */
	if (s == NULL || s->state != HS_WAIT_INTR || s->intr >= 0) {
		xlog("%s: HID interrupt channel without control; closing it",
		    addr);
		(void)close(fd);
		return;
	}
	s->intr = fd;
	s->intr_src = start_source(s, fd, intr_readable, intr_cancelled);
	if (s->intr_src == NULL) {
		s->intr = -1;
		(void)close(fd);
		session_close(s, AFTER_NOTHING, "no dispatch source");
		return;
	}
	xlog("%s: the device reconnected", addr);
	session_up(s);
}

static void
intr_wait_timeout(void *arg)
{
	struct hid_session *s = tick_session(arg);

	if (s != NULL && s->state == HS_WAIT_INTR && s->intr < 0)
		session_close(s, AFTER_NOTHING, "no interrupt channel");
}

static void
listen_readable(void *arg)
{
	int psm = (int)(intptr_t)arg;
	int lfd = g_listen[psm == HID_PSM_CONTROL ? 0 : 1];
	struct sockaddr_l2cap sa;
	struct bt_addr peer;
	socklen_t len = sizeof(sa);
	int fd;

	fd = accept(lfd, (struct sockaddr *)&sa, &len);
	if (fd < 0)
		return;
	if (set_nonblock(fd) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
		(void)close(fd);
		return;
	}
	(void)memcpy(peer.b, &sa.l2cap_bdaddr, BT_ADDR_LEN);
	peer.type = BT_ADDR_PUBLIC;
	accepted(psm, fd, &peer);
}

static int
listen_on(uint16_t psm)
{
	struct sockaddr_l2cap sa;
	int fd;

	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		return (-1);
	fill_sockaddr(&sa, (const uint8_t *)NG_HCI_BDADDR_ANY, psm);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(fd, 8) != 0 || set_nonblock(fd) != 0) {
		xlog("cannot listen on PSM 0x%02x: %s", psm, strerror(errno));
		(void)close(fd);
		return (-1);
	}
	return (fd);
}

/* ---- the API -------------------------------------------------------------- */

static void
queue_init(void)
{
	if (g_q == NULL)
		g_q = dispatch_queue_create("org.nextbsd.bluetoothd.hid",
		    NULL);
}

int
hid_host_start(void)
{
	static const uint16_t psms[2] = { HID_PSM_CONTROL,
	    HID_PSM_INTERRUPT };
	int i, rc = 0;

	(void)pthread_mutex_lock(&g_hlock);
	queue_init();
	for (i = 0; i < 2 && g_q != NULL; i++) {
		if (g_listen[i] >= 0)
			continue;
		g_listen[i] = listen_on(psms[i]);
		if (g_listen[i] < 0) {
			rc = -1;
			continue;
		}
		g_listen_src[i] = dispatch_source_create(
		    DISPATCH_SOURCE_TYPE_READ, (uintptr_t)g_listen[i], 0, g_q);
		if (g_listen_src[i] == NULL) {
			(void)close(g_listen[i]);
			g_listen[i] = -1;
			rc = -1;
			continue;
		}
		dispatch_set_context(g_listen_src[i],
		    (void *)(intptr_t)psms[i]);
		dispatch_source_set_event_handler_f(g_listen_src[i],
		    listen_readable);
		dispatch_resume(g_listen_src[i]);
		xlog("listening on PSM 0x%02x", psms[i]);
	}
	(void)pthread_mutex_unlock(&g_hlock);
	return (g_q == NULL ? -1 : rc);
}

bt_status_t
hid_host_connect(const struct bt_addr *local, const struct bt_addr *peer)
{
	struct hid_pending *p;
	pthread_attr_t attr;
	pthread_t t;
	char addr[BT_ADDR_STRLEN];
	int rc;

	(void)hid_host_start();
	if (g_q == NULL)
		return (bt_status_internal_error_e);
	(void)pthread_mutex_lock(&g_hlock);
	if (find_session(peer) != NULL || find_pending(peer) != NULL) {
		(void)pthread_mutex_unlock(&g_hlock);
		return (bt_status_success_e);	/* up, or on its way */
	}
	p = calloc(1, sizeof(*p));
	if (p == NULL) {
		(void)pthread_mutex_unlock(&g_hlock);
		return (bt_status_internal_error_e);
	}
	p->peer = *peer;
	p->peer.type = BT_ADDR_PUBLIC;
	if (local != NULL) {
		p->local = *local;
		p->have_local = true;
	}
	p->ctl = p->intr = -1;
	p->next = g_pending;
	g_pending = p;
	(void)pthread_mutex_unlock(&g_hlock);

	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rc = pthread_create(&t, &attr, connect_thread, p);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		/* adopt_pending unlinks and frees it, and reports failure. */
		p->err = rc;
		dispatch_async_f(g_q, p, adopt_pending);
		return (bt_status_internal_error_e);
	}
	xlog("%s: connecting HID", bt_addr_ntoa(peer, BT_ADDR_FMT_DISPLAY,
	    addr, sizeof(addr)));
	return (bt_status_success_e);
}

/* Close the session with this peer, on the session queue. */
struct hid_close_req {
	struct bt_addr	peer;
	hs_after_t	after;
};

static void
close_req(void *arg)
{
	struct hid_close_req *r = arg;
	struct hid_session *s;

	(void)pthread_mutex_lock(&g_hlock);
	s = find_session(&r->peer);
	(void)pthread_mutex_unlock(&g_hlock);
	if (s != NULL)
		session_close(s, r->after, r->after == AFTER_ACL_DISCONNECT ?
		    "disconnect" : "unpaired");
	else if (r->after == AFTER_ACL_DISCONNECT) {
		bluetoothd_lock();
		conn_acl_disconnect_locked(&r->peer);
		bluetoothd_unlock();
	}
	free(r);
}

static bool
request_close(const struct bt_addr *peer, hs_after_t after)
{
	struct hid_close_req *r;
	struct hid_pending *p;
	bool any;

	if (g_q == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_hlock);
	p = find_pending(peer);
	if (p != NULL) {
		p->cancelled = true;
		p->then_acl = (after == AFTER_ACL_DISCONNECT);
	}
	any = (p != NULL || find_session(peer) != NULL);
	(void)pthread_mutex_unlock(&g_hlock);
	if (!any || p != NULL)
		return (any);	/* a pending connect closes itself */
	r = malloc(sizeof(*r));
	if (r == NULL)
		return (false);
	r->peer = *peer;
	r->after = after;
	dispatch_async_f(g_q, r, close_req);
	return (true);
}

bool
hid_host_disconnect(const struct bt_addr *peer, bool then_acl)
{
	return (request_close(peer, then_acl ? AFTER_ACL_DISCONNECT :
	    AFTER_NOTHING));
}

void
hid_host_unplug(const struct bt_addr *peer)
{
	struct hid_session *s;
	struct bt_bond *b;
	bool vc;

	if (g_q == NULL)
		return;
	b = bond_copy(peer);
	vc = b != NULL && (b->flags & BT_BOND_HID_ATTRS) != 0 &&
	    (b->hid_flags & BT_HID_VIRTUAL_CABLE) != 0;
	free(b);

	(void)pthread_mutex_lock(&g_hlock);
	s = find_session(peer);
	/* On the list, its control socket is still open. */
	if (s != NULL && vc && s->ctl >= 0) {
		uint8_t msg = HIDP_HID_CONTROL |
		    HIDP_CTRL_VIRTUAL_CABLE_UNPLUG;

		(void)send(s->ctl, &msg, 1, 0);
	}
	(void)pthread_mutex_unlock(&g_hlock);
	(void)request_close(peer, AFTER_NOTHING);
}

bool
hid_host_active(const struct bt_addr *peer)
{
	bool any;

	if (g_q == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_hlock);
	any = (find_session(peer) != NULL || find_pending(peer) != NULL);
	(void)pthread_mutex_unlock(&g_hlock);
	return (any);
}

static void
bond_updated(void *arg)
{
	struct bt_addr *peer = arg;
	struct hid_session *s;

	(void)pthread_mutex_lock(&g_hlock);
	s = find_session(peer);
	(void)pthread_mutex_unlock(&g_hlock);
	free(peer);
	if (s != NULL && s->state == HS_WAIT_DESC) {
		struct bt_bond *b = bond_copy(&s->peer);
		bool have = b != NULL && (b->flags & BT_BOND_HID_DESC) != 0;

		free(b);
		if (have)
			session_up(s);
	}
}

void
hid_host_bond_updated(const struct bt_addr *peer)
{
	struct bt_addr *copy;

	if (g_q == NULL || (copy = malloc(sizeof(*copy))) == NULL)
		return;
	*copy = *peer;
	dispatch_async_f(g_q, copy, bond_updated);
}
