/*
 * hid_host.h — bluetoothd's Classic HID host (#524): HIDP over L2CAP PSM 0x11
 * (control) and 0x13 (interrupt) into IOHIDUserDevice. It replaces bthidd.
 *
 * One session per connected HID device:
 *
 *   connect      host-initiated, the first time after pairing (and for a
 *                device that does not reconnect by itself): control, then
 *                interrupt, on a thread of their own (paging blocks)
 *   accept       device-initiated: bluetoothd listens on PSM 0x11 and 0x13 on
 *                every controller, from the moment the L2CAP sockets are
 *                loaded, so a paired keyboard that pages us is taken whether
 *                or not anyone is logged in
 *   device       once both channels are up, one IOHIDUserDevice from the
 *                bond's cached report descriptor (SDP, at pair time), with
 *                VendorID/ProductID/VersionNumber from the PnP ID, Transport
 *                "Bluetooth", SerialNumber = the address, Product = the name.
 *                hidbus attaches hkbd (and so kbdmux: the console and the
 *                login window), hms, hmt, hcons as for USB.
 *   input        interrupt DATA(Input) -> IOHIDUserDeviceHandleReport
 *   set report   Output reports (keyboard LEDs) -> DATA(Output) on the
 *                interrupt channel; Feature -> SET_REPORT on control, and
 *                the HANDSHAKE is the answer
 *   get report   GET_REPORT on control; the DATA reply, or the HANDSHAKE
 *                error, is the answer
 *   no desc      a session that comes up before the bond has its descriptor
 *                (a keyboard that connects itself right after pairing) waits
 *                for the SDP query; a boot keyboard or mouse that never gives
 *                one is switched to the boot protocol with a built-in
 *                descriptor
 *   unplug       HID_CONTROL VIRTUAL_CABLE_UNPLUG from the device ends the
 *                session and forgets the device; unpairing a virtual-cable
 *                device here sends it the same
 *   disconnect   interrupt, then control; the IOHIDUserDevice is cancelled,
 *                and the kernel releases any key still held
 *
 * Threads: the sessions live on one serial dispatch queue; each device's
 * get/set-report handlers run on a queue of the device's own, because they
 * wait (bounded) for the HIDP answer that the session queue reads. Calls in
 * from the policy layer (connections.c) are made with bluetoothd_lock() held
 * and never wait for the session queue; the session queue takes
 * bluetoothd_lock() itself to report a session up or down.
 */
#ifndef _BLUETOOTHD_HID_HOST_H_
#define _BLUETOOTHD_HID_HOST_H_

#include <stdbool.h>

#include "bluetooth_mig_types.h"
#include "engine.h"

#define	HID_PSM_CONTROL		0x0011
#define	HID_PSM_INTERRUPT	0x0013

/*
 * Open the PSM 0x11/0x13 listeners. Safe to call again; fails quietly (and
 * returns -1) while the L2CAP sockets are not loaded, so the controller
 * bring-up calls it once it has loaded them.
 */
int		hid_host_start(void);

/*
 * Connect to `peer` through the controller with address `local` (NULL: any).
 * Returns at once; the session reports itself through
 * conn_profile_locked(). Lock held.
 */
bt_status_t	hid_host_connect(const struct bt_addr *local,
		    const struct bt_addr *peer);

/*
 * Close the session with `peer`, if there is one or one is being opened;
 * with `then_acl`, disconnect the ACL link once the channels are closed.
 * Returns false when there was nothing to close. Lock held.
 */
bool		hid_host_disconnect(const struct bt_addr *peer, bool then_acl);

/*
 * Unpairing: send VIRTUAL_CABLE_UNPLUG if the device is a virtual-cable one
 * with a session up, then close the session. Lock held.
 */
void		hid_host_unplug(const struct bt_addr *peer);

/* Is a session with `peer` up, or being opened? Lock held. */
bool		hid_host_active(const struct bt_addr *peer);

/*
 * The bond of `peer` changed (the SDP query brought its descriptor): a
 * session waiting for it carries on. Lock held.
 */
void		hid_host_bond_updated(const struct bt_addr *peer);

#endif /* _BLUETOOTHD_HID_HOST_H_ */
