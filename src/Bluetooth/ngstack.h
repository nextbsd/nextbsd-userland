/*
 * ngstack.h — the kernel Bluetooth stack above the driver: loading its kexts
 * and building each controller's netgraph graph.
 *
 * NgUbt.kext loads by IOKit matching and pulls in Netgraph, NgBluetooth and
 * NgHCI as its dependencies. Nothing loads the rest: NgSocket (the netgraph
 * control socket this file talks through), NgL2cap, and NgBtsocket (the
 * PF_BLUETOOTH sockets and their btsock_* nodes). On FreeBSD devd and
 * rc.d/bluetooth did; on NextBSD bluetoothd loads them with
 * KextManagerLoadKextWithIdentifier when the first controller arrives, so a
 * machine with no Bluetooth never loads them (E16 plan, decision Q1).
 *
 * The graph is rc.d/bluetooth's bluetooth_setup_stack(), in C:
 *
 *   ubt0:hook ── drv:ubt0hci ─┬─ acl ── hci:ubt0l2cap ─┬─ ctl ── btsock_l2c_raw:
 *                             │                        └─ l2c ── btsock_l2c:
 *                             ├─ raw ── btsock_hci_raw:
 *                             └─ sco ── btsock_sco:
 *
 * The sco hook is NextBSD's addition: upstream never wires it, so SCO sockets
 * (HFP audio) had nowhere to go.
 */
#ifndef _BLUETOOTHD_NGSTACK_H_
#define _BLUETOOTHD_NGSTACK_H_

#include <stdbool.h>

/* The upper-stack kexts, by bundle id (nextbsd-kernel-extensions kext.env). */
#define	NGSTACK_KEXT_SOCKET	"org.nextbsd.net.NgSocket"
#define	NGSTACK_KEXT_L2CAP	"org.nextbsd.net.NgL2cap"
#define	NGSTACK_KEXT_BTSOCKET	"org.nextbsd.net.NgBtsocket"

/*
 * Load the three kexts, in dependency order. Idempotent: KextManager reports
 * success for a kext that is already loaded. Returns 0 when the stack is
 * usable afterwards (also when the kexts are compiled into the kernel and
 * KextManager has nothing to load), -1 when it is not.
 */
int	ngstack_load(void);

/*
 * Build the graph for driver node `node` ("ubt0"). If "<node>hci" already
 * exists (bluetoothd restarted, the controller did not), the graph is adopted
 * as it is and *adopted is set. Returns 0 on success, -1 on failure (logged).
 */
int	ngstack_build(const char *node, bool *adopted);

/*
 * Shut down "<node>hci" and "<node>l2cap", as bluetooth_shutdown_stack()
 * does. The driver node goes with the device; these two would otherwise be
 * left behind holding their socket hooks.
 */
void	ngstack_teardown(const char *node);

#endif /* _BLUETOOTHD_NGSTACK_H_ */
