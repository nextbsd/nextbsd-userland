/*
 * mach_service.c — bluetoothd's Mach service thread.
 *
 * Owns the receive right for org.nextbsd.bluetooth. Runs a raw
 * mach_msg(MACH_RCV_MSG | MACH_RCV_TIMEOUT, 500ms) loop and hands each message
 * to bluetooth_server() (the MIG demux for bluetooth.defs). Structurally
 * identical to wland's mach_service.c, itself ipconfigd's: the same 500ms
 * timeout keeps the loop awake to notice shutdown. One difference: it asks for
 * the sender's audit token in the trailer, as configd does, because
 * bt_hci_command is root-only and reads it (ServerAuditToken).
 *
 * WHY org.nextbsd.* AND NOT com.apple.*:
 *
 * Apple's daemon is also called bluetoothd, but its Mach services
 * (com.apple.server.bluetooth and friends) are contracts that only Apple's
 * closed frameworks look up, and nothing Apple-derived on NextBSD does. We
 * write the bluetooth(8) CLI, and the IOBluetooth-shaped Objective-C API sits
 * above the Mach line in Gershwin. Both ends are ours, so the label is ours,
 * as for wland.
 */
#include "mach_service.h"
#include "bluetoothd.h"		/* bluetoothd_got_term */
#include "bluetoothServer.h"		/* MIG: bluetooth_server() */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * 8 KiB. bluetooth.defs carries no OOL data (MIG OOL is broken in this
 * kernel); the largest reply, bt_paired_entry, is a name, an address, a
 * profile list and a few scalars, about 500 bytes. Room for the surface to
 * grow.
 */
#define	BT_RPC_BUFSZ		8192

#define	BT_SERVICE_NAME		"org.nextbsd.bluetooth"

/* Append the audit token; MIG rejects a ServerAuditToken routine without it. */
#define	BT_RCV_TRAILER						\
	(MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |		\
	 MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT))

static pthread_t	worker;
static int		worker_running;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[mach] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void *
mach_service_thread(void *arg)
{
	mach_port_t sp = MACH_PORT_NULL;
	kern_return_t kr;

	(void)arg;

	kr = bootstrap_check_in(bootstrap_port, BT_SERVICE_NAME, &sp);
	if (kr != KERN_SUCCESS) {
		xlog("bootstrap_check_in('%s') failed: 0x%x — RPC off",
		    BT_SERVICE_NAME, (unsigned)kr);
		return (NULL);
	}
	xlog("Mach service '%s' checked in (port=0x%x)",
	    BT_SERVICE_NAME, (unsigned)sp);

	while (!bluetoothd_got_term) {
		union {
			mach_msg_header_t	hdr;
			char			buf[BT_RPC_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		(void)memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
		    BT_RCV_TRAILER, 0, sizeof(req), sp, 500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			xlog("receive failed: 0x%x — service off", (unsigned)mr);
			break;
		}

		(void)memset(&rep, 0, sizeof(rep));
		if (!bluetooth_server(&req.hdr, &rep.hdr)) {
			xlog("ignoring message id=%d", req.hdr.msgh_id);
			continue;
		}
		if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
			mr = mach_msg(&rep.hdr,
			    MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 200,
			    MACH_PORT_NULL);
			if (mr != MACH_MSG_SUCCESS)
				xlog("reply send failed: 0x%x", (unsigned)mr);
		}
	}
	return (NULL);
}

int
mach_service_start(void)
{
	int rc;

	if (worker_running)
		return (0);
	rc = pthread_create(&worker, NULL, mach_service_thread, NULL);
	if (rc != 0) {
		xlog("pthread_create failed: %s", strerror(rc));
		return (-1);
	}
	worker_running = 1;
	return (0);
}

void
mach_service_join(void)
{
	if (!worker_running)
		return;
	(void)pthread_join(worker, NULL);
	worker_running = 0;
}
