/*
 * iokit_watch.h — follow Bluetooth controllers through the IORegistry.
 *
 * NgUbt.kext publishes each controller as an IOBluetoothHostController entry
 * under its ubtN device node once the netgraph node exists, and retires it on
 * detach (nextbsd-kernel-extensions#27). bluetoothd matches that class with
 * IOServiceAddMatchingNotification, first-match and terminated, the way wland
 * follows IO80211Controller. No NGM_LISTNODES polling, no devd.
 *
 * A controller still in its bootloader (Intel, Realtek, waiting for firmware)
 * has no IOBluetoothHostController: ng_ubt refuses it. It is seen as an
 * IOUSBHostDevice instead (nextbsd-kernel#391), so the same notification port
 * also watches IOUSBHostDevice first-match for the vendor ids the firmware
 * loader knows (fw_ids.c), and hands those to firmware.c (#202).
 */
#ifndef _BLUETOOTHD_IOKIT_WATCH_H_
#define _BLUETOOTHD_IOKIT_WATCH_H_

#include <stdbool.h>
#include <stdint.h>

#include "bluetooth_mig_types.h"	/* BTNodeName, BTTransportName */

#define	BT_IOKIT_CLASS		"IOBluetoothHostController"
#define	BT_IOKIT_USB_CLASS	"IOUSBHostDevice"

/*
 * Properties read from the entry. Any of them may be missing; the node name
 * then falls back to the entry's own name, and the rest stay zero or empty.
 */
#define	BT_IOKIT_KEY_NODE	"org.nextbsd.bluetooth.NodeName"
#define	BT_IOKIT_KEY_TRANSPORT	"Transport"
#define	BT_IOKIT_KEY_VENDOR	"idVendor"
#define	BT_IOKIT_KEY_PRODUCT	"idProduct"
#define	BT_IOKIT_KEY_UGEN	"org.nextbsd.ugen"
/* IOUSBHostDevice: the first interface's class triple (nextbsd-kernel#391). */
#define	BT_IOKIT_KEY_IFCLASS	"bInterfaceClass"
#define	BT_IOKIT_KEY_IFSUBCLASS	"bInterfaceSubClass"
#define	BT_IOKIT_KEY_IFPROTOCOL	"bInterfaceProtocol"

typedef enum {
	BT_IOKIT_ARRIVED	= 1,	/* IOBluetoothHostController */
	BT_IOKIT_DEPARTED	= 2,
	BT_IOKIT_USB_ARRIVED	= 3	/* IOUSBHostDevice, a firmware candidate */
} bt_iokit_event_kind_t;

/*
 * One arrival or departure. A departure carries only entry_id: the entry is
 * gone, so its properties can no longer be read. entry_id is the IORegistry
 * entry ID, unique for the boot, and is what bluetoothd keys controllers on.
 */
struct bt_iokit_event {
	bt_iokit_event_kind_t	kind;
	uint64_t		entry_id;
	BTNodeName		node;		/* "ubt0" */
	BTTransportName		transport;	/* "USB" */
	uint16_t		vendor;
	uint16_t		product;
	char			ugen[32];	/* "ugen0.3" */
	uint8_t			if_class[3];	/* USB only: class, sub, proto */
};

/*
 * Register both notifications and drain their arming iterators, so a
 * controller that already exists is delivered as an arrival. Callbacks run on
 * a serial dispatch queue; each reads what it needs from the entry, queues an
 * event and writes a byte to `wake_fd` (the write end of the main loop's
 * self-pipe).
 *
 * Returns 0 when armed, -1 when /dev/ioregistry is absent or a registration
 * failed. There is no fallback: without the registry no controller is ever
 * seen, and bluetoothd idles.
 */
int	iokit_watch_start(int wake_fd);

/*
 * Take the oldest queued event. Returns false when the queue is empty. Called
 * from the main loop after the self-pipe wakes it.
 */
bool	iokit_watch_next(struct bt_iokit_event *ev);

#endif /* _BLUETOOTHD_IOKIT_WATCH_H_ */
