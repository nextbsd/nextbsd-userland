/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_usb.c — UsbGrant: Scanner.app's USB scanners (nextbsd/nextbsd#717).
 *
 * SANE drives most USB scanners from user space with libusb, through the
 * device's ugen nodes, /dev/usb/<bus>.<address>.<endpoint>, which devfs
 * makes root:operator 0600. When Scanner finds such a device it can't
 * open, it asks for that one device here; an administrator at the console
 * gets it (org.nextbsd.usb.device), nobody else.
 *
 * Only a device no kernel driver has is given: not a hub, and no
 * interface of its configuration with a driver attached. Disks (umass),
 * keyboards and mice (ukbd, ums, uhid), sound (uaudio) and network
 * adapters all have one, so they are refused, as is anything a driver
 * claims later in part. The check is libusb20's, as root.
 *
 * The nodes are changed in place: lstat'd in /dev/usb (which must be
 * devfs), each a character device, chown and chmod with
 * AT_SYMLINK_NOFOLLOW; /dev/ugen<bus>.<address> (devfs's link to the
 * control node) the same way if it is there. Nothing outside /dev/usb and
 * that link is touched. Nothing gives the device back: when it is
 * unplugged devfs removes its nodes, and they come back root's when it is
 * plugged in again.
 */
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libusb20.h>
#include <libusb20_desc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	USB_DIR		"/dev/usb"
#define	DEV_DIR		"/dev"
#define	HUB_CLASS	0x09

/*
 * Is the device at bus.address one no kernel driver has? *vendor and
 * *product name it for the log. SH_OK, or SH_ERR_INVALID (no such device,
 * a hub, or an interface with a driver).
 */
static int
unclaimed(int bus, int address, unsigned *vendor, unsigned *product)
{
	struct libusb20_backend *be;
	struct libusb20_device *dev = NULL;
	struct LIBUSB20_DEVICE_DESC_DECODED *dd;
	struct libusb20_config *cfg;
	int rc = SH_ERR_INVALID, found = 0, i;

	be = libusb20_be_alloc_default();
	if (be == NULL)
		return (SH_ERR_FAILED);
	while ((dev = libusb20_be_device_foreach(be, dev)) != NULL) {
		if (libusb20_dev_get_bus_number(dev) != bus ||
		    libusb20_dev_get_address(dev) != address)
			continue;
		found = 1;
		if (libusb20_dev_open(dev, 0) != 0) {
			rc = SH_ERR_FAILED;
			break;
		}
		dd = libusb20_dev_get_device_desc(dev);
		*vendor = dd->idVendor;
		*product = dd->idProduct;
		cfg = libusb20_dev_alloc_config(dev,
		    libusb20_dev_get_config_index(dev));
		if (dd->bDeviceClass == HUB_CLASS || cfg == NULL) {
			sh_log("UsbGrant: %d.%d (%04x:%04x) is a hub, or has no "
			    "configuration", bus, address, *vendor, *product);
		} else {
			rc = SH_OK;
			for (i = 0; i < cfg->num_interface; i++) {
				if (cfg->interface[i].desc.bInterfaceClass ==
				    HUB_CLASS ||
				    /* 0: a driver is attached. */
				    libusb20_dev_kernel_driver_active(dev,
				    (uint8_t)i) == 0) {
					sh_log("UsbGrant: %d.%d (%04x:%04x): "
					    "interface %d has a kernel driver",
					    bus, address, *vendor, *product, i);
					rc = SH_ERR_INVALID;
					break;
				}
			}
		}
		free(cfg);
		libusb20_dev_close(dev);
		break;
	}
	libusb20_be_free(be);
	if (!found)
		sh_log("UsbGrant: no USB device %d.%d", bus, address);
	return (rc);
}

/* Is the directory open on fd a devfs? */
static bool
on_devfs(int fd)
{
	struct statfs sf;

	return (fstatfs(fd, &sf) == 0 &&
	    strcmp(sf.f_fstypename, "devfs") == 0);
}

/* name in dir fd to uid, 0600, without following a link; character
 * devices (and, for /dev/ugen*, devfs's link) only. */
static bool
give(int fd, const char *dir, const char *name, uid_t uid, bool link_ok)
{
	struct stat st;

	if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (false);
	if (!S_ISCHR(st.st_mode) && !(link_ok && S_ISLNK(st.st_mode))) {
		sh_log("UsbGrant: %s/%s isn't a device node; left alone", dir,
		    name);
		return (false);
	}
	if (fchownat(fd, name, uid, (gid_t)-1, AT_SYMLINK_NOFOLLOW) != 0 ||
	    (S_ISCHR(st.st_mode) &&
	    fchmodat(fd, name, 0600, AT_SYMLINK_NOFOLLOW) != 0)) {
		sh_log("UsbGrant: %s/%s: %s", dir, name, strerror(errno));
		return (false);
	}
	return (true);
}

int
sh_usb_grant(const struct sh_caller *caller, int bus, int address)
{
	char prefix[32], name[32];
	unsigned vendor = 0, product = 0, bb, aa, ep;
	struct dirent *e;
	DIR *d;
	int fd, rc, n = 0;

	if (!caller->root && !caller->in_console)
		return (SH_ERR_NOT_CONSOLE);
	if (bus < 0 || bus > 255 || address < 1 || address > 255)
		return (SH_ERR_INVALID);
	rc = unclaimed(bus, address, &vendor, &product);
	if (rc != SH_OK)
		return (rc);

	fd = open(USB_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0 || !on_devfs(fd)) {
		sh_log("UsbGrant: %s isn't devfs", USB_DIR);
		if (fd >= 0)
			(void)close(fd);
		return (SH_ERR_FAILED);
	}
	d = fdopendir(fd);
	if (d == NULL) {
		(void)close(fd);
		return (SH_ERR_FAILED);
	}
	(void)snprintf(prefix, sizeof(prefix), "%d.%d.", bus, address);
	while ((e = readdir(d)) != NULL) {
		/* Exactly <bus>.<address>.<endpoint>. */
		if (strncmp(e->d_name, prefix, strlen(prefix)) != 0 ||
		    sscanf(e->d_name, "%u.%u.%u", &bb, &aa, &ep) != 3)
			continue;
		(void)snprintf(name, sizeof(name), "%u.%u.%u", bb, aa, ep);
		if (strcmp(name, e->d_name) != 0)
			continue;
		if (give(dirfd(d), USB_DIR, e->d_name, caller->euid, false))
			n++;
	}
	(void)closedir(d);
	if (n == 0)
		return (SH_ERR_FAILED);

	/* devfs's /dev/ugen<bus>.<address> link to the control node. */
	fd = open(DEV_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd >= 0) {
		(void)snprintf(name, sizeof(name), "ugen%d.%d", bus, address);
		if (on_devfs(fd))
			(void)give(fd, DEV_DIR, name, caller->euid, true);
		(void)close(fd);
	}
	sh_log("pid %d: UsbGrant: %d.%d (%04x:%04x), %d nodes, to uid %u",
	    (int)caller->pid, bus, address, vendor, product, n,
	    (unsigned)caller->euid);
	return (SH_OK);
}
