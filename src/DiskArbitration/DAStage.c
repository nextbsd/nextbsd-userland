/*
 * DAStage.c — the disk stage machine (E15 C5, nextbsd-userland#195).
 *
 * Port of Apple's DAStage.c (__DAStageDispatch and the four stages). Each
 * dispatch walks gDADiskList and, for every disk that is not waiting on a
 * command, runs the first stage it has not done:
 *
 *   Probe     the fs-bundle probe (DAProbe.c, #194), asynchronous; the
 *             first probe after idle re-reads the bundles, the preferences
 *             and the /etc/fstab mount map (#196), each only if its file
 *             changed;
 *   Peek      every session's DARegisterDiskPeekCallback, one at a time
 *             in callback order, each answered before the next is asked
 *             (or timed out, DAQueue.c) (#197);
 *   Mount     the mount request (DARequest.c: approval, then
 *             DAMountWithArguments(..., "automatic")) unless the
 *             DAAutoMountDisable preference is set; the disk waits for it,
 *             so the appearance carries DAVolumePath; a dirty volume waits
 *             until every other volume of its disk is probed and staged, so
 *             clean siblings mount first (Apple's rule);
 *   Appeared  DADiskAppearedCallback to every session (#197).
 *
 * After the disks, the request list (DARequest.c, #198): clients' unmount
 * and eject requests run once their disk has appeared and is not busy.
 *
 * When no disk has anything left to do the daemon is idle (gDAIdle) and
 * sessions that have not been told so get DAIdleCallback (#197).
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
#include "DAQueue.h"
#include "DARequest.h"
#include "DASession.h"
#include "DASupport.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static dispatch_queue_t	g_queue;
static int		g_pending;
static int		g_fresh = 1;
int			gDAIdle;

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
		DAMountMapListRefresh1();
		g_fresh = 0;
	}
	disk->state |= kDADiskStateStagedProbe | kDADiskStateCommandActive;
	if (DAProbeDisk(disk) != 0) {
		/* Nothing to probe (a disk without a name). */
		disk->state &= ~kDADiskStateCommandActive;
		DAStageSignal();
	}
}

/* A disk's peek: the candidates, in order, and the next one to ask. */
struct da_peek {
	struct da_callback	**candidates;	/* copies; each holds its session */
	int			 count;
	int			 next;
};

static void
peek_free(struct da_peek *peek)
{
	int i;

	for (i = 0; i < peek->count; i++)
		DACallbackFree(peek->candidates[i]);
	free(peek->candidates);
	free(peek);
}

static int
peek_compare(const void *a, const void *b)
{
	const struct da_callback *ca = *(struct da_callback *const *)a;
	const struct da_callback *cb = *(struct da_callback *const *)b;

	return (ca->order < cb->order ? -1 : ca->order > cb->order);
}

/* __DAStagePeekCallback: ask the next candidate, or end the stage. */
static void
peek_next(struct da_disk *disk, CFTypeRef response __unused, void *context)
{
	struct da_peek *peek = context;

	if (disk != NULL && peek->next < peek->count) {
		DADiskPeekCallback(disk, peek->candidates[peek->next++],
		    peek_next, peek);
		return;
	}
	if (disk != NULL)
		disk->state &= ~kDADiskStateCommandActive;
	peek_free(peek);
	DAStageSignal();
}

/* __DAStagePeek. */
static void
stage_peek(struct da_disk *disk)
{
	struct da_session *session;
	struct da_callback *callback;
	struct da_peek *peek;
	int n = 0;

	disk->state |= kDADiskStateStagedPeek;
	TAILQ_FOREACH(session, &gDASessionList, link)
		TAILQ_FOREACH(callback, &session->callbacks, link)
			if (callback->kind == _kDADiskPeekCallback)
				n++;
	if (n == 0) {
		DAStageSignal();
		return;
	}
	peek = calloc(1, sizeof(*peek));
	if (peek == NULL ||
	    (peek->candidates = calloc((size_t)n, sizeof(*peek->candidates))) ==
	    NULL) {
		free(peek);
		DALog("DA-STAGE: %s: out of memory for the peek", disk->id);
		DAStageSignal();
		return;
	}
	TAILQ_FOREACH(session, &gDASessionList, link)
		TAILQ_FOREACH(callback, &session->callbacks, link) {
			if (callback->kind != _kDADiskPeekCallback ||
			    peek->count >= n)
				continue;
			peek->candidates[peek->count] = DACallbackCreate(
			    session, callback->address, callback->context,
			    callback->kind, callback->order, callback->match,
			    callback->watch);
			if (peek->candidates[peek->count] != NULL)
				peek->count++;
		}
	qsort(peek->candidates, (size_t)peek->count,
	    sizeof(*peek->candidates), peek_compare);
	disk->state |= kDADiskStateCommandActive;
	peek_next(disk, NULL, peek);
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

/* The disk the mount stage waits for, by ID and serial. */
struct da_stage_mount {
	char		*id;
	uint64_t	 serial;
};

static void
stage_mount_done(int32_t status __unused, const char *mountpoint __unused,
    void *context)
{
	struct da_stage_mount *ctx = context;
	struct da_disk *disk;

	disk = DADiskListFind(ctx->id);
	if (disk != NULL && disk->serial == ctx->serial)
		disk->state &= ~kDADiskStateCommandActive;
	free(ctx->id);
	free(ctx);
	DAStageSignal();
}

static void
stage_mount(struct da_disk *disk)
{
	struct da_stage_mount *ctx;

	disk->state |= kDADiskStateStagedMount;
	if (!DAMountGetPreference(disk, kDAMountPreferenceDisableAutoMount)) {
		if (disk->filesystem != NULL &&
		    DADiskGetDescription(disk,
		    kDADiskDescriptionVolumePathKey) == NULL) {
			ctx = calloc(1, sizeof(*ctx));
			if (ctx == NULL || (ctx->id = strdup(disk->id)) ==
			    NULL) {
				free(ctx);
				DALog("DA-STAGE: %s: out of memory for the "
				    "mount", disk->id);
			} else {
				ctx->serial = disk->serial;
				disk->state |= kDADiskStateCommandActive;
				DARequestMount(disk, NULL, 1, NULL,
				    stage_mount_done, ctx);
				return;
			}
		}
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
	DADiskAppearedCallback(disk);
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

	/* Apple's request loop: unmount and eject requests (#198). */
	if (DARequestListDispatch())
		quiet = 0;

	if (!quiet) {
		gDAIdle = 0;
		return;
	}
	g_fresh = 1;
	if (gDAIdle)
		return;
	gDAIdle = 1;
	DAIdleCallback();
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
	/* Apple's DAMain.c reads the mount map at startup too. */
	DAMountMapListRefresh1();
	g_fresh = 0;
}
