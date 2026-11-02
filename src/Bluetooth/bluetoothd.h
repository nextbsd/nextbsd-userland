/*
 * bluetoothd.h — bluetoothd's shared state: the controller table and the
 * cached policy.
 *
 * One entry per Bluetooth controller (an IOBluetoothHostController in the
 * IORegistry): its netgraph node, its USB ids, where its bring-up has got to,
 * and the engines attached to it.
 *
 * Two threads touch this: the main event loop (IOKit arrivals and departures,
 * engine events, SCDynamicStore publication) and the Mach service thread
 * (bluetooth.defs RPCs from bluetooth(8)). One mutex guards it, as in wland:
 * the rate is a handful of events per minute.
 */
#ifndef _BLUETOOTHD_H_
#define _BLUETOOTHD_H_

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "bluetooth_mig_types.h"
#include "btstore.h"
#include "engine.h"

#define	BT_MAX_ADAPTERS		8

struct bt_adapter {
	bool			active;
	uint64_t		entry_id;	/* IORegistry entry ID */
	BTNodeName		node;		/* "ubt0" */
	BTTransportName		transport;	/* "USB" */
	uint16_t		vendor;		/* USB idVendor */
	uint16_t		product;	/* USB idProduct */
	bt_adapter_state_t	state;
	bool			want_power;	/* ControllerPowerState at arrival */

	/* Read from the controller at bring-up (#207). */
	bool			have_addr;
	struct bt_addr		addr;
	BTDeviceName		name;

	/* engines[i] runs bt_engines[i] on this controller, or is NULL. */
	struct bt_engine	*engines[BT_ENGINE_MAX];
};

/* Daemon shutdown flag, set by the signal handler. */
extern volatile sig_atomic_t	bluetoothd_got_term;

/*
 * Lock / unlock the shared state. The MIG routine bodies in bluetoothd_mig.c
 * call these: they run on the Mach service thread and read what the main loop
 * writes.
 */
void	bluetoothd_lock(void);
void	bluetoothd_unlock(void);

/* Daemon state for bt_status and State:/Bluetooth. Caller holds the lock. */
bt_daemon_state_t	bluetoothd_state_locked(void);

/* The policy as last loaded or changed. Caller holds the lock. */
void	bluetoothd_prefs_locked(struct bt_prefs *out);

/* Number of controllers. Caller holds the lock. */
int	bluetoothd_adapter_count_locked(void);

/* The idx-th controller, or NULL. Caller holds the lock. */
struct bt_adapter	*bluetoothd_adapter_at_locked(int idx);

/* A controller by node name ("ubt0"), or NULL. Caller holds the lock. */
struct bt_adapter	*bluetoothd_adapter_by_node_locked(const char *node);

#endif /* _BLUETOOTHD_H_ */
