/*
 * diskarbitrationd — NextBSD DiskArbitration daemon.
 *
 * The daemon claims the com.apple.DiskArbitration.diskarbitrationd Mach
 * service (Apple's _kDADaemonName, DAInternal.h), learns of
 * storage arrival/removal from the kernel registry (/dev/ioregistry,
 * da_iokit_subscribe.c) and keeps a disk model built from GEOM (DADisk.c,
 * DAGeom.c, #192): one DADisk per whole disk and per partition, each with
 * Apple's kDADiskDescription* dictionary, logged as DA-DISK lines. Mounts it
 * did not make are adopted from EVFILT_FS + getmntinfo (DAVolume.c, #193):
 * each sets DAVolumePath on its media disk or becomes a disk of its own.
 * New media walks Apple's stage machine (DAStage.c, #195): it is probed
 * through the filesystem bundles in /System/Library/Filesystems
 * (DAFileSystem.c, DAProbe.c, #194), which sets the volume kind, name, UUID
 * and repair state, and then mounted on /Volumes/<name> with Apple's
 * untrusted-media defaults (DAMount.c, DAMountHelper.c, DACommand.c).
 * Dirty volumes are never repaired automatically; they mount read-only.
 * The daemon owns /Volumes: it creates and sweeps it at startup.
 * /etc/fstab, when an admin creates one, is a policy overlay on disks the
 * daemon has found (UUID=, LABEL=, DEVICE= lines: mount point, options,
 * auto/noauto; DASupport.c, #196), never a list of things to mount.
 * DiskArbitration clients (libDiskArbitration, #197) talk to it over the
 * DAServer.defs MIG protocol (DAServer.c): sessions, registered callbacks
 * pulled through a doorbell (DASession.c, DAQueue.c), disk descriptions
 * and mount requests (DARequest.c). The main thread runs the MIG receive
 * loop; the disk model, the stages and every routine run on one serial
 * queue.
 *
 * Plan (E15, nextbsd/nextbsd#471):
 *   D2 #198 — unmount/eject with approval.
 *   D4 #199 — console user: deferred mounts, owner.
 */
#include "da_iokit_subscribe.h"
#include "DADisk.h"
#include "DAGeom.h"
#include "DAInternal.h"
#include "DAMount.h"
#include "DAProbe.h"
#include "DAQueue.h"
#include "DAServer.h"
#include "DAStage.h"
#include "DAVolume.h"

#include <dispatch/dispatch.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <notify.h>

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

	/* -d: debug detail (diskarbitrationd(8)). */
	if (argc > 1 && strcmp(argv[1], "-d") == 0)
		gDALogDebug = 1;

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
	 * our receive port. The name is Apple's, because clients look it up
	 * by that name (DASessionCreate).
	 */
	kr = bootstrap_check_in(bootstrap_port, _kDADaemonName, &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		xlog("DA-SERVICE-FAIL: bootstrap_check_in(%s): 0x%x",
		    _kDADaemonName, (unsigned)kr);
		return (1);
	}
	xlog("DA-SERVICE: %s checked in (receive right=0x%x)",
	    _kDADaemonName, (unsigned)svc);

	/*
	 * /Volumes before anything can mount on it (Apple's
	 * __DAMainCreateMountPointFolder, called from main).
	 */
	DAMountPointFolderCreate();

	/*
	 * One serial queue for the disk model: the GEOM walks, the mount
	 * refreshes (#193) and the stage machine (#195) run there, and the
	 * MIG server (#197) joins it.
	 */
	da_queue = dispatch_queue_create("org.nextbsd.diskarbitrationd", NULL);
	if (da_queue == NULL) {
		xlog("DA-GEOM-FAIL: dispatch_queue_create failed");
		return (1);
	}
	if (DAServerInit(svc, da_queue) != 0)
		return (1);
	{
		DAQueueInit(da_queue);
		DAStageInit(da_queue);
		DAMountInit(da_queue);
		DAGeomInit(da_queue);
		DAVolumeInit(da_queue);
		DAProbeInit(da_queue);
	}

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
	 * Tell keep-alive sessions of an earlier daemon to come back (Apple's
	 * DAMain.c posts the same name), then serve clients until a signal.
	 */
	(void)notify_post(_kDADaemonLaunchedNotification);
	DAServerRun(&got_term);

	xlog("exiting on signal %d", (int)got_term);
	return (0);
}
