/*
 * bluetoothd_mig.c — server-side bodies for bluetooth.defs.
 *
 * The MIG-generated demux _bluetooth_server() (in bluetoothServer.c) calls
 * these per routine. They run on the Mach service thread. Anything the main
 * loop writes (the controller table, the cached policy) is read under
 * bluetoothd_lock(). The pairing store is read directly: each call opens its
 * own SCPreferences and the file is replaced by rename, so a reader never sees
 * half a write.
 *
 * A routine reports its outcome in its `status` out-argument and returns
 * KERN_SUCCESS; a non-success kern_return_t would mean the RPC itself failed.
 * Every fixed-size out-array is zeroed first, so no stale stack bytes cross
 * the Mach line.
 */
#include "bluetoothd.h"
#include "adapter.h"
#include "audio.h"
#include "btstore.h"
#include "bluetooth_mig_types.h"
#include "connections.h"
#include "diag.h"
#include "discovery.h"
#include "engine.h"
#include "firmware.h"
#include "pairing.h"

#include <mach/mach.h>
#include "bluetoothServer.h"		/* MIG-emitted prototypes */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A fixed-size string from the wire, NUL-terminated whatever was sent. */
#define	WIRE_STR(dst, src)	do {					\
	(void)memcpy((dst), (src), sizeof(dst));			\
	(dst)[sizeof(dst) - 1] = '\0';					\
} while (0)

/* ---- 31000-31009: daemon and controllers ------------------------------- */

kern_return_t
_bt_status(mach_port_t server, bt_daemon_state_t *state, int *adapters,
    int *paired, int *power_state, int *discoverable, int *autoconnect)
{
	struct bt_prefs p;

	(void)server;

	bluetoothd_lock();
	*state = bluetoothd_state_locked();
	*adapters = bluetoothd_adapter_count_locked();
	bluetoothd_prefs_locked(&p);
	bluetoothd_unlock();

	*paired = bt_store_paired_count();
	*power_state = p.power_state;
	*discoverable = p.discoverable ? 1 : 0;
	*autoconnect = p.autoconnect ? 1 : 0;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_adapter_count(mach_port_t server, int *count)
{
	(void)server;

	bluetoothd_lock();
	*count = bluetoothd_adapter_count_locked();
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_adapter_info(mach_port_t server, int index, BTNodeName node,
    BTTransportName transport, int *vendor, int *product,
    bt_adapter_state_t *state, BTAddressString address, BTDeviceName name,
    bt_status_t *status)
{
	struct bt_adapter *a;

	(void)server;
	(void)memset(node, 0, sizeof(BTNodeName));
	(void)memset(transport, 0, sizeof(BTTransportName));
	(void)memset(address, 0, sizeof(BTAddressString));
	(void)memset(name, 0, sizeof(BTDeviceName));
	*vendor = 0;
	*product = 0;
	*state = bt_adapter_arrived_e;

	bluetoothd_lock();
	a = bluetoothd_adapter_at_locked(index);
	if (a == NULL) {
		*status = bt_status_index_out_of_range_e;
	} else {
		(void)strlcpy(node, a->node, sizeof(BTNodeName));
		(void)strlcpy(transport, a->transport, sizeof(BTTransportName));
		*vendor = a->vendor;
		*product = a->product;
		*state = a->state;
		if (a->have_addr)
			(void)bt_addr_ntoa(&a->addr, BT_ADDR_FMT_DISPLAY,
			    address, sizeof(BTAddressString));
		(void)strlcpy(name, a->name, sizeof(BTDeviceName));
		*status = bt_status_success_e;
	}
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_adapter_power(mach_port_t server, BTNodeName node_in, int on,
    bt_status_t *status)
{
	struct bt_adapter *a;
	struct bt_prefs p;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	bluetoothd_lock();
	a = bluetoothd_adapter_pick_locked(node);
	if (a == NULL && node[0] != '\0') {
		*status = bt_status_no_adapter_e;
	} else {
		/* Persisted first: a controller that arrives later obeys it. */
		bluetoothd_prefs_locked(&p);
		p.power_state = on ? 1 : 0;
		bluetoothd_prefs_update_locked(&p);
		*status = (bt_prefs_set_power_state(on ? 1 : 0) == 0) ?
		    bt_status_success_e : bt_status_store_error_e;
		if (a == NULL)
			*status = bt_status_no_adapter_e;
		else if (*status == bt_status_success_e)
			*status = adapter_set_power_locked(a, on != 0);
	}
	bluetoothd_unlock();
	bluetoothd_wake();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_adapter_discoverable(mach_port_t server, BTNodeName node_in, int on,
    int seconds, bt_status_t *status)
{
	struct bt_adapter *a;
	struct bt_prefs p;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	bluetoothd_lock();
	a = bluetoothd_adapter_pick_locked(node);
	if (a == NULL) {
		*status = bt_status_no_adapter_e;
	} else {
		*status = adapter_set_discoverable_locked(a, on != 0,
		    seconds > 0 ? seconds : 0);
		if (*status == bt_status_success_e && seconds <= 0) {
			bluetoothd_prefs_locked(&p);
			p.discoverable = (on != 0);
			bluetoothd_prefs_update_locked(&p);
			if (bt_prefs_set_discoverable(on != 0) != 0)
				*status = bt_status_store_error_e;
		}
	}
	bluetoothd_unlock();
	bluetoothd_wake();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_adapter_flags(mach_port_t server, int index, int *powered,
    int *discoverable, int *connectable, int *scanning, int *features,
    bt_status_t *status)
{
	struct bt_adapter *a;

	(void)server;
	*powered = *discoverable = *connectable = *scanning = *features = 0;
	bluetoothd_lock();
	a = bluetoothd_adapter_at_locked(index);
	if (a == NULL) {
		*status = bt_status_index_out_of_range_e;
	} else {
		*powered = a->powered ? 1 : 0;
		*discoverable = a->discoverable ? 1 : 0;
		*connectable = a->powered ? 1 : 0;
		*scanning = a->scanning ? 1 : 0;
		if ((a->features[6] & 0x08) != 0)
			*features |= BT_ADAPTER_FEAT_SSP;
		if ((a->features[4] & 0x40) != 0)
			*features |= BT_ADAPTER_FEAT_LE;
		if ((a->features[6] & 0x01) != 0)
			*features |= BT_ADAPTER_FEAT_EIR;
		*status = bt_status_success_e;
	}
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_firmware_info(mach_port_t server, int index, BTUSBDeviceName device,
    int *vendor, int *product, bt_firmware_state_t *state,
    bt_status_t *status)
{
	(void)server;
	(void)memset(device, 0, sizeof(BTUSBDeviceName));
	*vendor = *product = 0;
	*state = bt_firmware_none_e;
	*status = firmware_info(index, device, sizeof(BTUSBDeviceName), vendor,
	    product, state) ? bt_status_success_e :
	    bt_status_index_out_of_range_e;
	return (KERN_SUCCESS);
}

/* ---- 31010-31019: paired devices --------------------------------------- */

kern_return_t
_bt_paired_count(mach_port_t server, int *count, bt_status_t *status)
{
	(void)server;

	*count = bt_store_paired_count();
	*status = bt_status_success_e;
	return (KERN_SUCCESS);
}

/*
 * One paired device, by index, in address order. The flat one-entry-per-RPC
 * shape is forced by the broken-OOL constraint (bluetooth.defs), as for
 * wlan_scan_entry.
 */
kern_return_t
_bt_paired_entry(mach_port_t server, int index, BTAddressString address,
    BTDeviceName name, int *class_of_device, int *transports,
    BTProfileList profiles, int *autoconnect, bt_device_state_t *state,
    bt_status_t *status)
{
	struct bt_bond b;

	(void)server;
	(void)memset(address, 0, sizeof(BTAddressString));
	(void)memset(name, 0, sizeof(BTDeviceName));
	(void)memset(profiles, 0, sizeof(BTProfileList));
	*class_of_device = 0;
	*transports = 0;
	*autoconnect = 0;
	*state = bt_device_disconnected_e;

	if (!bt_store_paired_at(index, &b)) {
		*status = bt_status_index_out_of_range_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	*state = conn_state_locked(&b.addr);
	bluetoothd_unlock();
	(void)bt_addr_ntoa(&b.addr, BT_ADDR_FMT_DISPLAY, address,
	    sizeof(BTAddressString));
	(void)strlcpy(name, b.name, sizeof(BTDeviceName));
	*class_of_device = (int)b.class_of_device;
	*transports = b.transports;
	bt_profiles_format(b.profiles, profiles, sizeof(BTProfileList));
	*autoconnect = b.autoconnect ? 1 : 0;
	*status = bt_status_success_e;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_unpair(mach_port_t server, BTAddressString address_in,
    bt_status_t *status)
{
	BTAddressString address;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	*status = adapter_unpair_locked(&a);
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_device_status(mach_port_t server, BTAddressString address_in,
    BTDeviceName name, int *class_of_device, bt_device_state_t *state,
    BTNodeName node, int *rssi, BTProfileList profiles, int *paired,
    bt_status_t *status)
{
	BTAddressString address;
	struct conn_status cs;
	struct bt_bond *b;
	struct bt_addr a;
	bool linked;

	(void)server;
	WIRE_STR(address, address_in);
	(void)memset(name, 0, sizeof(BTDeviceName));
	(void)memset(node, 0, sizeof(BTNodeName));
	(void)memset(profiles, 0, sizeof(BTProfileList));
	*class_of_device = 0;
	*state = bt_device_disconnected_e;
	*rssi = 127;
	*paired = 0;
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	b = calloc(1, sizeof(*b));
	if (b == NULL) {
		*status = bt_status_internal_error_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	linked = conn_status_locked(&a, &cs);
	bluetoothd_unlock();
	*paired = bt_store_bond_find(&a, false, b) ? 1 : 0;
	if (!*paired && a.type == BT_ADDR_PUBLIC) {
		/* An LE device whose identity is a static random address. */
		a.type = BT_ADDR_RANDOM;
		*paired = bt_store_bond_find(&a, false, b) ? 1 : 0;
	}
	if (!linked && !*paired) {
		free(b);
		*status = bt_status_no_such_device_e;
		return (KERN_SUCCESS);
	}
	if (*paired) {
		(void)strlcpy(name, b->name, sizeof(BTDeviceName));
		*class_of_device = (int)b->class_of_device;
	}
	if (*class_of_device == 0)
		*class_of_device = (int)cs.class_of_device;
	*state = cs.state;
	(void)strlcpy(node, cs.node, sizeof(BTNodeName));
	*rssi = cs.rssi;
	bt_profiles_format(cs.profiles, profiles, sizeof(BTProfileList));
	free(b);
	*status = bt_status_success_e;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_device_autoconnect(mach_port_t server, BTAddressString address_in, int on,
    bt_status_t *status)
{
	BTAddressString address;
	struct bt_bond *b;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	b = calloc(1, sizeof(*b));
	if (b == NULL) {
		*status = bt_status_internal_error_e;
		return (KERN_SUCCESS);
	}
	/* Under the lock: the main loop saves bonds too. */
	bluetoothd_lock();
	if (!bt_store_bond_find(&a, true, b))
		*status = bt_status_not_paired_e;
	else {
		b->autoconnect = (on != 0);
		*status = bt_store_bond_save(b) == 0 ? bt_status_success_e :
		    bt_status_store_error_e;
	}
	bluetoothd_unlock();
	(void)memset(b, 0, sizeof(*b));
	free(b);
	return (KERN_SUCCESS);
}

/* ---- 31020-31029: discovery --------------------------------------------- */

kern_return_t
_bt_scan(mach_port_t server, BTNodeName node_in, int seconds,
    bt_status_t *status)
{
	struct bt_adapter *a;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	bluetoothd_lock();
	a = bluetoothd_adapter_pick_locked(node);
	*status = (a == NULL) ? bt_status_no_adapter_e :
	    adapter_scan_locked(a, seconds);
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_scan_count(mach_port_t server, BTNodeName node_in, int *count,
    int *scanning, bt_status_t *status)
{
	struct bt_adapter *a;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	*count = 0;
	*scanning = 0;
	bluetoothd_lock();
	a = bluetoothd_adapter_pick_locked(node);
	if (a == NULL) {
		*status = bt_status_no_adapter_e;
	} else {
		disc_prune(a, time(NULL));
		*count = disc_count(a);
		*scanning = a->scanning ? 1 : 0;
		*status = a->powered ? bt_status_success_e :
		    bt_status_powered_off_e;
	}
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_scan_entry(mach_port_t server, BTNodeName node_in, int index,
    BTAddressString address, BTDeviceName name, int *class_of_device,
    int *rssi, int *transports, int *appearance, int *paired, int *age,
    bt_status_t *status)
{
	const struct bt_found *f;
	struct bt_adapter *a;
	struct bt_addr addr;
	struct bt_bond *b;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	(void)memset(address, 0, sizeof(BTAddressString));
	(void)memset(name, 0, sizeof(BTDeviceName));
	*class_of_device = *transports = *appearance = *paired = *age = 0;
	*rssi = 127;

	bluetoothd_lock();
	a = bluetoothd_adapter_pick_locked(node);
	f = (a == NULL) ? NULL : disc_at(a, index);
	if (a == NULL) {
		*status = bt_status_no_adapter_e;
		bluetoothd_unlock();
		return (KERN_SUCCESS);
	}
	if (f == NULL) {
		*status = bt_status_index_out_of_range_e;
		bluetoothd_unlock();
		return (KERN_SUCCESS);
	}
	addr = f->addr;
	(void)bt_addr_ntoa(&f->addr, BT_ADDR_FMT_DISPLAY, address,
	    sizeof(BTAddressString));
	(void)strlcpy(name, f->name, sizeof(BTDeviceName));
	*class_of_device = (int)f->class_of_device;
	*rssi = f->rssi;
	*transports = f->transports;
	*appearance = f->appearance;
	*age = (int)(time(NULL) - f->last_seen);
	bluetoothd_unlock();

	b = calloc(1, sizeof(*b));
	if (b != NULL) {
		*paired = bt_store_bond_find(&addr, false, b) ? 1 : 0;
		free(b);
	}
	*status = bt_status_success_e;
	return (KERN_SUCCESS);
}

/* ---- 31030-31039: pairing ----------------------------------------------- */

kern_return_t
_bt_pair(mach_port_t server, BTAddressString address_in, BTPinString pin_in,
    bt_status_t *status)
{
	BTAddressString address;
	BTPinString pin;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	WIRE_STR(pin, pin_in);
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	*status = adapter_pair_locked(&a, pin);
	bluetoothd_unlock();
	(void)memset(pin, 0, sizeof(pin));
	return (KERN_SUCCESS);
}

kern_return_t
_bt_pair_status(mach_port_t server, BTAddressString address_in,
    bt_pair_state_t *state, bt_pair_method_t *method, int *passkey,
    int *expires_in, bt_status_t *result, bt_status_t *status)
{
	BTAddressString address;
	struct bt_pair_prompt pr;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	*state = bt_pair_state_none_e;
	*method = bt_pair_none_e;
	*passkey = 0;
	*expires_in = 0;
	*result = bt_status_success_e;
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	(void)pairing_status_locked(&a, state, result, &pr, NULL, 0);
	bluetoothd_unlock();
	if (*state == bt_pair_state_waiting_e) {
		time_t now = time(NULL);

		*method = pr.method;
		*passkey = (int)pr.passkey;
		*expires_in = pr.expires > now ? (int)(pr.expires - now) : 0;
	}
	*status = bt_status_success_e;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_pair_confirm(mach_port_t server, BTAddressString address_in, int accept,
    int passkey, BTPinString pin_in, bt_status_t *status)
{
	BTAddressString address;
	BTPinString pin;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	WIRE_STR(pin, pin_in);
	if (!bt_addr_aton(address, &a) || passkey < 0) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	*status = adapter_pair_confirm_locked(&a, accept != 0,
	    (uint32_t)passkey, pin);
	bluetoothd_unlock();
	(void)memset(pin, 0, sizeof(pin));
	return (KERN_SUCCESS);
}

/* ---- 31040-31049: connections ------------------------------------------- */

kern_return_t
_bt_connect(mach_port_t server, BTAddressString address_in,
    bt_status_t *status)
{
	BTAddressString address;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	*status = conn_connect_locked(&a);
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_bt_disconnect(mach_port_t server, BTAddressString address_in,
    bt_status_t *status)
{
	BTAddressString address;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	*status = conn_disconnect_locked(&a);
	bluetoothd_unlock();
	return (KERN_SUCCESS);
}

/* ---- 31080-31084: audio ------------------------------------------------- */

kern_return_t
_bt_audio_count(mach_port_t server, int *count, bt_status_t *status)
{
	struct audio_status st;
	int n = 0;

	(void)server;
	while (audio_status_at(n, &st))
		n++;
	*count = n;
	*status = bt_status_success_e;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_audio_entry(mach_port_t server, int index, BTAddressString address,
    int *role, int *route, int *active, int *avrcp, int *default_out,
    int *volume, BTNodeName node, BTTransportName codec, bt_status_t *status)
{
	struct audio_status st;

	(void)server;
	(void)memset(address, 0, sizeof(BTAddressString));
	(void)memset(node, 0, sizeof(BTNodeName));
	(void)memset(codec, 0, sizeof(BTTransportName));
	*role = *route = *active = *avrcp = *default_out = 0;
	*volume = -1;
	if (index < 0 || !audio_status_at(index, &st)) {
		*status = bt_status_index_out_of_range_e;
		return (KERN_SUCCESS);
	}
	(void)bt_addr_ntoa(&st.peer, BT_ADDR_FMT_DISPLAY, address,
	    sizeof(BTAddressString));
	*role = (int)st.role;
	*route = (int)st.route;
	*active = st.active ? 1 : 0;
	*avrcp = st.avrcp ? 1 : 0;
	*default_out = st.default_out ? 1 : 0;
	*volume = st.volume;
	(void)strlcpy(node, st.node, sizeof(BTNodeName));
	(void)strlcpy(codec, st.codec, sizeof(BTTransportName));
	*status = bt_status_success_e;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_audio_set(mach_port_t server, BTAddressString address_in, int on,
    bt_status_t *status)
{
	BTAddressString address;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	*status = audio_set_enabled(&a, on != 0);
	return (KERN_SUCCESS);
}

kern_return_t
_bt_audio_volume(mach_port_t server, BTAddressString address_in, int volume,
    bt_status_t *status)
{
	BTAddressString address;
	struct bt_addr a;

	(void)server;
	WIRE_STR(address, address_in);
	if (!bt_addr_aton(address, &a)) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	*status = audio_set_volume(&a, volume);
	return (KERN_SUCCESS);
}

kern_return_t
_bt_audio_policy(mach_port_t server, int autoswitch, int a2dp_sink,
    int *autoswitch_now, int *a2dp_sink_now, bt_status_t *status)
{
	struct bt_prefs p;
	bool sink_changed = false;

	(void)server;
	*status = bt_status_success_e;
	bluetoothd_lock();
	bluetoothd_prefs_locked(&p);
	if (autoswitch >= 0 && p.autoswitch_audio != (autoswitch != 0)) {
		p.autoswitch_audio = (autoswitch != 0);
		if (bt_prefs_set_autoswitch_audio(p.autoswitch_audio) != 0)
			*status = bt_status_store_error_e;
	}
	if (a2dp_sink >= 0 && p.a2dp_sink != (a2dp_sink != 0)) {
		p.a2dp_sink = (a2dp_sink != 0);
		sink_changed = true;
		if (bt_prefs_set_a2dp_sink(p.a2dp_sink) != 0)
			*status = bt_status_store_error_e;
	}
	bluetoothd_prefs_update_locked(&p);
	bluetoothd_unlock();
	*autoswitch_now = p.autoswitch_audio ? 1 : 0;
	*a2dp_sink_now = p.a2dp_sink ? 1 : 0;
	if (sink_changed)
		audio_prefs_changed();
	bluetoothd_wake();
	return (KERN_SUCCESS);
}

/* ---- 31050-31069: diagnostics ------------------------------------------- */
/*
 * bt_sdp_browse and bt_sdp_browse_line (31050, 31051) are in sdp_client.c.
 * These block on the controller or the peer (diag.h says for how long) with
 * the state lock not held.
 */

kern_return_t
_bt_adapter_details(mach_port_t server, BTNodeName node_in,
    BTNodeName node_out, BTAddressString address, int *hci_version,
    int *hci_revision, int *lmp_version, int *lmp_subversion,
    int *manufacturer, BTFeatures features, int *acl_mtu, int *acl_pkts,
    int *sco_mtu, int *sco_pkts, int *le_mtu, int *le_pkts,
    bt_status_t *status)
{
	struct diag_details d;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	(void)memset(node_out, 0, sizeof(BTNodeName));
	(void)memset(address, 0, sizeof(BTAddressString));
	(void)memset(features, 0, sizeof(BTFeatures));

	*status = diag_details(node, &d);
	(void)strlcpy(node_out, d.node, sizeof(BTNodeName));
	if (d.have_addr)
		(void)bt_addr_ntoa(&d.addr, BT_ADDR_FMT_DISPLAY, address,
		    sizeof(BTAddressString));
	*hci_version = d.hci_version;
	*hci_revision = d.hci_revision;
	*lmp_version = d.lmp_version;
	*lmp_subversion = d.lmp_subversion;
	*manufacturer = d.manufacturer;
	(void)memcpy(features, d.features, sizeof(BTFeatures));
	*acl_mtu = d.acl_mtu;
	*acl_pkts = d.acl_pkts;
	*sco_mtu = d.sco_mtu;
	*sco_pkts = d.sco_pkts;
	*le_mtu = d.le_mtu;
	*le_pkts = d.le_pkts;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_adapter_stats(mach_port_t server, BTNodeName node_in, int *cmd_sent,
    int *events, int *acl_recv, int *acl_sent, int *sco_recv, int *sco_sent,
    int *bytes_recv, int *bytes_sent, int *cmd_free, int *acl_free,
    int *sco_free, bt_status_t *status)
{
	struct diag_stats s;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	*status = diag_stats(node, &s);
	/* Counters cross as int and are printed back as unsigned. */
	*cmd_sent = (int)s.cmd_sent;
	*events = (int)s.events;
	*acl_recv = (int)s.acl_recv;
	*acl_sent = (int)s.acl_sent;
	*sco_recv = (int)s.sco_recv;
	*sco_sent = (int)s.sco_sent;
	*bytes_recv = (int)s.bytes_recv;
	*bytes_sent = (int)s.bytes_sent;
	*cmd_free = s.cmd_free;
	*acl_free = s.acl_free;
	*sco_free = s.sco_free;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_link_count(mach_port_t server, BTNodeName node_in, int *count,
    bt_status_t *status)
{
	struct diag_link *l;
	BTNodeName node;

	(void)server;
	WIRE_STR(node, node_in);
	*status = diag_links(node, &l, count);
	free(l);
	return (KERN_SUCCESS);
}

kern_return_t
_bt_link_entry(mach_port_t server, BTNodeName node_in, int index,
    BTAddressString address, int *link_type, int *handle, int *role,
    int *encryption, int *mode, int *pending, bt_status_t *status)
{
	struct diag_link *l;
	BTNodeName node;
	int n;

	(void)server;
	WIRE_STR(node, node_in);
	(void)memset(address, 0, sizeof(BTAddressString));
	*link_type = *handle = *role = *encryption = *mode = *pending = 0;

	*status = diag_links(node, &l, &n);
	if (*status == bt_status_success_e) {
		if (index < 0 || index >= n) {
			*status = bt_status_index_out_of_range_e;
		} else {
			(void)bt_addr_ntoa(&l[index].addr, BT_ADDR_FMT_DISPLAY,
			    address, sizeof(BTAddressString));
			*link_type = l[index].link_type;
			*handle = l[index].handle;
			*role = l[index].role;
			*encryption = l[index].encryption;
			*mode = l[index].mode;
			*pending = l[index].pending;
		}
	}
	free(l);
	return (KERN_SUCCESS);
}

/*
 * A raw HCI command can do anything to the controller, including reading or
 * writing link keys, so it is root's alone: the euid from the sender's audit
 * token (val[1] in the Darwin layout, as audit_token_to_au32 reads it), which
 * the receive loop asks the kernel to append (mach_service.c).
 */
kern_return_t
_bt_hci_command(mach_port_t server, BTNodeName node_in, int opcode,
    BTHCIData params, int plen, int *event, int *hci_status,
    BTHCIData rparams, int *rplen, int *errnum, bt_status_t *status,
    audit_token_t audit)
{
	BTNodeName node;
	uint8_t hst = 0;
	size_t n = 0;

	(void)server;
	WIRE_STR(node, node_in);
	(void)memset(rparams, 0, sizeof(BTHCIData));
	*event = *hci_status = *rplen = *errnum = 0;

	if (audit.val[1] != 0) {
		*errnum = EPERM;
		*status = bt_status_operation_not_supported_e;
		return (KERN_SUCCESS);
	}
	if (opcode <= 0 || opcode > 0xffff || plen < 0 ||
	    plen > BT_HCI_DATA_MAX) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	*status = diag_hci_command(node, (uint16_t)opcode,
	    (const uint8_t *)params, (size_t)plen, event, &hst,
	    (uint8_t *)rparams, &n, errnum);
	*hci_status = hst;
	*rplen = (int)n;
	return (KERN_SUCCESS);
}

kern_return_t
_bt_l2cap_ping(mach_port_t server, BTAddressString address_in, int size,
    int *rtt_us, int *result, int *errnum, bt_status_t *status)
{
	BTAddressString address;
	struct bt_addr a;
	uint32_t rtt = 0, res = 0;

	(void)server;
	WIRE_STR(address, address_in);
	*rtt_us = *result = *errnum = 0;
	if (!bt_addr_aton(address, &a) || size < 0) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	*status = diag_l2cap_ping(&a, (size_t)size, &rtt, &res, errnum);
	*rtt_us = (int)rtt;
	*result = (int)res;
	return (KERN_SUCCESS);
}
