/*
 * DAStage.c — the disk stage machine (E15 C5, nextbsd-userland#195).
 *
 * Port of Apple's DAStage.c (__DAStageDispatch and the four stages). Each
 * dispatch walks gDADiskList and, for every disk that is not waiting on a
 * command, runs the first stage it has not done:
 *
 *   Probe     the fs-bundle probe (DAProbe.c, #194), asynchronous; the
 *             first probe after idle re-reads the bundles and preferences
 *             (#196 adds the /etc/fstab map refresh at the same point);
 *   Peek      clients' DARegisterDiskPeekCallback (#197); until sessions
 *             exist the stage only marks itself done;
 *   Mount     DAMountWithArguments(..., "automatic") unless the
 *             DAAutoMountDisable preference is set; a dirty volume waits
 *             until every other volume of its disk is probed and staged, so
 *             clean siblings mount first (Apple's rule);
 *   Appeared  DADiskAppearedCallback to clients (#197); logged for now.
 *
 * When no disk has anything left to do the daemon is idle (gDAIdle;
 * #197's DAIdleCallback goes there).
 *
 * The console-user gate (#199) sits inside the Mount stage, in
 * DAMountWithArguments: a deferred mount returns ECANCELED and leaves the
 * disk staged with _kDADiskStateMountAutomatic still set, which is what
 * #199's login callback looks for when it mounts deferred disks (Apple's
 * _DAConfigurationCallback). Nothing here changes for it.
 *
 * Apple's busy timer (I/O Kit quiesce) and its unreadable/unrepairable
 * dialogs are not here: GEOM settles before the walk sees a disk, and the
 * dialogs are #199's agent.
 */
#include "DAStage.h"
#include "DADisk.h"
#include "DAFileSystem.h"
#include "DAMount.h"
#include "DAProbe.h"
#include "DASupport.h"

#include <errno.h>
#include <string.h>

static dispatch_queue_t	g_queue;
static int		g_pending;
static int		g_fresh = 1;
static int		gDAIdle;

/* The whole disk a volume sits on (Apple compares DADiskGetBSDUnit). */
static const struct da_disk *
unit_of(const struct da_disk *disk)
{
	return (disk->whole != NULL ? disk->whole : disk);
}

static void
stage_probe(struct da_disk *disk)
{
	if (g_fresh) {
		DAFileSystemListRefresh();
		DAPreferenceListRefresh();
		/* #196: DAMountMapListRefresh1(). */
		g_fresh = 0;
	}
	disk->state |= kDADiskStateStagedProbe | kDADiskStateCommandActive;
	if (DAProbeDisk(disk) != 0) {
		/* Nothing to probe (a disk without a name). */
		disk->state &= ~kDADiskStateCommandActive;
		DAStageSignal();
	}
}

static void
stage_peek(struct da_disk *disk)
{
	/* #197: walk the sessions' peek callbacks in order, CommandActive. */
	disk->state |= kDADiskStateStagedPeek;
	DAStageSignal();
}

/* A dirty volume mounts after its disk's other volumes are staged. */
static int
repair_wait(const struct da_disk *disk)
{
	const struct da_disk *sub;

	if ((disk->state & kDADiskStateRequireRepair) == 0)
		return (0);
	TAILQ_FOREACH(sub, &gDADiskList, link) {
		if (sub->volume_only || unit_of(sub) != unit_of(disk))
			continue;
		if ((sub->state & kDADiskStateStagedProbe) == 0)
			return (1);
		if ((sub->state & kDADiskStateStagedMount) == 0 &&
		    (sub->state & kDADiskStateRequireRepair) == 0)
			return (1);
	}
	return (0);
}

static void
stage_mount(struct da_disk *disk)
{
	disk->state |= kDADiskStateStagedMount;
	if (!DAMountGetPreference(disk, kDAMountPreferenceDisableAutoMount)) {
		if (disk->filesystem != NULL &&
		    DADiskGetDescription(disk,
		    kDADiskDescriptionVolumePathKey) == NULL)
			DAMountWithArguments(disk, NULL, 1, NULL, NULL, NULL);
	} else if (disk->filesystem != NULL)
		DALog("DA-MOUNT: %s not mounted: DAAutoMountDisable is set",
		    disk->id);
	DAStageSignal();
}

static void
stage_appeared(struct da_disk *disk)
{
	char kind[32], path[16];

	disk->state |= kDADiskStateStagedAppear;
	/* #197: DADiskAppearedCallback(disk). */
	(void)DADiskGetDescriptionCString(disk, kDADiskDescriptionVolumeKindKey,
	    kind, sizeof(kind));
	path[0] = '\0';
	if (DADiskGetDescription(disk, kDADiskDescriptionVolumePathKey) != NULL)
		(void)strlcpy(path, "mounted", sizeof(path));
	DALogDebug("DA-STAGE: %s appeared kind=%s %s", disk->id,
	    kind[0] != '\0' ? kind : "-", path);
	DAStageSignal();
}

/* __DAStageDispatch. */
static void
stage_dispatch(void *ctx __unused)
{
	struct da_disk *disk, *next;
	int quiet = 1, total = 0, mounted = 0;

	g_pending = 0;
	TAILQ_FOREACH_SAFE(disk, &gDADiskList, link, next) {
		if (disk->state & kDADiskStateCommandActive) {
			quiet = 0;
			continue;
		}
		if ((disk->state & kDADiskStateStagedProbe) == 0)
			stage_probe(disk);
		else if ((disk->state & kDADiskStateStagedPeek) == 0)
			stage_peek(disk);
		else if ((disk->state & kDADiskStateStagedMount) == 0) {
			if (repair_wait(disk)) {
				quiet = 0;
				continue;
			}
			stage_mount(disk);
		} else if ((disk->state & kDADiskStateStagedAppear) == 0)
			stage_appeared(disk);
		else
			continue;
		quiet = 0;
	}

	if (!quiet) {
		gDAIdle = 0;
		return;
	}
	g_fresh = 1;
	if (gDAIdle)
		return;
	gDAIdle = 1;
	/* #197: DAIdleCallback(). */
	TAILQ_FOREACH(disk, &gDADiskList, link) {
		if (disk->volume_only)
			continue;
		total++;
		if (DADiskGetDescription(disk,
		    kDADiskDescriptionVolumePathKey) != NULL)
			mounted++;
	}
	DALog("DA-STAGE: idle, %d disks staged, %d mounted", total, mounted);
}

void
DAStageSignal(void)
{
	if (g_queue == NULL || g_pending)
		return;
	g_pending = 1;
	dispatch_async_f(g_queue, NULL, stage_dispatch);
}

void
DAStageInit(dispatch_queue_t queue)
{
	g_queue = queue;
	DAPreferenceListRefresh();
	g_fresh = 0;
}
