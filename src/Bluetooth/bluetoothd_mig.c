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
#include "btstore.h"
#include "bluetooth_mig_types.h"
#include "engine.h"

#include <mach/mach.h>
#include "bluetoothServer.h"		/* MIG-emitted prototypes */

#include <string.h>

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
	/* The connection table arrives with #213; until then, disconnected. */
	*state = bt_device_disconnected_e;

	if (!bt_store_paired_at(index, &b)) {
		*status = bt_status_index_out_of_range_e;
		return (KERN_SUCCESS);
	}
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
