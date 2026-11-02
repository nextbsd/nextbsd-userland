/*
 * links.h — the Classic engine's ACL links (#213): what hccontrol
 * create_connection / disconnect did by hand, kept as a table.
 *
 *   connect      Create_Connection, unless a link is up or on its way
 *   disconnect   Disconnect (reason: remote user terminated)
 *   follow       Connection_Request (the peer paged us), Connection_Complete,
 *                Disconnection_Complete, Encryption_Change, Role_Change,
 *                and the Command Status of our Create_Connection
 *   secure       every new link to a bonded device gets
 *                Authentication_Requested, then Set_Connection_Encryption,
 *                unless the pairing in progress (security.c) owns it or the
 *                peer already encrypted it
 *   RSSI         Read_RSSI on every link every LINKS_RSSI_MS; a controller
 *                that refuses the command is not asked again
 *
 * Every change is raised to the policy layer as BT_EV_CONNECTED,
 * BT_EV_DISCONNECTED or BT_EV_LINK_CHANGED (engine.h, struct bt_link). The
 * policy (who may connect, what reconnects at power-up) is not here; it is
 * connections.c's.
 *
 * Accepting a Connection_Request is the kernel's job: ng_l2cap answers it
 * before the raw socket even sees the event. A link the policy does not want
 * is disconnected once it is up.
 */
#ifndef _BLUETOOTHD_LINKS_H_
#define _BLUETOOTHD_LINKS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "classic.h"

#define	LINKS_RSSI_MS		10000

void		links_init(struct bt_engine *e);

/* One HCI event. Returns true if it was only the links' business. */
bool		links_hci_event(struct bt_engine *e, uint8_t code,
		    const uint8_t *p, size_t len);

/* Command Status / Complete for a command the links sent. */
void		links_cmd_result(struct bt_engine *e, uint16_t opcode,
		    uint8_t status, const uint8_t *rp, size_t rplen);

bt_status_t	links_connect(struct bt_engine *e, const struct bt_addr *peer);
bt_status_t	links_disconnect(struct bt_engine *e,
		    const struct bt_addr *peer);

uint64_t	links_next_timer(struct bt_engine *e);
void		links_timer(struct bt_engine *e, uint64_t now);

#endif /* _BLUETOOTHD_LINKS_H_ */
