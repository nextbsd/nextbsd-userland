/*
 * pairing.h — the policy layer's side of pairing (#209): which pairings are
 * running, the prompts parked for the user, and how each one ended.
 *
 * Nothing above the Mach line can receive a Mach message, so a prompt is
 * pulled, never pushed:
 *
 *   State:/Bluetooth/Pairing/<addr> = { Address, Method, Passkey, Expires }
 *   notify_post("org.nextbsd.bluetooth.pairing")
 *
 * tells a client a prompt is waiting; bt_pair_status fetches it and
 * bt_pair_confirm answers it. The key goes away when the prompt is answered
 * or the pairing ends. A finished pairing is still reported by bt_pair_status
 * for PAIRING_KEEP_S seconds, so `bluetooth pair` sees how it ended.
 *
 * Engine-agnostic: the Classic security manager raises the prompts today and
 * LE SMP (#205) will raise the same ones.
 *
 * Every *_locked call expects bluetoothd_lock() held.
 */
#ifndef _BLUETOOTHD_PAIRING_H_
#define _BLUETOOTHD_PAIRING_H_

#include <stdbool.h>
#include <stdint.h>

#include "bluetooth_mig_types.h"
#include "engine.h"

#define	PAIRING_KEEP_S		120

/* A pairing was started with `peer` on controller `node`. */
void	pairing_begin_locked(const char *node, const struct bt_addr *peer);

/* Is a pairing with `peer` running? */
bool	pairing_active_locked(const struct bt_addr *peer);

/* The pairing created the bond (so a failure should take it away again). */
void	pairing_mark_created_locked(const struct bt_addr *peer);

/* Park a prompt (replaces any earlier one for the same peer). */
void	pairing_prompt_locked(const struct bt_pair_prompt *p);

/* The prompt for `peer` was answered. */
void	pairing_answered_locked(const struct bt_addr *peer);

/*
 * The pairing ended. Returns true if it had created the bond, which the caller
 * then forgets on failure.
 */
bool	pairing_finish_locked(const struct bt_addr *peer, bt_status_t st);

/* Every pairing on `node` fails: the controller went away. */
void	pairing_drop_node_locked(const char *node);

/*
 * What bt_pair_status reports. Returns false when nothing is known about
 * `peer` (state none). `prompt` is filled while state is waiting.
 */
bool	pairing_status_locked(const struct bt_addr *peer,
	    bt_pair_state_t *state, bt_status_t *result,
	    struct bt_pair_prompt *prompt, char *node, size_t nodelen);

/* A device was paired (true) or forgotten (false): publish or remove it. */
void	pairing_device_changed_locked(const struct bt_addr *peer, bool paired);

/* Publish what changed. Main loop, lock NOT held. */
void	pairing_publish(void);

#endif /* _BLUETOOTHD_PAIRING_H_ */
