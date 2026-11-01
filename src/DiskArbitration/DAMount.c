/*
 * DAMount.c — the mount decision, mount points and /Volumes (E15 C5,
 * nextbsd-userland#195).
 *
 * Port of Apple's DAMount.c (DAMountWithArguments and its callback
 * stages, DAMountCreateMountPointWithAction, DAMountRemoveMountPoint,
 * DAMountGetPreference), the mount half of DAFileSystem.c and
 * __DAMainCreateMountPointFolder (DAMain.c), minus the APFS, TDM, FSKit,
 * UserFS and snapshot branches. The decision, in Apple's order:
 *
 *   1. automatic, and the bundle entry said autodiskmount=false (the EFI
 *      system partition): ECANCELED, nothing is mounted.
 *   2. automatic, and nobody is at the console: deferred for removable and
 *      external media (Apple's defaults; internal media mount at boot).
 *      NextBSD has no console-user source until #199, so DAConsoleUserGet
 *      reports root and nothing is deferred yet (DA-MOUNT-DEFER-NOOP).
 *   3. media not writable: "ro" (Apple's rdonly).
 *   4. untrusted (removable, or external): Apple's nosuid,noowners,nodev,
 *      mapped onto NextBSD's mount(8):
 *        nosuid   -> nosuid
 *        nodev    -> nothing: the kernel has no MNT_NODEV and device nodes
 *                    exist only on devfs, so one on a FAT or UFS stick is
 *                    inert;
 *        noowners -> nothing: no MNT_IGNORE_OWNERSHIP; filesystems without
 *                    ownership get the console user's uid/gid and 755 from
 *                    DAMountHelper.c instead; foreign uids on a UFS stick
 *                    show through;
 *      plus two NextBSD choices Apple does not make, each a preference
 *      (DASupport.h): noatime (default on, flash writes) and noexec
 *      (default off, so programs on a stick still run).
 *   5. dirty (the probe's -q said so): Apple repairs with -y and, if that
 *      fails, mounts force,rdonly. NextBSD never repairs automatically: an
 *      automatic mount of a dirty volume goes straight to read-only, the
 *      disk keeps kDADiskStateRequireRepair for #199's agent to offer a
 *      repair, and quotas are not repaired either. NTFS always reports
 *      dirty (fsutil cannot read the $Volume flag yet), so NTFS mounts
 *      read-only until it can. A client mount of a dirty volume without
 *      "force" or "ro" fails with DA_EDIRTY, as on Apple.
 *   6. mount point: /Volumes/<DAVolumeName or "Untitled">, then "<name> 1"
 *      to "<name> 99"; '/' becomes ':'; mkdir 0111, chown to the owner.
 *   7. /sbin/mount -t <kind> -o <options> <device> <mountpoint>, as root.
 *      Apple's -k does not exist in NextBSD's mount(8) and is dropped.
 *      Apple runs mount as the disk owner; an unprivileged mount here
 *      would need vfs.usermount (kernel patch 0008, which
 *      nextbsd-kernel#229 wants gone) and device-node access, so root.
 *      Helpers outside /sbin go in as -o mountprog=<FSMountExecutable>.
 *      There is no retry: a failed mount is a failed mount, as on Apple.
 *   8. success: the EVFILT_FS refresh (DAVolume.c, #193) sets DAVolumePath,
 *      which is the "mounted" signal clients (#197) watch.
 *
 * The daemon owns /Volumes: it creates and sweeps the folder at startup,
 * and removes only the mount points it made (gDAMountPointList), once
 * they are no longer mounted on.
 *
 * #196 adds the /etc/fstab map between steps 1 and 2 (its options go in
 * before the trust defaults, its mount point is used when the caller gave
 * none); #199 adds the console user to step 2.
 */
#include "DAMount.h"
#include "DACommand.h"
#include "DAFileSystem.h"
#include "DAStage.h"
#include "DASupport.h"
#include "DAVolume.h"

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GID_WHEEL
#define GID_WHEEL	0
#endif

/* Apple's DAMain.c kDAMainMountPointFolderCookieFile. */
#define DA_MOUNT_POINT_COOKIE	".autodiskmounted"

/* gDAMountPointList: the mount points this daemon made. */
struct da_mount_point {
	TAILQ_ENTRY(da_mount_point)	link;
	char				*path;
	int				 inflight;	/* mount not finished */
};
static TAILQ_HEAD(, da_mount_point) gDAMountPointList =
    TAILQ_HEAD_INITIALIZER(gDAMountPointList);

static dispatch_queue_t	g_queue;

/* --- preferences ---------------------------------------------------------- */

static int
desc_true(const struct da_disk *disk, CFStringRef key)
{
	return (DADiskGetDescription(disk, key) == kCFBooleanTrue);
}

static int
desc_false(const struct da_disk *disk, CFStringRef key)
{
	return (DADiskGetDescription(disk, key) == kCFBooleanFalse);
}

int
DAMountGetPreference(const struct da_disk *disk, DAMountPreference preference)
{
	int removable = desc_true(disk, kDADiskDescriptionMediaRemovableKey);
	int internal = desc_true(disk, kDADiskDescriptionDeviceInternalKey);

	switch (preference) {
	case kDAMountPreferenceDefer:
		if (removable)
			return (DAPreferenceGetBool(
			    kDAPreferenceMountDeferRemovableKey, 1));
		if (internal)
			return (DAPreferenceGetBool(
			    kDAPreferenceMountDeferInternalKey, 0));
		return (DAPreferenceGetBool(
		    kDAPreferenceMountDeferExternalKey, 1));
	case kDAMountPreferenceTrust:
		if (removable)
			return (DAPreferenceGetBool(
			    kDAPreferenceMountTrustRemovableKey, 0));
		if (internal)
			return (DAPreferenceGetBool(
			    kDAPreferenceMountTrustInternalKey, 1));
		return (DAPreferenceGetBool(
		    kDAPreferenceMountTrustExternalKey, 0));
	case kDAMountPreferenceDisableAutoMount:
		return (DAPreferenceGetBool(kDAPreferenceAutoMountDisableKey,
		    0));
	}
	return (0);
}

/* --- gDAMountPointList ---------------------------------------------------- */

static struct da_mount_point *
mount_point_find(const char *path)
{
	struct da_mount_point *mp;

	TAILQ_FOREACH(mp, &gDAMountPointList, link)
		if (strcmp(mp->path, path) == 0)
			return (mp);
	return (NULL);
}

static void
mount_point_add(const char *path)
{
	struct da_mount_point *mp;

	mp = calloc(1, sizeof(*mp));
	if (mp == NULL || (mp->path = strdup(path)) == NULL) {
		free(mp);
		return;
	}
	mp->inflight = 1;
	TAILQ_INSERT_TAIL(&gDAMountPointList, mp, link);
}

/*
 * DAMountRemoveMountPoint: rmdir a mount point this daemon made. rmdir(2)
 * refuses a directory that is mounted on or not empty, so a busy one stays.
 */
static void
mount_point_remove(struct da_mount_point *mp)
{
	if (rmdir(mp->path) != 0 && errno != ENOENT)
		DALog("DA-MOUNT: cannot remove mount point \"%s\": %s",
		    mp->path, strerror(errno));
	else
		DALogDebug("DA-MOUNT: removed mount point \"%s\"", mp->path);
	TAILQ_REMOVE(&gDAMountPointList, mp, link);
	free(mp->path);
	free(mp);
}

void
DAMountPointListRefresh(const char *const *mounted, int count)
{
	struct da_mount_point *mp, *next;
	int i;

	TAILQ_FOREACH_SAFE(mp, &gDAMountPointList, link, next) {
		if (mp->inflight)
			continue;
		for (i = 0; i < count; i++)
			if (strcmp(mounted[i], mp->path) == 0)
				break;
		if (i == count)
			mount_point_remove(mp);
	}
}

/* --- /Volumes ------------------------------------------------------------- */

void
DAMountPointFolderCreate(void)
{
	struct stat st, est;
	struct dirent *de;
	DIR *d;
	int swept = 0, kept = 0;

	if (access(DA_MOUNT_POINT_FOLDER, F_OK) != 0) {
		if (mkdir(DA_MOUNT_POINT_FOLDER, 0755) != 0) {
			/*
			 * Apple exits with EX_SOFTWARE here. launchd would only
			 * respawn the daemon into the same failure and the disk
			 * model with it, so log and go on; mounts then fail.
			 */
			DALog("DA-VOLUMES: cannot create %s: %s",
			    DA_MOUNT_POINT_FOLDER, strerror(errno));
			return;
		}
		(void)chown(DA_MOUNT_POINT_FOLDER, 0, GID_WHEEL);
		DALog("DA-VOLUMES: created %s (0755 root:wheel)",
		    DA_MOUNT_POINT_FOLDER);
		return;
	}

	(void)chmod(DA_MOUNT_POINT_FOLDER, 0755);
	(void)chown(DA_MOUNT_POINT_FOLDER, (uid_t)-1, GID_WHEEL);
	if (stat(DA_MOUNT_POINT_FOLDER, &st) != 0 ||
	    (d = opendir(DA_MOUNT_POINT_FOLDER)) == NULL) {
		DALog("DA-VOLUMES: cannot read %s: %s", DA_MOUNT_POINT_FOLDER,
		    strerror(errno));
		return;
	}
	while ((de = readdir(d)) != NULL) {
		char path[MAXPATHLEN], file[MAXPATHLEN];

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		(void)snprintf(path, sizeof(path), "%s/%s",
		    DA_MOUNT_POINT_FOLDER, de->d_name);
		if (lstat(path, &est) != 0)
			continue;
		if (S_ISDIR(est.st_mode)) {
			/*
			 * Apple unlinks its cookie file first. Only in a plain
			 * directory: a different st_dev is a live mount, and a
			 * file of that name on someone's volume is theirs.
			 */
			if (est.st_dev == st.st_dev) {
				(void)snprintf(file, sizeof(file), "%s/%s",
				    path, DA_MOUNT_POINT_COOKIE);
				(void)unlink(file);
			}
			if (rmdir(path) == 0) {
				swept++;
				DALog("DA-VOLUMES: removed stale \"%s\"", path);
			} else
				kept++;
		} else if (S_ISLNK(est.st_mode)) {
			if (unlink(path) == 0) {
				swept++;
				DALog("DA-VOLUMES: removed stale link \"%s\"",
				    path);
			} else
				kept++;
		} else
			kept++;
	}
	(void)closedir(d);
	DALog("DA-VOLUMES: %s is 0%o uid=%u gid=%u, %d stale removed, "
	    "%d in use kept", DA_MOUNT_POINT_FOLDER,
	    (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid,
	    (unsigned)st.st_gid, swept, kept);
}

/*
 * DAMountCreateMountPointWithAction(kDAMountPointActionMake): the first of
 * /Volumes/<name>, "<name> 1" ... "<name> 99" that mkdir(2) creates.
 */
static int
mount_point_create(const struct da_disk *disk, char *path, size_t len)
{
	char name[NAME_MAX + 1];
	size_t max, n;
	char *p;
	int i;

	if (DADiskGetDescriptionCString(disk, kDADiskDescriptionVolumeNameKey,
	    name, sizeof(name)) != 0 || name[0] == '\0')
		(void)strlcpy(name, "Untitled", sizeof(name));

	/*
	 * Apple truncates to MNAMELEN - 20. NextBSD's MNAMELEN is 1024 but a
	 * path component is NAME_MAX, so keep room for " 99" under that, and
	 * cut on a UTF-8 character boundary.
	 */
	max = MNAMELEN - 20;
	if (max > NAME_MAX - 3)
		max = NAME_MAX - 3;
	n = strlen(name);
	if (n > max) {
		n = max;
		while (n > 0 && ((unsigned char)name[n] & 0xc0) == 0x80)
			n--;
		name[n] = '\0';
	}
	while ((p = strchr(name, '/')) != NULL)
		*p = ':';
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		(void)strlcpy(name, "Untitled", sizeof(name));

	for (i = 0; i < 100; i++) {
		if (i == 0)
			(void)snprintf(path, len, "%s/%s",
			    DA_MOUNT_POINT_FOLDER, name);
		else
			(void)snprintf(path, len, "%s/%s %d",
			    DA_MOUNT_POINT_FOLDER, name, i);
		if (mkdir(path, 0111) == 0) {
			if (disk->user_uid != 0)
				(void)chown(path, disk->user_uid, (gid_t)-1);
			mount_point_add(path);
			return (0);
		}
		if (errno != EEXIST)
			break;
	}
	path[0] = '\0';
	return (-1);
}

/* --- already mounted? ----------------------------------------------------- */

/*
 * Apple's probe callback checks getmntinfo before the mount stage. Here
 * the mount refresh (DAVolume.c) normally has set DAVolumePath already;
 * this is the last check before running mount(8): any mount whose source
 * is this disk, by name, by one of its aliases (/dev/ufs/ROOTFS) or by
 * device number, means the disk is in use and is not mounted again.
 */
static int
mounted_elsewhere(const struct da_disk *disk, char *on, size_t len)
{
	struct statfs *list;
	struct stat dst, mst;
	int i, j, n, have_dst;

	have_dst = stat(disk->id, &dst) == 0 && S_ISCHR(dst.st_mode);
	n = getmntinfo(&list, MNT_NOWAIT);
	for (i = 0; i < n; i++) {
		const char *from = list[i].f_mntfromname;
		int match = strcmp(from, disk->id) == 0;

		for (j = 0; !match && j < DA_DISK_LINK_MAX &&
		    disk->device_link[j] != NULL; j++)
			match = strcmp(from, disk->device_link[j]) == 0;
		if (!match && have_dst &&
		    strncmp(from, _PATH_DEV, sizeof(_PATH_DEV) - 1) == 0 &&
		    stat(from, &mst) == 0 && S_ISCHR(mst.st_mode))
			match = mst.st_rdev == dst.st_rdev;
		if (match) {
			(void)strlcpy(on, list[i].f_mntonname, len);
			return (1);
		}
	}
	return (0);
}

/* --- the mount ------------------------------------------------------------ */

struct da_mount_context {
	char		*id;
	uint64_t	 serial;
	char		 mountpoint[MAXPATHLEN];
	int		 made;		/* the daemon created mountpoint */
	DAMountCallback	 callback;
	void		*context;
};

static void
mount_finish(struct da_mount_context *ctx, int status)
{
	if (ctx->callback != NULL)
		ctx->callback(status, status == 0 ? ctx->mountpoint : NULL,
		    ctx->context);
	free(ctx->id);
	free(ctx);
}

/* __DAMountWithArgumentsCallbackStage2, on the daemon queue. */
static void
mount_done(int status, const char *output, void *context)
{
	struct da_mount_context *ctx = context;
	struct da_mount_point *mp;
	struct da_disk *disk;

	disk = DADiskListFind(ctx->id);
	if (disk != NULL && disk->serial != ctx->serial)
		disk = NULL;
	if (disk != NULL)
		disk->state &= ~kDADiskStateMountOngoing;

	mp = ctx->made ? mount_point_find(ctx->mountpoint) : NULL;
	if (status != 0) {
		DALog("DA-MOUNT: %s mount on \"%s\" failed (status %d)%s%s",
		    ctx->id, ctx->mountpoint, status,
		    output[0] != '\0' ? ": " : "", output);
		if (mp != NULL)
			mount_point_remove(mp);
		mount_finish(ctx, status == DA_COMMAND_NOT_RUN ? ENOEXEC :
		    (status > 0 ? EIO : status));
		return;
	}
	if (mp != NULL)
		mp->inflight = 0;
	DALog("DA-MOUNT: %s mounted on \"%s\"", ctx->id, ctx->mountpoint);
	/*
	 * The kernel's VQ_MOUNT reaches DAVolume.c too; refreshing here makes
	 * DAVolumePath appear before the caller's callback runs.
	 */
	DAVolumeRefresh();
	mount_finish(ctx, 0);
}

static int	g_defer_noop_logged;

static void
volume_refresh(void *ctx __unused)
{
	DAVolumeRefresh();
}

void
DAMountWithArguments(struct da_disk *disk, const char *mountpoint,
    int automatic, const char *options, DAMountCallback callback,
    void *context)
{
	const struct da_probe_candidate *fs = disk->filesystem;
	struct da_mount_args args;
	struct da_mount_context *ctx = NULL;
	struct xvfsconf vfc;
	char on[MNAMELEN], prog[MAXPATHLEN + 16], mountprog[MAXPATHLEN];
	const char *vfs;
	int status = 0, force = 0, trusted, decided;
	uid_t uid;
	gid_t gid;

	memset(&args, 0, sizeof(args));

	/* Apple's request layer: nothing to mount, or mounted already. */
	if (fs == NULL || !desc_true(disk,
	    kDADiskDescriptionVolumeMountableKey)) {
		status = ENOTSUP;
		goto out;
	}
	if (DADiskGetDescription(disk, kDADiskDescriptionVolumePathKey) !=
	    NULL || (disk->state & kDADiskStateMountOngoing) != 0) {
		status = EBUSY;
		goto out;
	}

	/* Caller's options; "force" is Apple's argument, not mount's. */
	if (options != NULL) {
		char *copy = strdup(options), *s = copy, *w;

		while (copy != NULL && (w = strsep(&s, ",")) != NULL) {
			if (strcmp(w, "force") == 0)
				force = 1;
			else
				(void)DAMountOptionAppend(args.options,
				    sizeof(args.options), w);
		}
		free(copy);
	}

	/* #196: the /etc/fstab map is consulted here. */

	/* 1-2: may this mount happen automatically now? */
	if (automatic) {
		decided = -1;
		if ((disk->state & _kDADiskStateMountAutomatic) == 0)
			decided = 0;
		else if (disk->state & _kDADiskStateMountAutomaticNoDefer)
			decided = 1;
		if (decided == 0) {
			DALog("DA-MOUNT: %s not mounted automatically: the %s "
			    "entry says autodiskmount=false", disk->id,
			    strrchr(fs->bundle, '/') + 1);
			status = ECANCELED;
			goto out;
		}
		if (decided < 0 && DAMountGetPreference(disk,
		    kDAMountPreferenceDefer) &&
		    !DAConsoleUserGet(&uid, &gid)) {
			DALog("DA-MOUNT: %s: no console users yet, delaying "
			    "mount", disk->id);
			status = ECANCELED;
			goto out;
		}
		if (decided < 0 && DAMountGetPreference(disk,
		    kDAMountPreferenceDefer) && !g_defer_noop_logged) {
			g_defer_noop_logged = 1;
			DALog("DA-MOUNT-DEFER-NOOP: %s would wait for a console "
			    "user; there is no console-user source yet (#199), "
			    "so it mounts now, owned by root", disk->id);
		}
	}
	/* The owner: Apple's DADiskGetUserUID, the console user. */
	if (DAConsoleUserGet(&uid, &gid)) {
		disk->user_uid = uid;
		disk->user_gid = gid;
	}

	/* The helper must exist; FUSE ports and some kernels lack theirs. */
	vfs = fs->user_impl ? "fusefs" : fs->kind;
	if (getvfsbyname(vfs, &vfc) != 0) {
		DALog("DA-MOUNT: %s kind=%s not mounted: this kernel has no "
		    "%s filesystem", disk->id, fs->kind, vfs);
		status = EOPNOTSUPP;
		goto out;
	}
	mountprog[0] = '\0';
	if (fs->mount_exec != NULL) {
		if (access(fs->mount_exec, X_OK) != 0) {
			DALog("DA-MOUNT: %s kind=%s not mounted: %s is not "
			    "installed", disk->id, fs->kind, fs->mount_exec);
			status = ENOENT;
			goto out;
		}
		(void)snprintf(prog, sizeof(prog), "/sbin/mount_%s", fs->kind);
		if (strcmp(fs->mount_exec, "/sbin/mount") != 0 &&
		    strcmp(fs->mount_exec, prog) != 0)
			(void)strlcpy(mountprog, fs->mount_exec,
			    sizeof(mountprog));
	}

	/* 3-4: read-only media, trust, filesystem options. */
	DAMountHelperOptions(fs->kind, disk->user_uid, disk->user_gid, &args);
	trusted = DAMountGetPreference(disk, kDAMountPreferenceTrust);
	if (!trusted) {
		if (DAPreferenceGetBool(kDAPreferenceMountNoExecuteUntrustedKey,
		    0))
			(void)DAMountOptionPrepend(args.options,
			    sizeof(args.options), "noexec");
		if (args.noatime && DAPreferenceGetBool(
		    kDAPreferenceMountNoAccessTimeUntrustedKey, 1))
			(void)DAMountOptionPrepend(args.options,
			    sizeof(args.options), "noatime");
		(void)DAMountOptionPrepend(args.options, sizeof(args.options),
		    "nosuid");
	}
	if (desc_false(disk, kDADiskDescriptionMediaWritableKey) ||
	    args.readonly)
		(void)DAMountOptionPrepend(args.options, sizeof(args.options),
		    "ro");

	/* 5: dirty volumes are never repaired automatically. */
	if (disk->state & kDADiskStateRequireRepair) {
		if (!DAMountOptionContains(args.options, "ro")) {
			if (!automatic && !force) {
				status = DA_EDIRTY;
				goto out;
			}
			(void)DAMountOptionPrepend(args.options,
			    sizeof(args.options), "ro");
		}
		DALog("DA-MOUNT: %s RequireRepair: the volume is dirty and is "
		    "not repaired automatically; mounting it read-only",
		    disk->id);
	}
	if (mountprog[0] != '\0') {
		char opt[MAXPATHLEN + 16];

		(void)snprintf(opt, sizeof(opt), "mountprog=%s", mountprog);
		(void)DAMountOptionPrepend(args.options, sizeof(args.options),
		    opt);
	}

	/* Last check before touching anything: is it in use already? */
	if (mounted_elsewhere(disk, on, sizeof(on))) {
		DALog("DA-MOUNT: %s is already mounted on \"%s\"; not mounting "
		    "it again", disk->id, on);
		/*
		 * Let the mount refresh adopt it, after this dispatch: the
		 * stage machine is walking the disk list right now.
		 */
		dispatch_async_f(g_queue, NULL, volume_refresh);
		status = EBUSY;
		goto out;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL || (ctx->id = strdup(disk->id)) == NULL) {
		free(ctx);
		ctx = NULL;
		status = ENOMEM;
		goto out;
	}
	ctx->serial = disk->serial;
	ctx->callback = callback;
	ctx->context = context;

	/* 6: the mount point. */
	if (mountpoint != NULL)
		(void)strlcpy(ctx->mountpoint, mountpoint,
		    sizeof(ctx->mountpoint));
	else if (mount_point_create(disk, ctx->mountpoint,
	    sizeof(ctx->mountpoint)) == 0)
		ctx->made = 1;
	else {
		DALog("DA-MOUNT: %s: no free mount point under %s: %s",
		    disk->id, DA_MOUNT_POINT_FOLDER, strerror(errno));
		free(ctx->id);
		free(ctx);
		ctx = NULL;
		status = ENOSPC;
		goto out;
	}

	/* 7: mount(8). */
	DALog("DA-MOUNT: %s kind=%s on \"%s\" options=%s trusted=%d owner=%u:%u%s",
	    disk->id, fs->kind, ctx->mountpoint,
	    args.options[0] != '\0' ? args.options : "-", trusted,
	    (unsigned)disk->user_uid, (unsigned)disk->user_gid,
	    !trusted && DAMountOptionContains(args.options, "noatime") ?
	    " (noatime: NextBSD default for untrusted media)" : "");
	disk->state |= kDADiskStateMountOngoing;
	{
		char *argv[9];
		int n = 0;

		argv[n++] = "/sbin/mount";
		argv[n++] = "-t";
		argv[n++] = fs->kind;
		if (args.options[0] != '\0') {
			argv[n++] = "-o";
			argv[n++] = args.options;
		}
		argv[n++] = disk->id;
		argv[n++] = ctx->mountpoint;
		argv[n] = NULL;
		DACommandExecute(argv, kDACommandExecuteOptionCaptureStderr,
		    g_queue, mount_done, ctx);
	}
	return;

out:
	if (status != 0 && status != ECANCELED)
		DALogDebug("DA-MOUNT: %s not mounted (status %d)", disk->id,
		    status);
	if (callback != NULL)
		callback(status, NULL, context);
}

void
DAMountInit(dispatch_queue_t queue)
{
	g_queue = queue;
}
