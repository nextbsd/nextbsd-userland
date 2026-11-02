/*
 * firmware.h — firmware download for controllers that boot into a loader
 * (#202): Intel (VID 0x8087) and Realtek (VID 0x0bda and OEM ids).
 *
 * Such a controller has no driver until its firmware is in: ng_ubt refuses it
 * (ng_ubt_intel.c, ng_ubt_rtl.c), so it never becomes an
 * IOBluetoothHostController. The kernel publishes every USB device as an
 * IOUSBHostDevice (nextbsd-kernel#391), and iokit_watch.c matches those by
 * vendor id on the same notification port as the controllers. Each candidate
 * gets a worker thread here, off the main loop:
 *
 *   1. Load NgUbt.kext (KextManagerLoadKextWithIdentifier). The load returns
 *      after ng_ubt has probed every device already present, so from then on
 *      "a driver is attached" is a reliable answer and nothing races ng_ubt's
 *      own probe (which sends HCI commands of its own on Intel parts).
 *   2. Open the device through libusb, by its ugen node (the
 *      org.nextbsd.ugen property), and stop if ng_ubt has attached to
 *      interface 0: the controller is operational. This is iwmbtfw's and
 *      rtlbtfw's own test.
 *   3. Download: fw_intel.c (iwmbtfw) or fw_rtl.c (rtlbtfw). The Intel code
 *      reads the boot state from the controller itself.
 *   4. Reset the device. It re-enumerates in operational mode, ng_ubt
 *      attaches and the IOBluetoothHostController arrival brings it up
 *      (adapter.c); that second IOUSBHostDevice arrival stops at step 2.
 *
 * Every USB transfer has a timeout (2 s for a command, 5 s for the load
 * complete), so a wedged controller fails the download, never the daemon.
 * Downloads run one at a time. The outcome is logged and published as
 * State:/Bluetooth/Firmware/<ugen>, with State "firmware-failed" on failure.
 *
 * No iwmbtfw or rtlbtfw binary is ever run, and there is no devd.
 */
#ifndef _BLUETOOTHD_FIRMWARE_H_
#define _BLUETOOTHD_FIRMWARE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bluetooth_mig_types.h"
#include "iokit_watch.h"

/* Where NgUbt.kext carries the linux-firmware files (nextbsd#476). */
#define	FW_DIR		"/System/Library/Extensions/NgUbt.kext/Contents/" \
			"Resources/firmware"
#define	FW_DIR_INTEL	FW_DIR "/intel"
#define	FW_DIR_RTL	FW_DIR "/rtl_bt"

#define	FW_KEXT_UBT	"org.nextbsd.net.NgUbt"

/* What the download code returns. */
#define	FW_RESULT_DOWNLOADED	0
#define	FW_RESULT_FAILED	1
#define	FW_RESULT_OPERATIONAL	2	/* it needed none */

/* An IOUSBHostDevice arrival (main loop). Starts a worker if it is ours. */
void	firmware_usb_arrived(const struct bt_iokit_event *ev);

/* The idx-th device the loader has looked at. False past the end. */
bool	firmware_info(int idx, char *ugen, size_t ugenlen, int *vendor,
	    int *product, bt_firmware_state_t *state);

/* Publish what changed (main loop). */
void	firmware_publish(void);

/* Log a line, "bluetoothd[firmware] ...". Safe from the workers. */
void	fw_log(const char *fmt, ...) __attribute__((__format__(__printf__,
	    1, 2)));

#endif /* _BLUETOOTHD_FIRMWARE_H_ */
