/*
 * btstore.h — bluetoothd's pairing store and persisted policy (SCPreferences).
 *
 * "Keep it paired across reboots", for Classic and LE alike, in one store. Two
 * files under /Local/Library/Preferences/SystemConfiguration, the directory
 * wland's known networks live in, both written through SCPreferences (temp
 * file plus rename on every commit):
 *
 *   org.nextbsd.Bluetooth.plist       0644  policy and device metadata
 *   org.nextbsd.Bluetooth.keys.plist  0600  link keys, LTKs, IRKs, CSRKs
 *
 * The first is the counterpart of macOS's /Library/Preferences/
 * com.apple.Bluetooth.plist and keeps Apple's key names where Apple has one:
 *
 *   ControllerPowerState   int   1 on, 0 off. Missing means on: Bluetooth is
 *                                on by default, and a controller is powered
 *                                when it arrives unless this says 0.
 *   DiscoverableState      bool  inquiry scan on at power-up. Default false.
 *   AutoConnect            bool  reconnect paired devices on power-up, before
 *                                login included. Default true. Each device
 *                                also has its own AutoConnect.
 *   AutoSwitchAudio        bool  a headset that connects takes /dev/dsp, so
 *                                programs play to it (#218). Default true.
 *   A2DPSink               bool  be a Bluetooth speaker: advertise the A2DP
 *                                sink record and the Audio/Rendering CoD
 *                                bits, and accept a phone's stream (#219).
 *                                Default false.
 *   PairedDevices          dict  keyed by the address in BT_ADDR_FMT_KEY form
 *                                ("aa-bb-cc-dd-ee-ff"), each entry:
 *       Name               string
 *       AddressType        string  "public" | "random"
 *       Transports         array   "BR/EDR", "LE"
 *       ClassOfDevice      int     Classic CoD
 *       Appearance         int     LE GAP appearance
 *       PnPID              dict    VendorIDSource, VendorID, ProductID, Version
 *       Profiles           array   "HID", "HOGP", "A2DPSink", ... (engine.c)
 *       HIDDescriptor      data    cached report descriptor, so a paired
 *                                  keyboard reconnects with no SDP/GATT query
 *       HIDVirtualCable, HIDReconnectInitiate, HIDNormallyConnectable,
 *       HIDBootDevice      bool    the SDP HID record's booleans
 *       HIDCountryCode     int
 *       HIDSessionEstablished bool a HID session has been up once: from
 *                                  then on a device that reconnects itself
 *                                  is left to (connections.h)
 *       AutoConnect        bool
 *       Trusted            bool    accept connections it initiates
 *       Audio              bool    play to it when it connects (an A2DP
 *                                  sink); absent means true (#218)
 *       Paired             int     unix time
 *       LastConnected      int     unix time
 *
 * The second holds the secrets and is written only by keystore.c (see
 * keystore.h). It is plaintext: NextBSD has no keychain. That is the same debt
 * WLAN accepted (src/WLAN/airport.h), with one improvement: the secrets have
 * their own file behind one module, so a keychain replaces keystore.c and the
 * metadata stays readable by tools.
 *
 * Every save commits immediately: a pairing must survive a power cut. A
 * missing or corrupt file reads as empty (SCPreferences starts fresh), so
 * bluetoothd never fails to start because of its store.
 */
#ifndef _BLUETOOTHD_BTSTORE_H_
#define _BLUETOOTHD_BTSTORE_H_

#include <stdbool.h>

#include "engine.h"

#ifndef BT_PREFS_DIR
#define	BT_PREFS_DIR		"/Local/Library/Preferences/SystemConfiguration"
#endif
#define	BT_PREFS_PATH		BT_PREFS_DIR "/org.nextbsd.Bluetooth.plist"
#define	BT_KEYS_PATH		BT_PREFS_DIR "/org.nextbsd.Bluetooth.keys.plist"

/* The persisted policy, with defaults applied. */
struct bt_prefs {
	int	power_state;		/* ControllerPowerState: 1 on, 0 off */
	bool	discoverable;		/* DiscoverableState */
	bool	autoconnect;		/* AutoConnect */
	bool	autoswitch_audio;	/* AutoSwitchAudio: a headset takes
					   /dev/dsp (#218) */
	bool	a2dp_sink;		/* A2DPSink: we are a speaker (#219) */
};

/*
 * Call once at startup, before any other btstore or keystore call: sets the
 * process umask to 077, so the key file is never readable by others even for
 * the instant between its temp file and the rename, and makes sure the
 * preferences directory exists 0755.
 */
void	bt_store_init(void);

/* Read the policy. Missing keys get the defaults above. */
void	bt_prefs_load(struct bt_prefs *out);

/* Persist one policy key. 0 on success. */
int	bt_prefs_set_power_state(int on);
int	bt_prefs_set_discoverable(bool on);
int	bt_prefs_set_autoconnect(bool on);
int	bt_prefs_set_autoswitch_audio(bool on);
int	bt_prefs_set_a2dp_sink(bool on);

/* Number of PairedDevices entries. */
int	bt_store_paired_count(void);

/*
 * The idx-th paired device in address order, metadata only (no keys). Returns
 * false when idx is out of range.
 */
bool	bt_store_paired_at(int idx, struct bt_bond *out);

/*
 * Look a bond up by address. With `with_keys`, the secrets are merged in from
 * the key file. Returns false when the device is not paired.
 */
bool	bt_store_bond_find(const struct bt_addr *addr, bool with_keys,
	    struct bt_bond *out);

/*
 * Save a bond: its metadata to the policy file and its keys (those `flags`
 * says it has) to the key file. Replaces any existing entry. 0 on success.
 */
int	bt_store_bond_save(const struct bt_bond *b);

/* Forget a device: its metadata and all its keys. 0 on success. */
int	bt_store_bond_forget(const struct bt_addr *addr);

#endif /* _BLUETOOTHD_BTSTORE_H_ */
