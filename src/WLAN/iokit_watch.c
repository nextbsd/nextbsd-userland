/*
 * iokit_watch.c — follow 802.11 radios through the IORegistry
 * (nextbsd-userland#502). See iokit_watch.h.
 *
 * Modelled on DiskArbitration's da_iokit_subscribe.c: one IONotificationPort,
 * its callbacks delivered on a serial dispatch queue, a kIOFirstMatchNotification
 * and a kIOTerminatedNotification watch, and the arming iterators drained at
 * registration so radios that already exist are not missed. Unlike DA, the
 * matching dictionary names one class, IO80211Controller, so the kernel
 * filters and the callbacks only ever see radios.
 *
 * The callbacks do no radio work. Bringing a VAP up spawns wpa_supplicant and
 * waits on its control socket, and tearing one down reaps it; that belongs on
 * the main loop, which owns the interface table and the child processes. So a
 * callback sets a flag and writes one byte to the main loop's self-pipe, and
 * the main loop asks net.wlan.devices what changed: an IO80211Controller entry
 * is published once the radio is listed there, and retired once it is gone.
 */
#include "iokit_watch.h"

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define	WLAN_IOKIT_CLASS	"IO80211Controller"

/* Held for the daemon's lifetime, as in DiskArbitration. */
static IONotificationPortRef	g_notify;
static dispatch_queue_t		g_queue;
static int			g_wake_fd = -1;
static atomic_bool		g_matched;
static atomic_bool		g_terminated;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "wland[iokit] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Release every object in `it`; returns true if there was at least one. The
 * per-event iterator belongs to libIOKit, which tears it down when the
 * callback returns, so only the objects are released here.
 */
static bool
drain_iterator(io_iterator_t it)
{
	io_object_t obj;
	bool any = false;

	while ((obj = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		any = true;
		IOObjectRelease(obj);
	}
	return (any);
}

static void
wake_main_loop(void)
{
	char c = 0;

	/* Non-blocking: a full pipe already means "wake up". */
	while (write(g_wake_fd, &c, 1) < 0 && errno == EINTR)
		;
}

static void
matched_callback(void *refcon, io_iterator_t it)
{
	(void)refcon;
	if (drain_iterator(it)) {
		atomic_store(&g_matched, true);
		wake_main_loop();
	}
}

static void
terminated_callback(void *refcon, io_iterator_t it)
{
	(void)refcon;
	if (drain_iterator(it)) {
		atomic_store(&g_terminated, true);
		wake_main_loop();
	}
}

int
iokit_watch_start(int wake_fd)
{
	io_iterator_t match_it = IO_OBJECT_NULL;
	io_iterator_t term_it = IO_OBJECT_NULL;
	kern_return_t kr;
	int fd;

	/*
	 * No /dev/ioregistry: a kernel without the in-kernel registry, so no
	 * notification will ever come. Quietly let the caller poll instead.
	 */
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
	g_queue = dispatch_queue_create("org.nextbsd.wland.iokit", NULL);
	if (g_queue == NULL) {
		xlog("dispatch_queue_create failed");
		goto fail;
	}
	IONotificationPortSetDispatchQueue(g_notify, g_queue);

	/* IOServiceAddMatchingNotification consumes one dictionary each. */
	kr = IOServiceAddMatchingNotification(g_notify,
	    kIOFirstMatchNotification, IOServiceMatching(WLAN_IOKIT_CLASS),
	    matched_callback, NULL, &match_it);
	if (kr != KERN_SUCCESS) {
		xlog("AddMatchingNotification(first match) kr=0x%x",
		    (unsigned)kr);
		goto fail;
	}
	kr = IOServiceAddMatchingNotification(g_notify,
	    kIOTerminatedNotification, IOServiceMatching(WLAN_IOKIT_CLASS),
	    terminated_callback, NULL, &term_it);
	if (kr != KERN_SUCCESS) {
		xlog("AddMatchingNotification(terminated) kr=0x%x",
		    (unsigned)kr);
		goto fail;
	}

	/*
	 * Arm: each watch handed back an iterator over what already matches.
	 * A radio that is already there counts as a first match. These are the
	 * caller-owned arming iterators, so they are released here too.
	 */
	if (drain_iterator(match_it)) {
		atomic_store(&g_matched, true);
		wake_main_loop();
	}
	(void)drain_iterator(term_it);
	IOObjectRelease(match_it);
	IOObjectRelease(term_it);
	return (0);

fail:
	if (match_it != IO_OBJECT_NULL) {
		(void)drain_iterator(match_it);
		IOObjectRelease(match_it);
	}
	if (term_it != IO_OBJECT_NULL) {
		(void)drain_iterator(term_it);
		IOObjectRelease(term_it);
	}
	IONotificationPortDestroy(g_notify);
	g_notify = NULL;
	if (g_queue != NULL) {
		dispatch_release(g_queue);
		g_queue = NULL;
	}
	g_wake_fd = -1;
	return (-1);
}

void
iokit_watch_take(bool *matched, bool *terminated)
{
	*matched = atomic_exchange(&g_matched, false);
	*terminated = atomic_exchange(&g_terminated, false);
}
