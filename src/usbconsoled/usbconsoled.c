/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * usbconsoled — the person at the console may use the USB devices no
 * kernel driver claims (nextbsd/nextbsd#717).
 *
 * Scanners (and cameras, and anything else driven from user space with
 * libusb) have no kernel driver: programs open the device's ugen nodes,
 * /dev/usb/<bus>.<address>.<endpoint>, which devfs makes root:operator
 * 0600. On a Mac any program of the logged-in user may open such a
 * device; here the nodes are handed to the console user, the way
 * diskarbitrationd hands them removable disks (#712), and handed back to
 * root when they log out.
 *
 *   - Who: configd's State:/Users/ConsoleUser (consoleuser(8), set by the
 *     login window), watched as diskarbitrationd watches it.
 *   - Which devices: not a hub, and at least one interface of its
 *     configuration with no kernel driver attached. Interfaces a driver
 *     has (a printer's ulpt, a keyboard's ukbd, a disk's umass) stay the
 *     kernel's: ugen refuses their endpoints to anybody else.
 *   - When: every two seconds the daemon reads /dev/usb; a new device is
 *     looked at once (libusb20, as root), and its nodes are given to the
 *     console user if it qualifies. A change of console user gives every
 *     qualifying device to the new one (or back to root).
 *
 * NextBSD runs no devd(8), so arrivals are noticed by reading /dev/usb
 * (one readdir, no device opened) rather than from devctl.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecific.h>
#include <dispatch/dispatch.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <libusb20.h>
#include <libusb20_desc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define USB_DIR		"/dev/usb"
#define MAX_DEVICES	256
#define HUB_CLASS	0x09

struct device {
	char	name[16];	/* "<bus>.<address>" */
	int	eligible;
	int	seen;		/* in the last read of /dev/usb */
	uid_t	given;		/* whom its nodes were last given to */
};

static struct device devices[MAX_DEVICES];
static int ndevices;
static dispatch_queue_t queue;
static SCDynamicStoreRef store;
static int console_present;
static uid_t console_uid;
static int dry_run;

static void
say(const char *fmt, ...)
{
	char stamp[32];
	time_t now = time(NULL);
	va_list ap;

	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
	fprintf(stderr, "%s usbconsoled: ", stamp);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Not a hub, and an interface no kernel driver has. */
static int
qualifies(const char *name)
{
	struct libusb20_backend *be;
	struct libusb20_device *dev = NULL;
	unsigned bus, addr;
	int result = 0;

	if (sscanf(name, "%u.%u", &bus, &addr) != 2)
		return (0);
	be = libusb20_be_alloc_default();
	if (be == NULL)
		return (0);
	while ((dev = libusb20_be_device_foreach(be, dev)) != NULL) {
		struct LIBUSB20_DEVICE_DESC_DECODED *dd;
		struct libusb20_config *cfg;
		int i;

		if (libusb20_dev_get_bus_number(dev) != bus ||
		    libusb20_dev_get_address(dev) != addr)
			continue;
		if (libusb20_dev_open(dev, 0) != 0)
			break;
		dd = libusb20_dev_get_device_desc(dev);
		cfg = libusb20_dev_alloc_config(dev,
		    libusb20_dev_get_config_index(dev));
		if (dd != NULL && dd->bDeviceClass != HUB_CLASS && cfg != NULL) {
			for (i = 0; i < cfg->num_interface; i++) {
				if (cfg->interface[i].desc.bInterfaceClass ==
				    HUB_CLASS)
					continue;
				/* 0: a driver is attached. */
				if (libusb20_dev_kernel_driver_active(dev,
				    (uint8_t)i) != 0) {
					result = 1;
					break;
				}
			}
			say("%s: %04x:%04x %s", name, dd->idVendor,
			    dd->idProduct, result ?
			    "has an interface no driver claims: for the "
			    "console user" : "claimed by the kernel or a hub: "
			    "left alone");
		}
		free(cfg);
		libusb20_dev_close(dev);
		break;
	}
	libusb20_be_free(be);
	return (result);
}

/* Every node of the device to uid (root: back to devfs's 0600). */
static void
give(struct device *d, uid_t uid)
{
	char prefix[20], path[64];
	struct dirent *e;
	DIR *dir;

	if (d->given == uid)
		return;
	snprintf(prefix, sizeof(prefix), "%s.", d->name);
	dir = opendir(USB_DIR);
	if (dir == NULL)
		return;
	while ((e = readdir(dir)) != NULL) {
		if (strncmp(e->d_name, prefix, strlen(prefix)) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", USB_DIR, e->d_name);
		if (dry_run)
			continue;
		if (chown(path, uid, (gid_t)-1) != 0 ||
		    chmod(path, uid != 0 ? 0660 : 0600) != 0)
			say("%s: %s", path, strerror(errno));
	}
	closedir(dir);
	say("%s: %s to uid %u", d->name, dry_run ? "would give" : "given",
	    (unsigned)uid);
	d->given = uid;
}

static uid_t
owner(void)
{
	return (console_present ? console_uid : 0);
}

/* Read /dev/usb: look at new devices, forget the ones gone. */
static void
scan(void)
{
	struct dirent *e;
	DIR *dir;
	int i;

	for (i = 0; i < ndevices; i++)
		devices[i].seen = 0;
	dir = opendir(USB_DIR);
	if (dir != NULL) {
		while ((e = readdir(dir)) != NULL) {
			char name[16];
			unsigned b, a, ep;
			struct device *d = NULL;

			/* The control endpoint's node stands for the device. */
			if (sscanf(e->d_name, "%u.%u.%u", &b, &a, &ep) != 3 ||
			    ep != 0)
				continue;
			snprintf(name, sizeof(name), "%u.%u", b, a);
			for (i = 0; i < ndevices; i++)
				if (strcmp(devices[i].name, name) == 0)
					d = &devices[i];
			if (d == NULL) {
				if (ndevices == MAX_DEVICES)
					continue;
				d = &devices[ndevices++];
				memset(d, 0, sizeof(*d));
				strlcpy(d->name, name, sizeof(d->name));
				d->eligible = qualifies(name);
			}
			d->seen = 1;
		}
		closedir(dir);
	}
	/* Gone (an address is used again by the next device plugged in). */
	for (i = 0; i < ndevices; ) {
		if (!devices[i].seen) {
			devices[i] = devices[--ndevices];
			continue;
		}
		i++;
	}
	for (i = 0; i < ndevices; i++)
		if (devices[i].eligible)
			give(&devices[i], owner());
}

static void
read_console_user(void)
{
	uid_t uid = 0;
	gid_t gid = 0;
	CFStringRef name;

	name = SCDynamicStoreCopyConsoleUser(store, &uid, &gid);
	console_present = name != NULL && uid != 0;
	console_uid = console_present ? uid : 0;
	if (name != NULL)
		CFRelease(name);
	say("console user: %s", console_present ? "uid set" : "none");
}

static void
console_changed(SCDynamicStoreRef s, CFArrayRef keys, void *info)
{
	read_console_user();
	scan();
}

static void connect_store(void *context);

static void
retry(void)
{
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
	    5 * (int64_t)NSEC_PER_SEC), queue, NULL, connect_store);
}

/* As diskarbitrationd: the console-user key, on our queue. */
static void
connect_store(void *context)
{
	CFStringRef key;
	CFArrayRef keys;

	store = SCDynamicStoreCreate(kCFAllocatorDefault,
	    CFSTR("org.nextbsd.usbconsoled"), console_changed, NULL);
	if (store == NULL) {
		retry();
		return;
	}
	key = SCDynamicStoreKeyCreateConsoleUser(kCFAllocatorDefault);
	keys = CFArrayCreate(kCFAllocatorDefault, (const void **)&key, 1,
	    &kCFTypeArrayCallBacks);
	CFRelease(key);
	if (!SCDynamicStoreSetNotificationKeys(store, keys, NULL) ||
	    !SCDynamicStoreSetDispatchQueue(store, queue)) {
		CFRelease(keys);
		CFRelease(store);
		store = NULL;
		say("configd is not reachable (%s); retrying",
		    SCErrorString(SCError()));
		retry();
		return;
	}
	CFRelease(keys);
	read_console_user();
	scan();
}

int
main(int argc, char **argv)
{
	dispatch_source_t timer;
	int ch;

	while ((ch = getopt(argc, argv, "n")) != -1) {
		switch (ch) {
		case 'n':
			dry_run = 1;
			break;
		default:
			fprintf(stderr, "usage: usbconsoled [-n]\n");
			return (64);
		}
	}
	if (getuid() != 0 && !dry_run) {
		fprintf(stderr, "usbconsoled: must be run as root\n");
		return (77);
	}
	setvbuf(stderr, NULL, _IOLBF, 0);
	queue = dispatch_queue_create("org.nextbsd.usbconsoled", NULL);
	dispatch_async_f(queue, NULL, connect_store);
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW,
	    2 * (int64_t)NSEC_PER_SEC), 2 * (int64_t)NSEC_PER_SEC,
	    (int64_t)NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler_f(timer, (dispatch_function_t)scan);
	dispatch_resume(timer);
	say("watching %s and the console user%s", USB_DIR,
	    dry_run ? " (-n: changing nothing)" : "");
	dispatch_main();
}
