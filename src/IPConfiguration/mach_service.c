/*
 * mach_service.c — ipconfigd's Mach service thread.
 *
 * Owns the receive right for com.apple.IPConfiguration. Runs a raw
 * mach_msg(MACH_RCV_MSG | MACH_RCV_TIMEOUT, 500ms) loop and hands
 * each message to ipconfig_server() (the MIG demux for
 * ipconfig.defs). Same shape as configd_serve() / hwregd's
 * mach_service_thread().
 *
 * 500ms receive timeout: keeps the loop awake to notice got_term
 * for a clean shutdown.
 */
#include "mach_service.h"
#include "dhcp_discover.h"		/* got_term */
#include "ipconfigServer.h"		/* MIG: ipconfig_server() */

#include <mach/mach.h>
#include <mach/mig_errors.h>		/* mig_reply_error_t */
#include <servers/bootstrap.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* RPC receive buffer size. ipconfig.defs' payloads travel
 * out-of-line, so a message is only a header, descriptors, scalars, a
 * 16-byte if_name and the audit trailer; 4 KiB is generous. */
#define IPCFG_RPC_BUFSZ		4096

#define IPCFG_SERVICE_NAME	"com.apple.IPConfiguration"

static pthread_t	worker;
static int		worker_running;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[mach] ");
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

	kr = bootstrap_check_in(bootstrap_port, IPCFG_SERVICE_NAME, &sp);
	if (kr != KERN_SUCCESS) {
		xlog("bootstrap_check_in('%s') failed: 0x%x — RPC off",
		    IPCFG_SERVICE_NAME, (unsigned)kr);
		return (NULL);
	}
	xlog("Mach service '%s' checked in (port=0x%x)",
	    IPCFG_SERVICE_NAME, (unsigned)sp);

	while (!got_term) {
		union {
			mach_msg_header_t	hdr;
			char			buf[IPCFG_RPC_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		(void)memset(&req, 0, sizeof(req));
		/*
		 * Ask for the audit trailer: routines declared with
		 * ServerAuditToken (ipconfig_set's root check) read the
		 * caller's credentials from it, and the MIG demux rejects
		 * them with MIG_TRAILER_ERROR if it is missing. Same
		 * receive options launchd's and syslogd's servers use.
		 */
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
		    MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
		    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
		    0, sizeof(req), sp, 500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			xlog("receive failed: 0x%x — service off",
			    (unsigned)mr);
			break;
		}

		(void)memset(&rep, 0, sizeof(rep));
		if (!ipconfig_server(&req.hdr, &rep.hdr)) {
			xlog("ignoring message id=%d", req.hdr.msgh_id);
			/* Unclaimed: its reply right and any OOL are ours. */
			mach_msg_destroy(&req.hdr);
			continue;
		}
		if (!(rep.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
		    ((mig_reply_error_t *)(void *)&rep.hdr)->RetCode !=
		    KERN_SUCCESS) {
			/*
			 * MIG rejected the request before a routine took
			 * ownership of it (bad arguments, a missing audit
			 * trailer): its out-of-line memory is still ours to
			 * release, as Apple's mach_msg_server() and configd
			 * do. The reply consumes the reply right, so keep
			 * that out of the destroy.
			 */
			req.hdr.msgh_remote_port = MACH_PORT_NULL;
			mach_msg_destroy(&req.hdr);
		}
		if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
			mr = mach_msg(&rep.hdr,
			    MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 200,
			    MACH_PORT_NULL);
			if (mr != MACH_MSG_SUCCESS) {
				xlog("reply send failed: 0x%x",
				    (unsigned)mr);
				/* Drop the reply's OOL rather than leak it. */
				mach_msg_destroy(&rep.hdr);
			}
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
