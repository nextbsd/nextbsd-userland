/*
 * btstate.h — what bluetoothd publishes in the SCDynamicStore.
 *
 *   State:/Bluetooth                  the daemon (published here)
 *       State               string   "Idle" | "Active" | "NoIORegistry"
 *       Adapters            array    node names ("ubt0")
 *       PairedDevices       int
 *       ControllerPowerState int     the persisted policy
 *       DiscoverableState   bool
 *       AutoConnect         bool
 *       AutoSwitchAudio     bool
 *       A2DPSink            bool
 *
 *   State:/Bluetooth/Adapter/<node>   one controller (#207)
 *       Address, Name, Transport, VendorID, ProductID, State,
 *       Powered, Discoverable, Connectable, Scanning
 *   State:/Bluetooth/Device/<addr>    one paired or connected device (#209,
 *                                     #213); <addr> is BT_ADDR_FMT_KEY
 *       Address, Name, ClassOfDevice, Adapter, Paired, Connected,
 *       Connecting, Profiles (array: what runs over the link now, "HID"),
 *       RSSI (dBm, while connected and known)
 *   State:/Bluetooth/Device/<addr>/Audio   its audio, while it has any
 *                                     (#218, #219, #220, #525)
 *       Role ("Source": we play to it; "Sink": it plays to us), Route
 *       ("A2DP" | "HFP"), Active, Codec ("SBC"; on HFP "CVSD" | "mSBC"),
 *       Node ("dsp.bt0"), DefaultOutput (it owns /dev/dsp), AVRCP, Volume
 *       (0-127, AVRCP absolute volume, while known), HandsFree (the HFP
 *       service level connection is up), Microphone (its SCO link is up:
 *       Node records from the headset), Battery (percent, while the
 *       headset reports it)
 *   State:/Bluetooth/Adapter/<node>/A2DPSink   the speaker role (#219)
 *       Enabled, Peer (the address playing to us, while one does)
 *   State:/Bluetooth/Pairing/<addr>   a prompt waiting for the user (#209)
 *       Address, Method, Passkey, Expires
 *   State:/Bluetooth/Firmware/<ugen>  a firmware download (#202)
 *       VendorID, ProductID, State ("downloading", "done", "firmware-failed")
 *
 * Each change is followed by a notify_post(3) of the matching
 * org.nextbsd.bluetooth.* name, so a client can wait instead of polling.
 *
 * Only summaries go here. CONFIG_DATA_MAX caps a value at 8 KiB, so lists
 * (scan results, the paired list) go over MIG.
 *
 * configd may not be up when bluetoothd starts: there is no launchd service
 * ordering. Every call opens the store lazily and tolerates its absence, the
 * pattern wland's airport.c uses.
 */
#ifndef _BLUETOOTHD_BTSTATE_H_
#define _BLUETOOTHD_BTSTATE_H_

#include <stdbool.h>
#include <stddef.h>

#include "bluetooth_mig_types.h"
#include "btstore.h"
#include "engine.h"

#define	BT_STATE_KEY		"State:/Bluetooth"
#define	BT_STATE_ADAPTER_PREFIX	BT_STATE_KEY "/Adapter/"	/* + node */
#define	BT_STATE_DEVICE_PREFIX	BT_STATE_KEY "/Device/"	/* + addr key */
#define	BT_STATE_PAIRING_PREFIX	BT_STATE_KEY "/Pairing/"	/* + addr key */
#define	BT_STATE_FIRMWARE_PREFIX BT_STATE_KEY "/Firmware/"	/* + ugen */

#define	BT_NOTIFY_ADAPTER	"org.nextbsd.bluetooth.adapter"
#define	BT_NOTIFY_SCAN		"org.nextbsd.bluetooth.scan"
#define	BT_NOTIFY_PAIRING	"org.nextbsd.bluetooth.pairing"
#define	BT_NOTIFY_DEVICE	"org.nextbsd.bluetooth.device"
#define	BT_NOTIFY_FIRMWARE	"org.nextbsd.bluetooth.firmware"
#define	BT_NOTIFY_AUDIO		"org.nextbsd.bluetooth.audio"

struct bt_daemon_summary {
	bt_daemon_state_t	state;
	int			nadapters;
	const char * const	*adapters;	/* nadapters node names */
	int			npaired;
	struct bt_prefs		prefs;
};

/* Publish State:/Bluetooth. 0 on success, -1 when configd is unreachable. */
int	bt_state_publish_daemon(const struct bt_daemon_summary *s);

/* One controller, for State:/Bluetooth/Adapter/<node>. */
struct bt_adapter_summary {
	const char		*node;
	const char		*address;	/* "" until read */
	const char		*name;
	const char		*transport;
	int			vendor;
	int			product;
	const char		*state;
	bool			powered;
	bool			discoverable;
	bool			connectable;
	bool			scanning;
};

int	bt_state_publish_adapter(const struct bt_adapter_summary *s);

/* A parked prompt, at full key `key` (BT_STATE_PAIRING_PREFIX + addr). */
int	bt_state_publish_pairing(const char *key,
	    const struct bt_pair_prompt *p, const char *method);

/* One device, for State:/Bluetooth/Device/<addr> (connections.c). */
struct bt_device_summary {
	const struct bt_addr	*addr;
	const char		*name;		/* "" when unknown */
	uint32_t		class_of_device;
	const char		*adapter;	/* "" when not connected */
	bool			paired;
	bool			connected;
	bool			connecting;
	uint32_t		profiles;	/* BT_PROFILE_* running */
	int			rssi;		/* 127: unknown */
};

/* A device, at full key `key` (BT_STATE_DEVICE_PREFIX + addr). */
int	bt_state_publish_device(const char *key,
	    const struct bt_device_summary *s);

/* A device's audio, at full key `key` (BT_STATE_DEVICE_PREFIX + addr +
 * "/Audio"). */
struct audio_status;
int	bt_state_publish_audio(const char *key, const struct audio_status *s);

/* A controller's A2DP sink role; `peer` is NULL when nothing plays. */
int	bt_state_publish_a2dp_sink(const char *node, bool enabled,
	    const struct bt_addr *peer);

/* A firmware download (BT_STATE_FIRMWARE_PREFIX + ugen). */
int	bt_state_publish_firmware(const char *ugen, int vendor, int product,
	    const char *state);

/* notify_post(3) one of the BT_NOTIFY_* names. */
void	bt_state_notify(const char *name);

/*
 * The name a controller should advertise: the ComputerName configd holds,
 * else the short host name. Returns false when neither is known.
 */
bool	bt_state_computer_name(char *buf, size_t len);

/* Remove one key (a full key, e.g. BT_STATE_KEY). */
int	bt_state_remove(const char *key);

/* Release the store session. */
void	bt_state_close(void);

#endif /* _BLUETOOTHD_BTSTATE_H_ */
