/*
 * firmware.c — firmware download for bootloader-mode controllers. See
 * firmware.h.
 *
 * The device half of iwmbtfw's and rtlbtfw's main(): find the device by its
 * ugen bus and address, check that ng_ubt has not attached, hand it to the
 * protocol code, reset it. The table of devices seen is guarded by its own
 * mutex, not bluetoothd's state lock: the workers write it.
 */
#include "firmware.h"
#include "bluetoothd.h"
#include "btstate.h"
#include "fw_ids.h"
#include "fw_intel.h"
#include "fw_rtl.h"

/*
 * KextManager.h uses the AVAILABLE_MAC_OS_X_* macros and kern_return_t
 * without including them.
 */
#include <mach/mach.h>
#include <AvailabilityMacros.h>
#include <IOKit/kext/KextManager.h>
#include <CoreFoundation/CoreFoundation.h>

#include <libusb.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	FW_MAX		8

struct fw_dev {
	bool			used;
	char			ugen[32];
	uint16_t		vendor;
	uint16_t		product;
	enum fw_kind		kind;
	enum fw_intel_gen	gen;
	bt_firmware_state_t	state;
	bool			busy;		/* a worker owns it */
	bool			dirty;		/* publication out of date */
};

static pthread_mutex_t	g_fw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t	g_dl_lock = PTHREAD_MUTEX_INITIALIZER;	/* 1 at once */
static struct fw_dev	g_fw[FW_MAX];

void
fw_log(const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	/* One write per line, so worker and main-loop lines do not mix. */
	(void)fprintf(stderr, "bluetoothd[firmware] %s\n", line);
	(void)fflush(stderr);
}

static void
set_state(struct fw_dev *d, bt_firmware_state_t st)
{
	(void)pthread_mutex_lock(&g_fw_lock);
	d->state = st;
	d->dirty = true;
	(void)pthread_mutex_unlock(&g_fw_lock);
	bluetoothd_wake();
}

/* ugenB.A -> bus B, address A (iwmbtfw's parse_ugen_name). */
static int
parse_ugen_name(const char *ugen, uint8_t *bus, uint8_t *addr)
{
	char *ep;

	if (strncmp(ugen, "ugen", 4) != 0)
		return (-1);
	*bus = (uint8_t)strtoul(ugen + 4, &ep, 10);
	if (*ep != '.')
		return (-1);
	*addr = (uint8_t)strtoul(ep + 1, &ep, 10);
	if (*ep != '\0')
		return (-1);
	return (0);
}

/*
 * Make sure ng_ubt has probed this device. KextManager returns once kextd's
 * load is done, and a kext load probes the devices already present.
 */
static void
load_ubt(void)
{
	OSReturn r;

	r = KextManagerLoadKextWithIdentifier(CFSTR(FW_KEXT_UBT), NULL);
	if (r != kOSReturnSuccess)
		fw_log("cannot load %s: 0x%x", FW_KEXT_UBT, (unsigned)r);
}

/* The device at ugen bus `bus`, address `addr`, if it is still `d`'s. */
static libusb_device *
find_dev(libusb_context *ctx, const struct fw_dev *d, uint8_t bus,
    uint8_t addr)
{
	libusb_device **list = NULL, *dev = NULL;
	struct libusb_device_descriptor dd;
	ssize_t cnt, i;

	cnt = libusb_get_device_list(ctx, &list);
	for (i = 0; i < cnt; i++) {
		if (libusb_get_bus_number(list[i]) == bus &&
		    libusb_get_device_address(list[i]) == addr &&
		    libusb_get_device_descriptor(list[i], &dd) == 0 &&
		    dd.idVendor == d->vendor && dd.idProduct == d->product) {
			dev = libusb_ref_device(list[i]);
			break;
		}
	}
	if (list != NULL)
		libusb_free_device_list(list, 1);
	return (dev);
}

/*
 * After libusb_reset_device(). The reset is asynchronous: USB_DEVICEENUMERATE
 * only queues the port reset for the hub's explore thread, which then resets
 * the port, sets the configuration and lets ng_ubt probe again, all under the
 * device's enumeration lock. Every ugen request that follows (the open, the
 * kernel-driver test) takes that lock, so it waits for all of that once it has
 * begun; this is only for the thread to begin. 150 ms is what Linux's btintel
 * allows an Intel controller after its own reset (btintel_reset_to_bootloader).
 */
#define	FW_RESET_SETTLE_US	150000

static bt_firmware_state_t
download(struct fw_dev *d)
{
	libusb_context *ctx = NULL;
	libusb_device *dev = NULL;
	libusb_device_handle *hdl = NULL;
	bt_firmware_state_t st = bt_firmware_failed_e;
	bool retried = false;
	uint8_t bus, addr;
	int r;

	if (parse_ugen_name(d->ugen, &bus, &addr) < 0) {
		fw_log("%s: not a ugen name", d->ugen);
		return (st);
	}
	if ((r = libusb_init(&ctx)) != 0) {
		fw_log("libusb_init failed: %d", r);
		return (st);
	}

	dev = find_dev(ctx, d, bus, addr);
	if (dev == NULL) {
		fw_log("%s: device not found (it may have gone)", d->ugen);
		goto out;
	}

	/* Realtek's own VID: only its Bluetooth interface (rtlbtfw.conf). */
	if (d->kind == FW_KIND_REALTEK_GENERIC && !fw_rtl_is_bluetooth(dev)) {
		st = bt_firmware_none_e;
		goto out;
	}

	for (;;) {
		if ((r = libusb_open(dev, &hdl)) != 0) {
			fw_log("%s: libusb_open failed: %s", d->ugen,
			    libusb_strerror(r));
			goto out;
		}

		/* ng_ubt attached: the firmware is in already. */
		r = libusb_kernel_driver_active(hdl, 0);
		if (r < 0) {
			fw_log("%s: libusb_kernel_driver_active failed: %s",
			    d->ugen, libusb_strerror(r));
			goto out;
		}
		if (r > 0) {
			if (retried)
				fw_log("%s: ng_ubt attached after the reset",
				    d->ugen);
			st = bt_firmware_operational_e;
			goto out;
		}

		if (!retried) {
			fw_log("%s: %04x:%04x has no driver; checking its "
			    "firmware", d->ugen, d->vendor, d->product);
			set_state(d, bt_firmware_downloading_e);
		}
		if (d->kind == FW_KIND_INTEL)
			r = fw_intel_run(hdl, d->gen, FW_DIR_INTEL);
		else
			r = fw_rtl_run(hdl, FW_DIR_RTL);
		if (r != FW_RESULT_NO_REPLY || retried)
			break;

		/*
		 * An Intel controller left wedged by a warm reboot does not
		 * answer Read Version (0xfc05) at all, and only a power cycle
		 * or a USB reset brings it back (#539). Reset it once, as
		 * btusb does for a controller whose commands time out, and
		 * try again. The re-enumeration keeps the ugen name and this
		 * worker still owns it (`busy`), so firmware_usb_arrived()
		 * leaves any arrival it produces to us. The handle is not
		 * trusted across the reset: close it and find the device
		 * again by bus and address, as above.
		 */
		fw_log("%s: no reply to identify; resetting the device and "
		    "trying once more", d->ugen);
		r = libusb_reset_device(hdl);
		if (r != 0)
			fw_log("%s: libusb_reset_device failed: %s", d->ugen,
			    libusb_strerror(r));
		libusb_close(hdl);
		hdl = NULL;
		libusb_unref_device(dev);
		retried = true;
		(void)usleep(FW_RESET_SETTLE_US);
		dev = find_dev(ctx, d, bus, addr);
		if (dev == NULL) {
			/* A new address is a new ugen: its own arrival. */
			fw_log("%s: device not found after the reset",
			    d->ugen);
			goto out;
		}
	}

	switch (r) {
	case FW_RESULT_DOWNLOADED:
		fw_log("%s: firmware loaded%s; resetting so ng_ubt attaches",
		    d->ugen, retried ? " after the reset" : "");
		/* Ask the kernel to probe and attach the device again. */
		r = libusb_reset_device(hdl);
		if (r != 0)
			fw_log("%s: libusb_reset_device failed: %s", d->ugen,
			    libusb_strerror(r));
		st = bt_firmware_done_e;
		break;
	case FW_RESULT_OPERATIONAL:
		if (retried)
			fw_log("%s: operational after the reset", d->ugen);
		st = bt_firmware_operational_e;
		break;
	default:
		fw_log("%s: firmware download failed%s", d->ugen,
		    retried ? (r == FW_RESULT_NO_REPLY ?
		    " (still no reply after the reset)" :
		    " after the reset") : "");
		st = bt_firmware_failed_e;
		break;
	}

out:
	if (hdl != NULL)
		libusb_close(hdl);
	if (dev != NULL)
		libusb_unref_device(dev);
	libusb_exit(ctx);
	return (st);
}

static void *
worker(void *arg)
{
	struct fw_dev *d = arg;
	bt_firmware_state_t st;

	(void)pthread_mutex_lock(&g_dl_lock);
	load_ubt();
	st = download(d);
	(void)pthread_mutex_unlock(&g_dl_lock);

	(void)pthread_mutex_lock(&g_fw_lock);
	d->state = st;
	d->dirty = true;
	d->busy = false;
	(void)pthread_mutex_unlock(&g_fw_lock);
	bluetoothd_wake();
	return (NULL);
}

void
firmware_usb_arrived(const struct bt_iokit_event *ev)
{
	struct fw_dev *d = NULL;
	enum fw_intel_gen gen;
	enum fw_kind kind;
	pthread_attr_t attr;
	pthread_t t;
	int i;

	kind = fw_ids_lookup(ev->vendor, ev->product, &gen);
	if (kind == FW_KIND_NONE || ev->ugen[0] == '\0')
		return;
	/*
	 * Realtek's own VID is on card readers and Ethernet too: only a
	 * Bluetooth HCI interface (e0/01/01) is ours, as in rtlbtfw.conf.
	 */
	if (kind == FW_KIND_REALTEK_GENERIC && (ev->if_class[0] != 0xe0 ||
	    ev->if_class[1] != 0x01 || ev->if_class[2] != 0x01))
		return;

	(void)pthread_mutex_lock(&g_fw_lock);
	for (i = 0; i < FW_MAX; i++) {
		if (g_fw[i].used && strcmp(g_fw[i].ugen, ev->ugen) == 0) {
			d = &g_fw[i];
			break;
		}
	}
	if (d != NULL && d->busy) {
		/* The re-enumeration of a device we are resetting. */
		(void)pthread_mutex_unlock(&g_fw_lock);
		return;
	}
	for (i = 0; d == NULL && i < FW_MAX; i++) {
		if (!g_fw[i].used || !g_fw[i].busy)
			d = &g_fw[i];
	}
	if (d == NULL) {
		(void)pthread_mutex_unlock(&g_fw_lock);
		fw_log("too many devices at once; ignoring %s", ev->ugen);
		return;
	}
	(void)memset(d, 0, sizeof(*d));
	d->used = true;
	(void)strlcpy(d->ugen, ev->ugen, sizeof(d->ugen));
	d->vendor = ev->vendor;
	d->product = ev->product;
	d->kind = kind;
	d->gen = gen;
	d->state = bt_firmware_checking_e;
	d->busy = true;
	(void)pthread_mutex_unlock(&g_fw_lock);

	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &attr, worker, d) != 0) {
		fw_log("%s: cannot start a firmware worker", ev->ugen);
		(void)pthread_mutex_lock(&g_fw_lock);
		d->busy = false;
		d->state = bt_firmware_failed_e;
		d->dirty = true;
		(void)pthread_mutex_unlock(&g_fw_lock);
	}
	(void)pthread_attr_destroy(&attr);
}

bool
firmware_info(int idx, char *ugen, size_t ugenlen, int *vendor, int *product,
    bt_firmware_state_t *state)
{
	int i, n = 0;
	bool found = false;

	(void)pthread_mutex_lock(&g_fw_lock);
	for (i = 0; i < FW_MAX; i++) {
		/* Devices that were not Bluetooth after all are not listed. */
		if (!g_fw[i].used || g_fw[i].state == bt_firmware_none_e)
			continue;
		if (n++ != idx)
			continue;
		(void)strlcpy(ugen, g_fw[i].ugen, ugenlen);
		*vendor = g_fw[i].vendor;
		*product = g_fw[i].product;
		*state = g_fw[i].state;
		found = true;
		break;
	}
	(void)pthread_mutex_unlock(&g_fw_lock);
	return (found);
}

static const char *
state_name(bt_firmware_state_t s)
{
	switch (s) {
	case bt_firmware_checking_e:	return ("checking");
	case bt_firmware_downloading_e:	return ("downloading");
	case bt_firmware_done_e:	return ("done");
	case bt_firmware_operational_e:	return ("operational");
	case bt_firmware_failed_e:	return ("firmware-failed");
	default:			return ("none");
	}
}

void
firmware_publish(void)
{
	struct fw_dev snap[FW_MAX];
	int i, n = 0;

	(void)pthread_mutex_lock(&g_fw_lock);
	for (i = 0; i < FW_MAX; i++) {
		if (g_fw[i].used && g_fw[i].dirty) {
			g_fw[i].dirty = false;
			if (g_fw[i].state != bt_firmware_none_e)
				snap[n++] = g_fw[i];
		}
	}
	(void)pthread_mutex_unlock(&g_fw_lock);

	for (i = 0; i < n; i++)
		(void)bt_state_publish_firmware(snap[i].ugen, snap[i].vendor,
		    snap[i].product, state_name(snap[i].state));
	if (n > 0)
		bt_state_notify(BT_NOTIFY_FIRMWARE);
}
