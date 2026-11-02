/*
 * connections.h — the policy layer's side of connections (#213): which
 * devices are connected, who may connect, what reconnects by itself, and
 * State:/Bluetooth/Device/<addr>.
 *
 *   links        the engines raise BT_EV_CONNECTED, BT_EV_DISCONNECTED and
 *                BT_EV_LINK_CHANGED (links.c); this table keeps one row per
 *                device with its controller, handle, encryption, RSSI and
 *                the profiles running over it (HID, from hid_host.c;
 *                A2DP and AVRCP, from audio.c)
 *   trust        an incoming link from a device that is not paired and
 *                Trusted, and not the pairing in progress, is disconnected
 *                and logged (the kernel has already accepted it)
 *   connect      bt_connect: a paired HID device gets the HID host's
 *                control and interrupt channels, anything else an ACL link;
 *                returns once the attempt has started (the wlan_connect rule)
 *   disconnect   bt_disconnect: the HID session first, then the link
 *   audio        every link up and down, and every bond refresh, is passed
 *                to audio.c, which runs the A2DP source's mixer while a
 *                paired sink's link is up (#218)
 *   reconnect    when a controller comes up powered, every paired device
 *                whose AutoConnect is on (and the global AutoConnect) is
 *                connected once, CONN_AUTO_GAP_MS apart. A HID device is
 *                connected by the host only until its first HID session;
 *                after that, if its SDP record says it reconnects itself
 *                (HIDReconnectInitiate), the host only listens (hid_host.c)
 *   first        a new bond's SDP query (sdp_client_refresh_bond) finishing
 *                connects a HID device for its first session
 *   publish      State:/Bluetooth/Device/<addr> =
 *                  { Address, Name, ClassOfDevice, Adapter, Paired,
 *                    Connected, Profiles, RSSI }
 *                for every paired or connected device, rewritten on every
 *                change, then notify_post("org.nextbsd.bluetooth.device").
 *                A device that is neither has no key.
 *
 * Every *_locked call expects bluetoothd_lock() held.
 */
#ifndef _BLUETOOTHD_CONNECTIONS_H_
#define _BLUETOOTHD_CONNECTIONS_H_

#include <stdbool.h>
#include <stdint.h>

#include "bluetooth_mig_types.h"
#include "bluetoothd.h"
#include "engine.h"

#define	CONN_AUTO_GAP_MS	10000	/* between automatic reconnects */
#define	CONN_AUTO_FIRST_MS	1000	/* the first, after power-up */
#define	CONN_CONNECT_GUARD_MS	45000	/* "connecting" gives up after */

/* What bt_device_status reports about one device. */
struct conn_status {
	bt_device_state_t	state;
	char			node[32];	/* "" when not connected */
	int			rssi;		/* dBm; 127 when unknown */
	uint32_t		profiles;	/* BT_PROFILE_* running now */
	uint32_t		class_of_device; /* from the link, if known */
	bool			encrypted;
};

/* A link event from controller `a`'s engine. */
void		conn_event_locked(struct bt_adapter *a,
		    const struct bt_event *ev);

/* The RPCs. */
bt_status_t	conn_connect_locked(const struct bt_addr *peer);
bt_status_t	conn_disconnect_locked(const struct bt_addr *peer);

/* Drop the ACL link to `peer`, wherever it is (the HID host's last step). */
void		conn_acl_disconnect_locked(const struct bt_addr *peer);

/* Fill `out`; false when the device is not connected or connecting. */
bool		conn_status_locked(const struct bt_addr *peer,
		    struct conn_status *out);
bt_device_state_t conn_state_locked(const struct bt_addr *peer);

/* A controller came up powered, or went off or away. */
void		conn_adapter_up_locked(struct bt_adapter *a);
void		conn_adapter_down_locked(struct bt_adapter *a);

/*
 * A profile started or stopped over the link to `peer` (the HID host). A
 * profile that failed to start before any link came up ends "connecting".
 */
void		conn_profile_locked(const struct bt_addr *peer,
		    uint32_t profile, bool up);

/* A device was paired (its SDP query has been started). */
void		conn_paired_locked(const struct bt_addr *peer);

/* The SDP query of a bond finished and the bond was saved. */
void		conn_bond_refreshed_locked(const struct bt_addr *peer);

/* Republish State:/Bluetooth/Device/<addr> (paired, unpaired, renamed). */
void		conn_device_changed_locked(const struct bt_addr *peer);

/* Publish every paired device (at startup: the dynamic store is empty). */
void		conn_publish_paired_locked(void);

uint64_t	conn_next_timer_locked(void);
void		conn_timers_locked(uint64_t now);

/* Publish what changed. Main loop, lock NOT held. */
void		conn_publish(void);

#endif /* _BLUETOOTHD_CONNECTIONS_H_ */
