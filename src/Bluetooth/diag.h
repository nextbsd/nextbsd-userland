/*
 * diag.h — the diagnostics behind `bluetooth info`, `bluetooth hci` and
 * `bluetooth ping` (#211): what hccontrol and l2ping gave, run inside
 * bluetoothd so the CLI stays a plain MIG client.
 *
 * Every call here runs on the Mach service thread and may block, for at most
 * DIAG_HCI_TIMEOUT_MS on a controller or the kernel's L2CAP RTX timeout on a
 * ping. None of them holds bluetoothd_lock() while it waits: each copies what
 * it needs from the controller table under the lock, then talks to the kernel
 * on a socket of its own, never on the controller's own HCI socket, so the
 * main loop and the engines are not disturbed. Events the controller sends in
 * answer reach both sockets; the main loop ignores answers to commands it did
 * not send.
 */
#ifndef _BLUETOOTHD_DIAG_H_
#define _BLUETOOTHD_DIAG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bluetooth_mig_types.h"
#include "engine.h"

#define	DIAG_HCI_TIMEOUT_MS	5000

struct diag_details {
	BTNodeName	node;
	struct bt_addr	addr;
	bool		have_addr;
	int		hci_version, hci_revision;
	int		lmp_version, lmp_subversion;
	int		manufacturer;
	uint8_t		features[8];
	int		acl_mtu, acl_pkts;
	int		sco_mtu, sco_pkts;
	int		le_mtu, le_pkts;
};

struct diag_stats {
	uint32_t	cmd_sent, events;
	uint32_t	acl_recv, acl_sent;
	uint32_t	sco_recv, sco_sent;
	uint32_t	bytes_recv, bytes_sent;
	int		cmd_free, acl_free, sco_free;
};

struct diag_link {
	struct bt_addr	addr;
	int		link_type;	/* NG_HCI_LINK_* */
	int		handle;
	int		role;
	int		encryption;
	int		mode;
	int		pending;
};

/* `node` "" means the first controller, as everywhere else. */
bt_status_t	diag_details(const char *node, struct diag_details *out);
bt_status_t	diag_stats(const char *node, struct diag_stats *out);

/* The links open now. *out is malloc()ed; the caller frees it. */
bt_status_t	diag_links(const char *node, struct diag_link **out, int *n);

/*
 * Send one raw HCI command and wait for its Command Complete or Command
 * Status. *event is the event code that answered; for Command Complete, `rp`
 * gets the return parameters after the status (at most BT_HCI_DATA_MAX).
 * *errnum is set on failure.
 */
bt_status_t	diag_hci_command(const char *node, uint16_t opcode,
		    const uint8_t *cp, size_t cplen, int *event,
		    uint8_t *hci_status, uint8_t *rp, size_t *rplen,
		    int *errnum);

/* One L2CAP echo to a BR/EDR peer. */
bt_status_t	diag_l2cap_ping(const struct bt_addr *peer, size_t size,
		    uint32_t *rtt_us, uint32_t *result, int *errnum);

#endif /* _BLUETOOTHD_DIAG_H_ */
