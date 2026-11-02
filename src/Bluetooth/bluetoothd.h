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

/* The discovery cache (discovery.c): entries and how long they live. */
#define	BT_SCAN_MAX		64
#define	BT_SCAN_TTL		300	/* seconds since last seen */

struct bt_name_cache {
	struct bt_addr		addr;
	char			name[BT_NAME_MAX + 1];
};

struct bt_adapter {
	bool			active;
	uint64_t		entry_id;	/* IORegistry entry ID */
	BTNodeName		node;		/* "ubt0" */
	BTTransportName		transport;	/* "USB" */
	uint16_t		vendor;		/* USB idVendor */
	uint16_t		product;	/* USB idProduct */
	char			ugen[32];	/* "ugen0.3" */
	bt_adapter_state_t	state;
	bool			want_power;	/* ControllerPowerState at arrival */

	/* Bring-up (adapter.c). */
	char			hci_node[32];	/* "ubt0hci" */
	int			hci_fd;		/* raw HCI socket, or -1 */
	bool			adopted;	/* the graph outlived a restart */
	int			init_step;
	uint16_t		init_opcode;	/* the command awaited */
	uint64_t		init_deadline;
	int			init_tries;
	uint8_t			features[8];	/* LMP features, page 0 */
	uint8_t			hci_version;
	uint16_t		manufacturer;

	/* Read from the controller at bring-up (#207). */
	bool			have_addr;
	struct bt_addr		addr;
	BTDeviceName		name;

	/* Policy. */
	bool			powered;	/* page scan on, engines live */
	bool			discoverable;	/* inquiry scan on */
	uint64_t		discoverable_until;	/* 0: no timer */
	bool			scanning;

	/* The discovery cache (discovery.c). */
	struct bt_found		found[BT_SCAN_MAX];
	int			nfound;
	struct bt_name_cache	names[BT_SCAN_MAX];
	int			nnames_next;	/* round-robin slot */

	/* Publication: set when something State:/Bluetooth shows changed. */
	bool			dirty;
	bool			scan_dirty;

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

/*
 * The controller an RPC means: by node name, or with an empty name the first
 * one. NULL when there is none. Caller holds the lock.
 */
struct bt_adapter	*bluetoothd_adapter_pick_locked(const char *node);

/* The persisted DiscoverableState. Caller holds the lock. */
bool	bluetoothd_discoverable_pref_locked(void);

/* Update the cached policy after an RPC changed it. Caller holds the lock. */
void	bluetoothd_prefs_update_locked(const struct bt_prefs *p);

/*
 * Wake the main loop, so it publishes what an RPC changed and re-computes its
 * timers. Safe from any thread.
 */
void	bluetoothd_wake(void);

#endif /* _BLUETOOTHD_H_ */
