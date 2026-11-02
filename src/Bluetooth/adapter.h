/*
 * adapter.h — the controller lifecycle (#207): from an
 * IOBluetoothHostController arrival to a powered controller with its engines
 * attached, and back.
 *
 *   arrival      load NgSocket/NgL2cap/NgBtsocket (first controller only),
 *                build or adopt the netgraph graph (ngstack.c), open the raw
 *                HCI socket (hci.c)
 *   init         Reset, Read_Local_Version, Read_BD_ADDR,
 *                Read_Local_Supported_Features, Read_Buffer_Size, then for an
 *                LE controller LE_Read_Buffer_Size, LE_Set_Event_Mask and
 *                Write_LE_Host_Supported; Set_Event_Mask, Write_Simple_
 *                Pairing_Mode, Write_Inquiry_Mode, Write_Class_Of_Device,
 *                Change_Local_Name, Write_Scan_Enable; then NGM_HCI_NODE_INIT.
 *                One command at a time, each answered by its Command Complete
 *                on the main loop: nothing blocks.
 *   powered      the engines attach; on means page scan (and inquiry scan
 *                when discoverable), off means no scans, no links and no
 *                scans or pairing for clients
 *   departure    engines detach, the socket closes, the graph is shut down
 *                and State:/Bluetooth/Adapter/<node> is removed
 *
 * Bluetooth is on by default: a controller comes up powered unless the
 * persisted ControllerPowerState says 0.
 *
 * Locking: adapter_bringup() and adapter_teardown() run on the main loop with
 * the state lock NOT held (they block on kextd and netgraph); everything named
 * *_locked expects bluetoothd_lock() held, and may be called from the main
 * loop or the Mach service thread.
 */
#ifndef _BLUETOOTHD_ADAPTER_H_
#define _BLUETOOTHD_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "bluetoothd.h"

/* Bring a freshly recorded controller up. Main loop, lock not held. */
void		adapter_bringup(struct bt_adapter *a);

/* Take a controller down before its slot is freed. Main loop, lock held. */
void		adapter_teardown_locked(struct bt_adapter *a);

/* Drain the controller's HCI socket and dispatch every event. */
void		adapter_input_locked(struct bt_adapter *a);

/* The earliest timer over every controller and engine; 0 for none. */
uint64_t	adapter_next_timer_locked(void);

/* Run the timers that are due. */
void		adapter_timers_locked(uint64_t now);

/* Publish what changed to the SCDynamicStore. Main loop, lock not held. */
void		adapter_publish(void);

/* The RPCs. */
bt_status_t	adapter_set_power_locked(struct bt_adapter *a, bool on);
bt_status_t	adapter_set_discoverable_locked(struct bt_adapter *a, bool on,
		    int seconds);
bt_status_t	adapter_scan_locked(struct bt_adapter *a, int seconds);
bt_status_t	adapter_pair_locked(const struct bt_addr *peer,
		    const char *pin);
bt_status_t	adapter_pair_confirm_locked(const struct bt_addr *peer,
		    bool accept, uint32_t passkey, const char *pin);
bt_status_t	adapter_unpair_locked(const struct bt_addr *peer);

/* The Classic engine of `a` and its ops, or NULL. Lock held. */
struct bt_engine	*adapter_classic_locked(struct bt_adapter *a,
			    const struct bt_engine_ops **ops);

/* The engine of `a` for `transport` (BT_TRANSPORT_*), or NULL. Lock held. */
struct bt_engine	*adapter_engine_locked(struct bt_adapter *a,
			    int transport, const struct bt_engine_ops **ops);

#endif /* _BLUETOOTHD_ADAPTER_H_ */
