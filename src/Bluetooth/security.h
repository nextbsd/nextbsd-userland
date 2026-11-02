/*
 * security.h — the Classic engine's security manager: what hcsecd did on
 * FreeBSD, plus Secure Simple Pairing, a real store and the user.
 *
 * Responder (always on):
 *   Link_Key_Request         reply from the store, else a negative reply
 *   Link_Key_Notification    saved at once (bonded devices, or the pairing
 *                            in progress); never for a stranger
 *   PIN_Code_Request         the PIN given to bt_pair, else ask the user;
 *                            a request nobody asked for is refused
 *   IO_Capability_Request    DisplayYesNo, no OOB, MITM with dedicated
 *                            bonding, for a pairing we started; refused with
 *                            "pairing not allowed" otherwise
 *   User_Confirmation_Req    accepted at once when the peer has no input or
 *                            output (headphones) and we started the pairing;
 *                            otherwise the user compares the number
 *   User_Passkey_Request     the user types the peer's passkey
 *   User_Passkey_Notif       the user types our passkey on the peer
 *                            (keyboards)
 *
 * Initiator (bt_pair): Create_Connection if there is no ACL link yet, then
 * Authentication_Requested; the events above do the rest. On success the
 * link is encrypted (Set_Connection_Encryption). A prompt the user does not
 * answer within 60 s gets the negative reply and fails the pairing with
 * bt_status_pairing_timeout_e.
 *
 * Prompts follow the pull model of engine.h: the security manager raises
 * BT_EV_PAIR_PROMPT, bluetoothd parks it, and bt_pair_confirm comes back as
 * pair_reply. LE pairing (SMP, #205) uses the same prompts and store.
 *
 * Key bytes are never logged.
 */
#ifndef _BLUETOOTHD_SECURITY_H_
#define _BLUETOOTHD_SECURITY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "classic.h"

#define	SEC_USER_TIMEOUT_MS	60000	/* a prompt waits this long */
#define	SEC_PAIR_TIMEOUT_MS	90000	/* the whole pairing */

void		sec_init(struct bt_engine *e);

/*
 * One HCI event. Returns true if the security manager consumed it (the
 * discovery half then ignores it).
 */
bool		sec_hci_event(struct bt_engine *e, uint8_t code,
		    const uint8_t *p, size_t len);

/* Command Status / Complete for a command the security manager sent. */
void		sec_cmd_result(struct bt_engine *e, uint16_t opcode,
		    uint8_t status);

bt_status_t	sec_pair(struct bt_engine *e, const struct bt_addr *peer,
		    const char *pin);
bt_status_t	sec_pair_reply(struct bt_engine *e,
		    const struct bt_pair_answer *a);
bt_status_t	sec_unpair(struct bt_engine *e, const struct bt_addr *peer);

/* Fail the pairing in progress, if any (power off, detach). */
void		sec_abort(struct bt_engine *e, bt_status_t why);

uint64_t	sec_next_timer(struct bt_engine *e);
void		sec_timer(struct bt_engine *e, uint64_t now);

#endif /* _BLUETOOTHD_SECURITY_H_ */
