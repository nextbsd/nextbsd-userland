/*
 * diskarbitrationd — NextBSD DiskArbitration daemon.
 *
 * The daemon claims the com.apple.DiskArbitration Mach service, learns of
 * storage arrival/removal from the kernel registry (/dev/ioregistry,
 * da_iokit_subscribe.c) and keeps a disk model built from GEOM (DADisk.c,
 * DAGeom.c, #192): one DADisk per whole disk and per partition, each with
 * Apple's kDADiskDescription* dictionary, logged as DA-DISK lines. Nothing is
 * probed or mounted yet.
 *
 * Plan (E15, nextbsd/nextbsd#471):
 *   C3 #193 — adopt mounts made elsewhere (EVFILT_FS + getmntinfo).
 *   C4 #194 — filesystem bundles and probing.
 *   C5 #195 — stage machine and mount executor (/Volumes).
 *   D1 #197 — DiskArbitration client library over MIG.
 *   D2 #198 — unmount/eject with approval.
 */
#include "da_iokit_subscribe.h"
#include "DAGeom.h"

#include <dispatch/dispatch.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got_term;

static void
on_signal(int sig)
{
	got_term = sig;
}

static void
xlog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "diskarbitrationd %s ", tbuf);

	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	mach_port_t svc = MACH_PORT_NULL;
	dispatch_queue_t da_queue;
	kern_return_t kr;

	(void)argc;
	(void)argv;

	xlog("starting");

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);

	/*
	 * Claim the Mach service. Same pattern configd /
	 * ipconfigd / mDNSResponder use: bootstrap_check_in returns the
	 * receive right for the service port that launchd's plist-
	 * declared MachServices broker has set up. Once we hold the
	 * right, peer bootstrap_look_up calls route through launchd to
	 * our receive port — even before we serve real RPC, the lookup
	 * succeeds, which is what the iter-1 DA-BOOT marker proves.
	 */
	kr = bootstrap_check_in(bootstrap_port, "com.apple.DiskArbitration",
	    &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		xlog("DA-BOOT-FAIL: bootstrap_check_in: 0x%x", (unsigned)kr);
		return (1);
	}
	xlog("DA-BOOT-OK: com.apple.DiskArbitration registered "
	    "(receive right=0x%x)", (unsigned)svc);

	/*
	 * One serial queue for the disk model: the GEOM walks run there, and
	 * later stages (mount adoption, the MIG server) join it.
	 */
	da_queue = dispatch_queue_create("org.nextbsd.diskarbitrationd", NULL);
	if (da_queue != NULL)
		DAGeomInit(da_queue);
	else
		xlog("DA-GEOM-FAIL: dispatch_queue_create failed");

	/*
	 * Subscribe to storage device arrival/removal over the kernel notify
	 * channel (C1.3, #218): libIOKit's IOServiceAddMatchingNotification
	 * registers DA's recv port with the in-kernel registry via IOREGIOCWATCH
	 * on /dev/ioregistry (#225), and the kernel pushes a binary event per
	 * matching device. DA registers a match-all arrival + departure watch and
	 * filters storage names (ada*, da*, nvd*, nda*, cd*, mmcsd*) client-side.
	 *
	 * Non-fatal: if /dev/ioregistry is absent (a kernel image predating K1) or
	 * registration fails, da_iokit_subscribe_start returns -1 and the daemon
	 * stays alive without storage events — KeepAlive respawns / relookups
	 * happen as services appear. (hwregd, the former fallback, was retired in
	 * #218; the kernel is now the registry.)
	 */
	(void)da_iokit_subscribe_start();

	/*
	 * Walk once at startup even if the registry channel is missing; with
	 * it, the initial-match drain above has already asked for this walk
	 * and the two coalesce.
	 */
	DAGeomStorageChanged();

	/*
	 * Hold-alive loop; the work happens on the dispatch queues. SIGTERM /
	 * SIGHUP short-circuit. #197 replaces this with the MIG server.
	 */
	while (!got_term)
		(void)sleep(60);

	xlog("exiting on signal %d", (int)got_term);
	return (0);
}
