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
 *
 *   State:/Bluetooth/Adapter/<node>   one controller (reserved: #207)
 *   State:/Bluetooth/Device/<addr>    one paired or connected device
 *                                     (reserved: #209, #213); <addr> is
 *                                     BT_ADDR_FMT_KEY
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

#include "bluetooth_mig_types.h"
#include "btstore.h"

#define	BT_STATE_KEY		"State:/Bluetooth"
#define	BT_STATE_ADAPTER_PREFIX	BT_STATE_KEY "/Adapter/"	/* + node */
#define	BT_STATE_DEVICE_PREFIX	BT_STATE_KEY "/Device/"	/* + addr key */

struct bt_daemon_summary {
	bt_daemon_state_t	state;
	int			nadapters;
	const char * const	*adapters;	/* nadapters node names */
	int			npaired;
	struct bt_prefs		prefs;
};

/* Publish State:/Bluetooth. 0 on success, -1 when configd is unreachable. */
int	bt_state_publish_daemon(const struct bt_daemon_summary *s);

/* Remove one key (a full key, e.g. BT_STATE_KEY). */
int	bt_state_remove(const char *key);

/* Release the store session. */
void	bt_state_close(void);

#endif /* _BLUETOOTHD_BTSTATE_H_ */
