/*
 * bluetooth_mig_types.h — C typedefs for the named MIG types in
 * bluetooth.defs, and the enums that cross the Mach line.
 *
 * MIG records the wire layout of a `type` declaration but emits no C typedef
 * for it: the generated stubs use the name as-is and expect an imported header
 * to define it. bluetooth.defs imports this header so the generated server
 * (bluetoothServer.{c,h}) and user (bluetoothUser.c / bluetooth.h) sides agree.
 * Same arrangement as wlan_mig_types.h.
 *
 * Everything is a FIXED-SIZE array or a scalar: MIG out-of-line data is broken
 * in this kernel (ipconfig.defs says so), so lists go over the flat
 * count-then-entry(i) pattern wlan.defs uses.
 *
 * The enums are defined in full here, ahead of the routines that use them, so
 * later tickets only append routines and values. Values are wire contract:
 * append new ones, never renumber. bluetoothd's engines (engine.h) use the same
 * enums, so a pairing prompt or a status code means the same thing from the
 * HCI event to the CLI.
 *
 * This header is pure C with no CoreFoundation: the bluetooth(8) CLI includes
 * it and links only liblaunch and libsystem_kernel.
 */
#ifndef _BLUETOOTH_MIG_TYPES_H
#define _BLUETOOTH_MIG_TYPES_H

#include <stdint.h>

/* Netgraph node of a controller, as IOBluetoothHostController names it: "ubt0". */
typedef char	BTNodeName[32];

/*
 * A device address as text. Long enough for "AA:BB:CC:DD:EE:FF/random" plus a
 * NUL, with room to spare. bt_addr_ntoa() (engine.h) is the only formatter.
 */
typedef char	BTAddressString[32];

/* An HCI local or remote name: up to 248 octets of UTF-8, plus a NUL. */
#define	BT_NAME_MAX	248
typedef char	BTDeviceName[BT_NAME_MAX + 1];

/*
 * Profiles a device offers, comma-separated ("HID,A2DPSink"). The names are
 * the ones the pairing store writes into PairedDevices/<addr>/Profiles.
 */
typedef char	BTProfileList[128];

/* Transport of a controller: "USB" (later "UART"). */
typedef char	BTTransportName[16];

/*
 * One line of `bluetooth sdp <addr>` output (bt_sdp_browse_line): the daemon
 * renders the peer's records as text, so the CLI needs no SDP parser.
 */
#define	BT_SDP_LINE_MAX	256
typedef char	BTSDPLine[BT_SDP_LINE_MAX];

/* A legacy PIN: 1..16 characters, plus a NUL. */
typedef char	BTPinString[17];

/* A USB device node, as the IORegistry names it: "ugen0.3". */
typedef char	BTUSBDeviceName[32];

/* LMP features, page 0, as Read_Local_Supported_Features returns them. */
typedef char	BTFeatures[8];

/*
 * HCI command parameters or return parameters for bt_hci_command: up to 255
 * octets (NG_HCI_CMD_PKT_SIZE), with the length passed beside it.
 */
#define	BT_HCI_DATA_MAX	255
typedef char	BTHCIData[256];

/* Daemon state, as `bluetooth status` and State:/Bluetooth report it. */
typedef enum {
	bt_daemon_idle_e		= 0,	/* running, no controller */
	bt_daemon_active_e		= 1,	/* at least one controller */
	bt_daemon_no_ioregistry_e	= 2	/* no /dev/ioregistry: blind */
} bt_daemon_state_t;

/* One controller's state. */
typedef enum {
	bt_adapter_arrived_e		= 0,	/* matched, not brought up yet */
	bt_adapter_firmware_e		= 1,	/* waiting for firmware download */
	bt_adapter_initializing_e	= 2,	/* graph build + HCI init */
	bt_adapter_powered_on_e		= 3,
	bt_adapter_powered_off_e	= 4,
	bt_adapter_error_e		= 5
} bt_adapter_state_t;

/* One remote device's link state. */
typedef enum {
	bt_device_disconnected_e	= 0,
	bt_device_connecting_e		= 1,
	bt_device_connected_e		= 2,
	bt_device_pairing_e		= 3
} bt_device_state_t;

/*
 * Transports a device was seen or bonded on, as a bitmask. A dual-mode device
 * (a headset that also speaks LE) is one device with both bits.
 */
#define	BT_TRANSPORT_BREDR	0x1
#define	BT_TRANSPORT_LE		0x2

/*
 * The one pairing-prompt shape. Legacy PIN, Secure Simple Pairing and LE SMP
 * all reduce to one of these; the daemon parks the prompt and a client fetches
 * and answers it (the pull model: no Mach-speaking UI exists).
 */
typedef enum {
	bt_pair_none_e			= 0,	/* nothing pending */
	bt_pair_just_works_e		= 1,	/* confirm yes/no, no number */
	bt_pair_numeric_comparison_e	= 2,	/* both sides show 6 digits */
	bt_pair_passkey_entry_e		= 3,	/* type the 6 digits the peer shows */
	bt_pair_passkey_display_e	= 4,	/* show 6 digits; type them on the peer */
	bt_pair_pin_e			= 5	/* legacy PIN, 1..16 characters */
} bt_pair_method_t;

/*
 * Where a pairing has got to, for bt_pair_status. A finished pairing stays
 * reported (succeeded or failed, with its status) for a while, so a client
 * that polls slowly still sees how it ended.
 */
typedef enum {
	bt_pair_state_none_e		= 0,	/* no pairing with this device */
	bt_pair_state_pending_e		= 1,	/* connecting, authenticating */
	bt_pair_state_waiting_e		= 2,	/* a prompt waits for the user */
	bt_pair_state_succeeded_e	= 3,
	bt_pair_state_failed_e		= 4
} bt_pair_state_t;

/* Firmware download for a controller in its bootloader (Intel, Realtek). */
typedef enum {
	bt_firmware_none_e		= 0,	/* not looked at */
	bt_firmware_checking_e		= 1,	/* reading the boot state */
	bt_firmware_downloading_e	= 2,
	bt_firmware_done_e		= 3,	/* downloaded; re-enumerating */
	bt_firmware_operational_e	= 4,	/* needed none */
	bt_firmware_failed_e		= 5
} bt_firmware_state_t;

/* Controller capability bits, as bt_adapter_flags reports them. */
#define	BT_ADAPTER_FEAT_SSP	0x1	/* Secure Simple Pairing */
#define	BT_ADAPTER_FEAT_LE	0x2	/* Low Energy */
#define	BT_ADAPTER_FEAT_EIR	0x4	/* Extended Inquiry Response */

/*
 * Status codes. Shape follows wlan_status_t / ipconfig_status_t so the CLI's
 * error handling reads the same.
 */
typedef enum {
	bt_status_success_e			= 0,
	bt_status_invalid_parameter_e		= 1,
	bt_status_no_adapter_e			= 2,
	bt_status_index_out_of_range_e		= 3,
	bt_status_internal_error_e		= 4,
	bt_status_operation_not_supported_e	= 5,
	bt_status_powered_off_e			= 6,
	bt_status_busy_e			= 7,
	bt_status_not_paired_e			= 8,
	bt_status_not_connected_e		= 9,
	bt_status_no_such_device_e		= 10,
	bt_status_pairing_failed_e		= 11,
	bt_status_pairing_timeout_e		= 12,
	bt_status_pairing_rejected_e		= 13,
	bt_status_no_prompt_e			= 14,	/* nothing to confirm */
	bt_status_store_error_e			= 15
} bt_status_t;

#endif /* _BLUETOOTH_MIG_TYPES_H */
