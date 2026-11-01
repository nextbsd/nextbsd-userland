/*
 * DAVolume.c — adopt mounts the daemon did not make (E15 C3,
 * nextbsd-userland#193).
 *
 * Port of Apple's embedded-build path (DAMain.c): an EVFILT_FS source with
 * VQ_MOUNT/VQ_UNMOUNT/VQ_UPDATE, whose handler walks getmntinfo(3) the way
 * _DAVolumeMountedCallback and _DAVolumeUnmountedCallback (DAServer.c) do:
 *
 *   - the volume ID is f_mntfromname when it is a /dev path, else
 *     "<f_mntonname>?owner=<f_owner>" (_DAVolumeGetID, DAInternal.c);
 *   - a mount of media the daemon already has as a disk (by ID or by one of
 *     its _deviceLink aliases, since NextBSD mounts /dev/ufs/ROOTFS rather
 *     than /dev/vtbd0p3) sets DAVolumePath on that disk; this is how a
 *     #195 mount completes;
 *   - any other mount, except devfs and union ones (Apple's two skips;
 *     NextBSD adds unionfs, its live-media root), becomes a disk of its
 *     own (DADiskCreateFromVolumePath);
 *   - when a mount goes away, its stand-alone disk is removed, or the media
 *     disk loses DAVolumePath, and a /Volumes mount point the daemon made
 *     for it is removed (DAMount.c, #195).
 *
 * libdispatch has DISPATCH_SOURCE_TYPE_VFS only in its private headers and
 * built without VQ_UPDATE, so the daemon owns a kqueue with the EVFILT_FS
 * knote. A dedicated thread blocks in kevent(2) on it and hands each batch
 * of events to the daemon queue. (With a DISPATCH_SOURCE_TYPE_READ source on
 * the kqueue fd, a CI boot showed one VQ_UPDATE but not the NFS gate's later
 * mount and unmount, although vfs_domount_first() and dounmount() signal
 * both.) The NextBSD kernel posts
 * VQ_UPDATE on MNT_UPDATE (nextbsd-kernel patch 0064, the event launchd's
 * /var check uses), so remounts are seen too.
 */
#include "DAVolume.h"
#include "DADisk.h"
#include "DAMount.h"
#include "DAStage.h"

#include <sys/param.h>
#include <sys/event.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <errno.h>
#include <paths.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef VQ_UPDATE
#define VQ_UPDATE	0x0100	/* NextBSD kernel patch 0064 (Darwin value) */
#endif

static dispatch_queue_t		g_queue;
static pthread_t		g_thread;
static int			g_kq = -1;
static int			g_ready;	/* first refresh has run */
static uint32_t			g_gen;

/*
 * Pseudo filesystems: adopted like any other mount (Apple skips only devfs
 * and union mounts; clients filter), but logged at debug level only.
 */
static int
is_pseudo(const char *kind)
{
	static const char *pseudo[] = {
		"fdescfs", "procfs", "linprocfs", "linsysfs", "mqueuefs",
		"autofs",
	};
	size_t i;

	for (i = 0; i < nitems(pseudo); i++)
		if (strcmp(kind, pseudo[i]) == 0)
			return (1);
	return (0);
}

/* _DAVolumeGetID (DAInternal.c). */
static void
volume_id(const struct statfs *fs, char *buf, size_t len)
{
	if (strncmp(fs->f_mntfromname, _PATH_DEV, sizeof(_PATH_DEV) - 1) == 0)
		(void)strlcpy(buf, fs->f_mntfromname, len);
	else
		(void)snprintf(buf, len, "%s?owner=%u", fs->f_mntonname,
		    (unsigned)fs->f_owner);
}

/* DAVolumePath of `disk` as a filesystem path; -1 if it has none. */
static int
volume_path(const struct da_disk *disk, char *buf, size_t len)
{
	CFTypeRef v = DADiskGetDescription(disk, kDADiskDescriptionVolumePathKey);

	if (v == NULL || CFGetTypeID(v) != CFURLGetTypeID())
		return (-1);
	if (!CFURLGetFileSystemRepresentation((CFURLRef)v, true,
	    (UInt8 *)buf, (CFIndex)len))
		return (-1);
	return (0);
}

/* Last path component; "/" stays "/". Stand-in until #194 probes names. */
static const char *
path_name(const char *path)
{
	const char *p = strrchr(path, '/');

	if (p == NULL || p[1] == '\0')
		return (path);
	return (p + 1);
}

/* The keys DADiskCreateFromVolumePath and a refresh set from a statfs. */
static void
set_volume_keys(struct da_disk *disk, const struct statfs *fs)
{
	CFURLRef url;

	url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)fs->f_mntonname, (CFIndex)strlen(fs->f_mntonname),
	    true);
	DADiskSetDescription(disk, kDADiskDescriptionVolumePathKey, url);
	if (url != NULL)
		CFRelease(url);
	DADiskSetDescriptionString(disk, kDADiskDescriptionVolumeKindKey,
	    fs->f_fstypename);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionVolumeMountableKey, 1);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionVolumeNetworkKey,
	    (fs->f_flags & MNT_LOCAL) == 0);
	if (DADiskGetDescription(disk, kDADiskDescriptionVolumeNameKey) == NULL)
		DADiskSetDescriptionString(disk,
		    kDADiskDescriptionVolumeNameKey, path_name(fs->f_mntonname));
	/*
	 * A mounted disk skips the mount stage (Apple's probe callback sets
	 * StagedMount when getmntinfo shows the disk). Media is still probed
	 * for its kind, name and UUID; a mount-only disk has nothing to probe.
	 */
	disk->state |= kDADiskStateStagedMount;
	if (disk->volume_only)
		disk->state |= kDADiskStateStagedProbe | kDADiskStateStagedPeek;
}

static void
log_volume(const struct da_disk *disk, const struct statfs *fs, int media)
{
	char name[256];
	int pseudo = is_pseudo(fs->f_fstypename);

	(void)DADiskGetDescriptionCString(disk,
	    kDADiskDescriptionVolumeNameKey, name, sizeof(name));
	(pseudo ? DALogDebug : DALog)("DA-VOLUME: %s kind=%s path=%s "
	    "network=%d name=\"%s\" owner=%u media=%d", disk->id,
	    fs->f_fstypename, fs->f_mntonname, (fs->f_flags & MNT_LOCAL) == 0,
	    name, (unsigned)fs->f_owner, media);
}

/* DADiskCreateFromVolumePath (DADisk.c). */
static void
create_from_volume(const char *id, const struct statfs *fs)
{
	struct da_disk *disk;
	struct stat st;

	disk = DADiskCreate(id, "VFS");
	if (disk == NULL) {
		DALog("DA-VOLUME: out of memory adopting %s", fs->f_mntonname);
		return;
	}
	disk->volume_only = 1;
	disk->volume_gen = g_gen;
	disk->user_uid = fs->f_owner;
	set_volume_keys(disk, fs);
	/*
	 * Apple takes major/minor from f_fsid; on NextBSD f_fsid is not a
	 * device number, so use the source device when there is one.
	 */
	if (strncmp(fs->f_mntfromname, _PATH_DEV, sizeof(_PATH_DEV) - 1) == 0 &&
	    stat(fs->f_mntfromname, &st) == 0 && S_ISCHR(st.st_mode)) {
		DADiskSetDescriptionInt32(disk,
		    kDADiskDescriptionMediaBSDMajorKey, (int32_t)major(st.st_rdev));
		DADiskSetDescriptionInt32(disk,
		    kDADiskDescriptionMediaBSDMinorKey, (int32_t)minor(st.st_rdev));
	}
	DADiskSetDescriptionDouble(disk, kDADiskDescriptionAppearanceTimeKey,
	    CFAbsoluteTimeGetCurrent());
	TAILQ_INSERT_TAIL(&gDADiskList, disk, link);
	log_volume(disk, fs, 0);
}

void
DAVolumeRefresh(void)
{
	struct statfs *list;
	struct da_disk *disk;
	char id[MAXPATHLEN + 32], have[MAXPATHLEN];
	int i, n;

	n = getmntinfo(&list, MNT_NOWAIT);
	if (n <= 0) {
		DALog("DA-VFS-FAIL: getmntinfo: %s", strerror(errno));
		return;
	}
	g_ready = 1;
	g_gen++;

	for (i = 0; i < n; i++) {
		struct statfs *fs = &list[i];

		/*
		 * Apple's skips: devfs and union mounts. NextBSD's live-media
		 * root is a unionfs mount (/cow over /rofs) rather than
		 * MNT_UNION, so unionfs counts as a union mount too.
		 */
		if (strcmp(fs->f_fstypename, "devfs") == 0 ||
		    strcmp(fs->f_fstypename, "unionfs") == 0 ||
		    (fs->f_flags & MNT_UNION) != 0) {
			DALogDebug("DA-VOLUME: not adopting %s on %s (%s)",
			    fs->f_mntfromname, fs->f_mntonname,
			    fs->f_fstypename);
			continue;
		}

		disk = NULL;
		if (strncmp(fs->f_mntfromname, _PATH_DEV,
		    sizeof(_PATH_DEV) - 1) == 0)
			disk = DADiskListFindMedia(fs->f_mntfromname);
		if (disk == NULL) {
			volume_id(fs, id, sizeof(id));
			disk = DADiskListFind(id);
			if (disk == NULL) {
				create_from_volume(id, fs);
				continue;
			}
		}

		if (!disk->volume_only && (volume_path(disk, have,
		    sizeof(have)) != 0 || strcmp(have, fs->f_mntonname) != 0)) {
			set_volume_keys(disk, fs);
			log_volume(disk, fs, 1);
		}
		disk->volume_gen = g_gen;
	}

	/* _DAVolumeUnmountedCallback: drop what is no longer mounted. */
restart:
	TAILQ_FOREACH(disk, &gDADiskList, link) {
		if (disk->volume_gen == g_gen)
			continue;
		if (disk->volume_only) {
			DALog("DA-VOLUME-GONE: %s", disk->id);
			DADiskListRemove(disk);
			goto restart;
		}
		if (DADiskGetDescription(disk,
		    kDADiskDescriptionVolumePathKey) != NULL) {
			/*
			 * StagedMount stays set, as on Apple: a volume the
			 * user unmounted is not mounted again behind their
			 * back. It mounts again when the media reappears.
			 */
			DADiskSetDescription(disk,
			    kDADiskDescriptionVolumePathKey, NULL);
			DALog("DA-VOLUME-UNMOUNTED: %s", disk->id);
		}
	}

	/* Mount points the daemon made (#195) go with their mount. */
	{
		const char **on = calloc((size_t)n + 1, sizeof(*on));

		if (on != NULL) {
			for (i = 0; i < n; i++)
				on[i] = list[i].f_mntonname;
			DAMountPointListRefresh(on, n);
			free(on);
		}
	}
	DAStageSignal();
}

/* On the daemon queue: one batch of EVFILT_FS fflags from the watch thread. */
static void
vfs_event(void *ctx)
{
	u_int fflags = (u_int)(uintptr_t)ctx;

	DALog("DA-VFS: fflags=0x%x%s%s%s", fflags,
	    (fflags & VQ_MOUNT) ? " VQ_MOUNT" : "",
	    (fflags & VQ_UNMOUNT) ? " VQ_UNMOUNT" : "",
	    (fflags & VQ_UPDATE) ? " VQ_UPDATE" : "");
	if (g_ready)
		DAVolumeRefresh();
}

/*
 * The watch thread: block for the next event, then drain whatever else is
 * already queued, so a mount + unmount pair costs one refresh.
 */
static void *
vfs_watch_thread(void *arg __unused)
{
	struct kevent ev[8];
	struct timespec zero = { 0, 0 };
	u_int fflags;
	int i, n;

	for (;;) {
		n = kevent(g_kq, NULL, 0, ev, (int)nitems(ev), NULL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			DALog("DA-VFS-FAIL: kevent wait: %s", strerror(errno));
			return (NULL);
		}
		fflags = 0;
		do {
			for (i = 0; i < n; i++)
				if (ev[i].filter == EVFILT_FS)
					fflags |= ev[i].fflags;
		} while ((n = kevent(g_kq, NULL, 0, ev, (int)nitems(ev),
		    &zero)) > 0);
		if (fflags != 0)
			dispatch_async_f(g_queue, (void *)(uintptr_t)fflags,
			    vfs_event);
	}
}

void
DAVolumeInit(dispatch_queue_t queue)
{
	struct kevent kev;

	g_queue = queue;
	g_kq = kqueue();
	if (g_kq < 0) {
		DALog("DA-VFS-FAIL: kqueue: %s", strerror(errno));
		return;
	}
	/* EVFILT_FS ignores ident; knotes are EV_CLEAR and accumulate fflags. */
	EV_SET(&kev, 0, EVFILT_FS, EV_ADD | EV_CLEAR,
	    VQ_MOUNT | VQ_UNMOUNT | VQ_UPDATE, 0, NULL);
	if (kevent(g_kq, &kev, 1, NULL, 0, NULL) < 0) {
		DALog("DA-VFS-FAIL: kevent EVFILT_FS: %s", strerror(errno));
		(void)close(g_kq);
		g_kq = -1;
		return;
	}
	if (pthread_create(&g_thread, NULL, vfs_watch_thread, NULL) != 0) {
		DALog("DA-VFS-FAIL: pthread_create");
		(void)close(g_kq);
		g_kq = -1;
		return;
	}
	(void)pthread_detach(g_thread);
	DALog("DA-VFS-ARMED: EVFILT_FS VQ_MOUNT|VQ_UNMOUNT|VQ_UPDATE");
}
