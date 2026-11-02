/*
 * hci.h — bluetoothd's raw HCI socket: one per controller, shared by the
 * controller bring-up (adapter.c) and both engines.
 *
 * The socket is a PF_BLUETOOTH/BLUETOOTH_PROTO_HCI raw socket bound and
 * connected to the controller's HCI node ("ubt0hci"), so it sees only that
 * controller. Its filter passes every HCI event and nothing else (no ACL or
 * SCO data); commands are sent with send() and ng_hci queues them against the
 * controller's command credits. The socket is non-blocking: the main loop
 * polls it and drains it with hci_recv_event().
 *
 * Nothing here waits for a reply. Command Complete and Command Status arrive as
 * ordinary events, and whoever sent the command matches them by opcode.
 */
#ifndef _BLUETOOTHD_HCI_H_
#define _BLUETOOTHD_HCI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine.h"

#define	HCI_OPCODE(ogf, ocf)	((uint16_t)((((ogf) & 0x3f) << 10) | \
				    ((ocf) & 0x3ff)))

/* One event read from the socket. params points into the caller's buffer. */
struct hci_event {
	uint8_t		code;
	uint8_t		len;
	const uint8_t	*params;
};

/*
 * Open the raw socket for HCI node `hci_node` ("ubt0hci"). Returns the fd, or
 * -1 with errno set (EPROTONOSUPPORT while NgBtsocket is not loaded).
 */
int	hci_open(const char *hci_node);

/* Send one command. 0 on success, -1 with errno set. */
int	hci_send_cmd(int fd, uint16_t opcode, const void *params, size_t len);

/*
 * Read one event. Returns 1 with *ev filled, 0 when nothing is waiting, -1 on
 * a socket error. `buf` must outlive the use of ev->params.
 */
int	hci_recv_event(int fd, uint8_t *buf, size_t buflen,
	    struct hci_event *ev);

/*
 * Pick apart a Command Complete (0x0e) or Command Status (0x0f) event: the
 * opcode it answers, its status (the first return parameter of a Command
 * Complete, the status field of a Command Status) and, for Command Complete,
 * the return parameters after the status. Returns false for any other event
 * or a short one.
 */
bool	hci_cmd_result(const struct hci_event *ev, uint16_t *opcode,
	    uint8_t *status, const uint8_t **rp, size_t *rplen);

/* Ask ng_hci to mark the node initialised (NGM_HCI_NODE_INIT). */
int	hci_node_init(int fd);

/* The node's state bits (NG_HCI_UNIT_*), or -1. */
int	hci_node_state(int fd);

/*
 * The ACL connection handle to `peer`, from ng_hci's connection list; -1 when
 * there is none.
 */
int	hci_acl_handle(int fd, const struct bt_addr *peer);

/*
 * Every open ACL handle, up to `max`. Returns how many were written; used to
 * disconnect everything when the controller is powered off.
 */
int	hci_acl_handles(int fd, uint16_t *handles, int max);

/* bdaddr_t (wire order) <-> struct bt_addr (always public for BR/EDR). */
void	hci_addr_from_wire(const uint8_t wire[BT_ADDR_LEN], struct bt_addr *a);
void	hci_addr_to_wire(const struct bt_addr *a, uint8_t wire[BT_ADDR_LEN]);

/* Monotonic milliseconds, for every timer in bluetoothd. */
uint64_t	bt_now_ms(void);

#endif /* _BLUETOOTHD_HCI_H_ */
