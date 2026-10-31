/*
 * mach_service.c — wland's Mach service thread.
 *
 * Owns the receive right for org.nextbsd.wlan. Runs a raw mach_msg(MACH_RCV_MSG
 * | MACH_RCV_TIMEOUT, 500ms) loop and hands each message to wlan_server() (the
 * MIG demux for wlan.defs). Structurally identical to ipconfigd's
 * mach_service.c — same 500ms timeout so the loop stays awake to notice
 * shutdown.
 *
 * WHY org.nextbsd.* AND NOT com.apple.*:
 *
 * Every NextBSD launchd job is labelled org.nextbsd.* (nextbsd-userland#437),
 * including the daemons ported from Apple — org.nextbsd.configd,
 * org.nextbsd.notifyd, org.nextbsd.IPConfiguration. Those daemons still check
 * in Apple's Mach service names, because Apple-derived clients look the
 * services up by exact string (a clean-room Security.framework carries
 * bootstrap_look_up("com.apple.securityd") in its source, for instance).
 *
 * Neither reason for an Apple service name applies here. Apple's daemon is
 * `wifid`; ours is `wland`, so com.apple.wland would be a FABRICATED Apple name
 * for a daemon Apple never shipped. And nothing Apple-derived looks this
 * service up: we write the `wlan` CLI, we write the System Preferences backend,
 * and the CoreWLAN-shaped ObjC API sits above the Mach line in Gershwin. Both
 * ends are ours, so the service name is ours too.
 */
#include "mach_service.h"
#include "wland.h"			/* wland_got_term */
#include "wlanServer.h"			/* MIG: wlan_server() */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * 8 KiB. wlan.defs carries no OOL data (MIG OOL is broken in this kernel), but
 * wlan_scan_entry returns an SSID + BSSID + four scalars, and wlan_status
 * returns the same — comfortably inside this, with room for the surface to grow.
 */
#define	WLAN_RPC_BUFSZ		8192

#define	WLAN_SERVICE_NAME	"org.nextbsd.wlan"

static pthread_t	worker;
static int		worker_running;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "wland[mach] ");
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

	kr = bootstrap_check_in(bootstrap_port, WLAN_SERVICE_NAME, &sp);
	if (kr != KERN_SUCCESS) {
		xlog("bootstrap_check_in('%s') failed: 0x%x — RPC off",
		    WLAN_SERVICE_NAME, (unsigned)kr);
		return (NULL);
	}
	xlog("WLAN-RPC-OK: Mach service '%s' checked in (port=0x%x)",
	    WLAN_SERVICE_NAME, (unsigned)sp);

	while (!wland_got_term) {
		union {
			mach_msg_header_t	hdr;
			char			buf[WLAN_RPC_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		(void)memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(req), sp, 500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			xlog("receive failed: 0x%x — service off", (unsigned)mr);
			break;
		}

		(void)memset(&rep, 0, sizeof(rep));
		if (!wlan_server(&req.hdr, &rep.hdr)) {
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
