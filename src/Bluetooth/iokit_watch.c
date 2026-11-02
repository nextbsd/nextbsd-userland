/*
 * iokit_watch.c — follow Bluetooth controllers through the IORegistry. See
 * iokit_watch.h.
 *
 * Same shape as src/WLAN/iokit_watch.c, itself modelled on DiskArbitration's
 * da_iokit_subscribe.c: one IONotificationPort, its callbacks delivered on a
 * serial dispatch queue, a kIOFirstMatchNotification and a
 * kIOTerminatedNotification watch on one class, and the arming iterators
 * drained at registration so a controller that is already there is not missed.
 *
 * One difference from wland. wland's callbacks only set a flag, because the
 * main loop can ask net.wlan.devices what changed. There is no such list for
 * Bluetooth (polling NGM_LISTNODES is what this replaces), so each callback
 * records the entry itself: its registry ID, and on arrival its node name and
 * USB ids. The events go on a FIFO the main loop drains. The callbacks still do
 * no controller work: bring-up builds a netgraph graph and talks HCI, which
 * belongs on the main loop.
 */
#include "iokit_watch.h"
#include "bluetoothd.h"
#include "fw_ids.h"

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ev_node {
	struct ev_node		*next;
	struct bt_iokit_event	ev;
};

/* Held for the daemon's lifetime, as in DiskArbitration. */
static IONotificationPortRef	g_notify;
static dispatch_queue_t		g_queue;
static int			g_wake_fd = -1;

/* The event FIFO: written on the dispatch queue, read on the main loop. */
static pthread_mutex_t		g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ev_node		*g_ev_head;
static struct ev_node		*g_ev_tail;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[iokit] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void
wake_main_loop(void)
{
	char c = 0;

	if (g_wake_fd < 0)
		return;
	/* Non-blocking: a full pipe already means "wake up". */
	while (write(g_wake_fd, &c, 1) < 0 && errno == EINTR)
		;
}

static void
enqueue(const struct bt_iokit_event *ev)
{
	struct ev_node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL) {
		xlog("out of memory: lost a controller %s event",
		    ev->kind == BT_IOKIT_ARRIVED ? "arrival" : "departure");
		return;
	}
	n->ev = *ev;
	(void)pthread_mutex_lock(&g_ev_lock);
	if (g_ev_tail != NULL)
		g_ev_tail->next = n;
	else
		g_ev_head = n;
	g_ev_tail = n;
	(void)pthread_mutex_unlock(&g_ev_lock);
}

static void
prop_string(CFDictionaryRef props, const char *key, char *out, size_t len)
{
	CFStringRef k;
	CFTypeRef v;

	k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	if (k == NULL)
		return;
	v = CFDictionaryGetValue(props, k);
	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
		(void)CFStringGetCString(v, out, (CFIndex)len,
		    kCFStringEncodingUTF8);
}

static uint16_t
prop_u16(CFDictionaryRef props, const char *key)
{
	CFStringRef k;
	CFTypeRef v;
	long long n = 0;

	k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	if (k == NULL)
		return (0);
	v = CFDictionaryGetValue(props, k);
	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID())
		(void)CFNumberGetValue(v, kCFNumberLongLongType, &n);
	return ((uint16_t)n);
}

/* Fill an arrival event from a live entry. */
static void
describe(io_object_t obj, struct bt_iokit_event *ev)
{
	CFMutableDictionaryRef props = NULL;
	io_name_t name;
	kern_return_t kr;

	kr = IORegistryEntryCreateCFProperties(obj, &props, NULL, 0);
	if (kr != KERN_SUCCESS || props == NULL) {
		if (bluetoothd_debug)
			xlog("registry entry %llu: properties unreadable "
			    "(kr=0x%x)", (unsigned long long)ev->entry_id,
			    (unsigned)kr);
	} else {
		prop_string(props, BT_IOKIT_KEY_NODE, ev->node,
		    sizeof(ev->node));
		prop_string(props, BT_IOKIT_KEY_TRANSPORT, ev->transport,
		    sizeof(ev->transport));
		ev->vendor = prop_u16(props, BT_IOKIT_KEY_VENDOR);
		ev->product = prop_u16(props, BT_IOKIT_KEY_PRODUCT);
		prop_string(props, BT_IOKIT_KEY_UGEN, ev->ugen,
		    sizeof(ev->ugen));
		ev->if_class[0] = (uint8_t)prop_u16(props,
		    BT_IOKIT_KEY_IFCLASS);
		ev->if_class[1] = (uint8_t)prop_u16(props,
		    BT_IOKIT_KEY_IFSUBCLASS);
		ev->if_class[2] = (uint8_t)prop_u16(props,
		    BT_IOKIT_KEY_IFPROTOCOL);
		CFRelease(props);
	}
	if (ev->node[0] == '\0' && IORegistryEntryGetName(obj, name) ==
	    KERN_SUCCESS)
		(void)strlcpy(ev->node, name, sizeof(ev->node));
}

/*
 * Queue one event per object in `it` and release the objects; returns true if
 * there was at least one. The per-event iterator belongs to libIOKit, which
 * tears it down when the callback returns, so only the objects are released.
 */
static bool
drain_iterator(io_iterator_t it, bt_iokit_event_kind_t kind)
{
	io_object_t obj;
	bool any = false;

	while ((obj = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		struct bt_iokit_event ev;

		(void)memset(&ev, 0, sizeof(ev));
		ev.kind = kind;
		if (IORegistryEntryGetRegistryEntryID(obj, &ev.entry_id) ==
		    KERN_SUCCESS) {
			if (kind != BT_IOKIT_DEPARTED)
				describe(obj, &ev);
			enqueue(&ev);
			any = true;
		} else if (bluetoothd_debug) {
			xlog("%s match skipped: no registry entry ID (it may "
			    "have gone)", kind == BT_IOKIT_USB_ARRIVED ?
			    BT_IOKIT_USB_CLASS : BT_IOKIT_CLASS);
		}
		IOObjectRelease(obj);
	}
	return (any);
}

static void
matched_callback(void *refcon, io_iterator_t it)
{
	(void)refcon;
	if (drain_iterator(it, BT_IOKIT_ARRIVED))
		wake_main_loop();
}

static void
terminated_callback(void *refcon, io_iterator_t it)
{
	(void)refcon;
	if (drain_iterator(it, BT_IOKIT_DEPARTED))
		wake_main_loop();
}

static void
usb_matched_callback(void *refcon, io_iterator_t it)
{
	(void)refcon;
	if (drain_iterator(it, BT_IOKIT_USB_ARRIVED))
		wake_main_loop();
}

/*
 * IOUSBHostDevice first-match for each vendor id the firmware loader knows
 * (libIOKit compares idVendor as a number, nextbsd-userland#527). Only
 * arrivals: a device that goes away mid-download fails its transfers, and a
 * departure needs nothing else. A failure here costs firmware download only,
 * so it is logged and the controller watch stays up.
 */
static void
usb_watch_start(void)
{
	uint16_t vids[FW_IDS_MAX_VENDORS];
	int i, n;

	n = fw_ids_vendors(vids, FW_IDS_MAX_VENDORS);
	for (i = 0; i < n; i++) {
		CFMutableDictionaryRef m;
		CFNumberRef v;
		io_iterator_t it = IO_OBJECT_NULL;
		kern_return_t kr;
		int vid = vids[i];

		m = IOServiceMatching(BT_IOKIT_USB_CLASS);
		v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
		if (m == NULL || v == NULL) {
			if (m != NULL)
				CFRelease(m);
			if (v != NULL)
				CFRelease(v);
			continue;
		}
		CFDictionarySetValue(m, CFSTR(BT_IOKIT_KEY_VENDOR), v);
		CFRelease(v);
		kr = IOServiceAddMatchingNotification(g_notify,
		    kIOFirstMatchNotification, m, usb_matched_callback, NULL,
		    &it);
		if (kr != KERN_SUCCESS) {
			xlog("AddMatchingNotification(%s idVendor 0x%04x) "
			    "kr=0x%x", BT_IOKIT_USB_CLASS, vid, (unsigned)kr);
			continue;
		}
		/* A device present at boot counts as an arrival. */
		if (drain_iterator(it, BT_IOKIT_USB_ARRIVED))
			wake_main_loop();
		IOObjectRelease(it);
	}
}

int
iokit_watch_start(int wake_fd)
{
	io_iterator_t match_it = IO_OBJECT_NULL;
	io_iterator_t term_it = IO_OBJECT_NULL;
	kern_return_t kr;
	int fd;

	/* No /dev/ioregistry: a kernel without the in-kernel registry. */
	fd = open("/dev/ioregistry", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	(void)close(fd);

	g_wake_fd = wake_fd;
	g_notify = IONotificationPortCreate(kIOMainPortDefault);
	if (g_notify == NULL) {
		xlog("IONotificationPortCreate failed");
		return (-1);
	}
	g_queue = dispatch_queue_create("org.nextbsd.bluetoothd.iokit", NULL);
	if (g_queue == NULL) {
		xlog("dispatch_queue_create failed");
		goto fail;
	}
	IONotificationPortSetDispatchQueue(g_notify, g_queue);

	/* IOServiceAddMatchingNotification consumes one dictionary each. */
	kr = IOServiceAddMatchingNotification(g_notify,
	    kIOFirstMatchNotification, IOServiceMatching(BT_IOKIT_CLASS),
	    matched_callback, NULL, &match_it);
	if (kr != KERN_SUCCESS) {
		xlog("AddMatchingNotification(first match) kr=0x%x",
		    (unsigned)kr);
		goto fail;
	}
	kr = IOServiceAddMatchingNotification(g_notify,
	    kIOTerminatedNotification, IOServiceMatching(BT_IOKIT_CLASS),
	    terminated_callback, NULL, &term_it);
	if (kr != KERN_SUCCESS) {
		xlog("AddMatchingNotification(terminated) kr=0x%x",
		    (unsigned)kr);
		goto fail;
	}

	/*
	 * Arm: each watch handed back an iterator over what already matches. A
	 * controller that is already there counts as an arrival; the terminated
	 * iterator arms empty. These are the caller-owned arming iterators, so
	 * they are released here too.
	 */
	if (drain_iterator(match_it, BT_IOKIT_ARRIVED))
		wake_main_loop();
	(void)drain_iterator(term_it, BT_IOKIT_DEPARTED);
	IOObjectRelease(match_it);
	IOObjectRelease(term_it);

	/* Controllers that need firmware first (#202). */
	usb_watch_start();
	return (0);

fail:
	if (match_it != IO_OBJECT_NULL) {
		(void)drain_iterator(match_it, BT_IOKIT_ARRIVED);
		IOObjectRelease(match_it);
	}
	if (term_it != IO_OBJECT_NULL) {
		(void)drain_iterator(term_it, BT_IOKIT_DEPARTED);
		IOObjectRelease(term_it);
	}
	IONotificationPortDestroy(g_notify);
	g_notify = NULL;
	if (g_queue != NULL) {
		dispatch_release(g_queue);
		g_queue = NULL;
	}
	g_wake_fd = -1;

	/* Drop anything the failed arming queued. */
	{
		struct bt_iokit_event ev;

		while (iokit_watch_next(&ev))
			;
	}
	return (-1);
}

bool
iokit_watch_next(struct bt_iokit_event *ev)
{
	struct ev_node *n;

	(void)pthread_mutex_lock(&g_ev_lock);
	n = g_ev_head;
	if (n != NULL) {
		g_ev_head = n->next;
		if (g_ev_head == NULL)
			g_ev_tail = NULL;
	}
	(void)pthread_mutex_unlock(&g_ev_lock);

	if (n == NULL)
		return (false);
	*ev = n->ev;
	free(n);
	return (true);
}
