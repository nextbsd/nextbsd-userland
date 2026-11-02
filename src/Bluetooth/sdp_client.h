/*
 * sdp_client.h — bluetoothd's SDP client: read a remote device's service
 * records.
 *
 * What bluetoothd needs from a peer's SDP database, read once at pair time and
 * kept in the pairing store so a reconnect needs no SDP round trip:
 *
 *   HID record (0x1124)   HIDDescriptorList (0x0206: the report descriptor),
 *                         ReconnectInitiate (0x0205), NormallyConnectable
 *                         (0x020d), ParserVersion (0x0201), VirtualCable,
 *                         BootDevice, country code, subclass, and the control
 *                         and interrupt PSMs
 *   PnP Information       VendorIDSource, VendorID, ProductID, Version
 *   (0x1200)
 *   service classes       AudioSink 0x110B, AudioSource 0x110A, A/V Remote
 *                         Control 0x110E / Target 0x110C / Controller 0x110F,
 *                         Handsfree 0x111E, HID 0x1124: the bond's Profiles
 *
 * One L2CAP channel to PSM 1, one ServiceSearchAttribute for every record that
 * uses L2CAP (every BR/EDR service does) with just the attributes above, and
 * continuation until the answer is complete. If a peer's server rejects that,
 * the HID and PnP records are asked for by class instead.
 *
 * Every call here BLOCKS: it may page the device (seconds) and waits for each
 * response (bounded by timeouts). Call it from a thread that may block: the
 * Mach service thread (a diagnostic), or sdp_client_refresh_bond(), which runs
 * the query on a thread of its own. Never from the main loop.
 *
 * Classic (BR/EDR) only: LE devices describe themselves over GATT (#205).
 */
#ifndef _BLUETOOTHD_SDP_CLIENT_H_
#define _BLUETOOTHD_SDP_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>

#include "engine.h"		/* struct bt_addr, struct bt_bond */

/* The HID record, as the HID host (N6) needs it. */
struct sdp_hid_info {
	bool		present;		/* the peer has a HID record */
	uint16_t	device_release;		/* 0x0200 */
	uint16_t	parser_version;		/* 0x0201 */
	uint8_t		subclass;		/* 0x0202 */
	uint8_t		country_code;		/* 0x0203 */
	bool		virtual_cable;		/* 0x0204 */
	bool		reconnect_initiate;	/* 0x0205: the device reconnects */
	bool		battery_power;		/* 0x0209 */
	bool		remote_wake;		/* 0x020a */
	uint16_t	profile_version;	/* 0x020b */
	uint16_t	supervision_timeout;	/* 0x020c, 0 if absent */
	bool		normally_connectable;	/* 0x020d: it page-scans */
	bool		boot_device;		/* 0x020e */
	uint16_t	control_psm;		/* 0x0004, normally 0x11 */
	uint16_t	interrupt_psm;		/* 0x000d, normally 0x13 */
	uint16_t	desc_len;		/* 0 if absent or too large */
	uint8_t		desc[BT_HID_DESC_MAX];	/* report descriptor (0x22) */
};

/* Everything one query learns. */
struct sdp_device_info {
	uint32_t		profiles;	/* BT_PROFILE_* (Classic ones) */
	bool			have_pnp;
	struct bt_pnp_id	pnp;
	struct sdp_hid_info	hid;
	int			nrecords;	/* records the peer returned */
};

/*
 * Query `peer` (a BR/EDR address). Returns 0, or an errno: EPROTONOSUPPORT
 * (no L2CAP stack), EHOSTUNREACH / ETIMEDOUT / ECONNREFUSED (could not reach
 * the device), EPROTO (the peer's SDP answer was malformed or an error).
 */
int	sdp_client_query(const struct bt_addr *peer, struct sdp_device_info *out);

/* The three questions on their own; each is one sdp_client_query(). */
int	sdp_client_fetch_hid(const struct bt_addr *peer,
	    struct sdp_hid_info *out);
int	sdp_client_fetch_pnp(const struct bt_addr *peer, struct bt_pnp_id *out);
int	sdp_client_detect_profiles(const struct bt_addr *peer,
	    uint32_t *profiles);

/*
 * Fold a query's results into a bond: the Classic Profiles bits are replaced
 * (HOGP is kept), the PnP ID, the HID report descriptor and the HID record's
 * booleans (hid_flags) are set with their BT_BOND_* flags. Nothing is saved.
 */
void	sdp_client_apply(const struct sdp_device_info *info,
	    struct bt_bond *b);

/*
 * The RFCOMM server channel (1..30) of `peer`'s first record of class
 * `service` (Handsfree 0x111E for the hands-free gateway), and its
 * SupportedFeatures (0x0311; 0 when absent). Returns 0, ENOENT when there is
 * no such record, or an errno as sdp_client_query.
 */
int	sdp_client_rfcomm_service(const struct bt_addr *peer, uint16_t service,
	    uint8_t *channel, uint16_t *features);

/*
 * Query and apply, synchronously. The caller saves the bond. Returns 0 or an
 * errno as sdp_client_query.
 */
int	sdp_client_update_bond(const struct bt_addr *peer, struct bt_bond *b);

/*
 * The pair-time hook: query `peer` on a detached thread, then, under
 * bluetoothd_lock(), merge the result into its stored bond (keys included),
 * save it and tell connections.c (conn_bond_refreshed_locked: a new HID
 * device gets its first session). Call it once the bond is saved (BT_EV_PAIRED). Does nothing if the
 * device is no longer paired by the time the answer arrives. Returns 0 if the
 * thread started.
 */
int	sdp_client_refresh_bond(const struct bt_addr *peer);

/*
 * Browse every record of `peer` and render it as text lines, for
 * `bluetooth sdp <addr>` (MIG 31050/31051, bodies in sdp_client.c). The lines
 * are kept until the next browse.
 */
int	sdp_client_browse(const struct bt_addr *peer, int *nrecords,
	    int *nlines);

#endif /* _BLUETOOTHD_SDP_CLIENT_H_ */
