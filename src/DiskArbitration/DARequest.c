/*
 * DARequest.c — the mount request (nextbsd-userland#197), and the unmount
 * and eject requests (#198).
 *
 * Mount: __DARequestMount and its approval step from Apple's
 * diskarbitrationd/DARequest.c (DiskArbitration-535.120.1.0.1). In Apple's
 * order:
 *   1. a client's mount (not the stage machine's) re-reads the /etc/fstab
 *      mount map first (DAMountMapListRefresh1, as Apple's dispatch of a
 *      client request does), so an edited fstab applies without a disk
 *      event;
 *   2. every session with a mount approval callback is asked
 *      (DADiskMountApprovalCallback); the first dissenter stops the mount
 *      and is the answer;
 *   3. the mount executor (DAMountWithArguments, DAMount.c) mounts.
 * Disks with nothing to mount, or mounted already, skip the approval and
 * get the executor's answer directly.
 *
 * Unmount and eject: Apple's request list (gDARequestList, DAQueueRequest
 * in DAQueue.c, the request loop of __DAStageDispatch) with
 * __DARequestUnmount and __DARequestEject, in Apple's order:
 *
 *   unmount  not mountable: kDAReturnUnsupported; not mounted:
 *            kDAReturnNotMounted; mounted on "/": a kDAReturnBusy dissenter,
 *            with no approval round. Otherwise a writable disk is read once
 *            (Apple's tickle), every session with an unmount approval
 *            callback is asked (first dissenter wins, 10 s + 1 s per silent
 *            session, DAQueue.c), and unless one dissented, or the client
 *            passed kDADiskUnmountOptionForce, unmount(2) runs on a worker
 *            thread (MNT_FORCE with Force). A failure is unix_err(errno);
 *            for EBUSY the dissenter also names the first process using the
 *            volume (DADevice.c, libprocstat in place of proc_listpidspath).
 *            Apple turns every failure into EBUSY; NextBSD keeps the real
 *            errno. On success the /Volumes directory goes if the daemon
 *            made it (an /etc/fstab mount point stays), DAVolumePath is
 *            removed (clients get DescriptionChanged), and a disk that was
 *            only a mount (NFS, tmpfs) disappears.
 *   eject    only a whole disk (else kDAReturnUnsupported). Every volume of
 *            the disk is unmounted first, as linked unmount subrequests; a
 *            volume that will not unmount stops the eject with its
 *            dissenter. Then the eject approval round, then the eject on a
 *            worker thread (DADevice.c: CDIOCEJECT for cd, CAM START STOP
 *            UNIT for da, nothing for the rest, Apple's ENOTTY).
 *            Apple's eject expansion makes eject subrequests that answer
 *            kDAReturnUnsupported and leaves the unmounts to its clients
 *            (diskutil, Finder unmount first); NextBSD does the unmounts so a
 *            bare DADiskEject works.
 *   Whole    kDADiskUnmountOptionWhole on a whole disk: an unmount
 *            subrequest for each of its other volumes; the request answers
 *            with its own dissenter or the first linked one (Apple's
 *            __DARequestDispatchCallback).
 *
 * A request runs only when its disk has appeared and no stage or request
 * is active on it, and after every earlier request on the same disk; the
 * unmount and eject proper also wait until nothing on the whole disk is
 * active (Apple's kDAUnitStateCommandActive). Apple's magic 0xF8DAFF01
 * status, hibernation, the APFS and FSKit branches and the
 * vfs.generic.print_busy_vnodes retry have no NextBSD counterpart.
 */
#define __DISKARBITRATIOND__ 1

#include "DARequest.h"
#include "DADevice.h"
#include "DAInternal.h"
#include "DAMount.h"
#include "DAQueue.h"
#include "DAStage.h"
#include "DASupport.h"
#include "DAThread.h"

#include <DiskArbitration/DADissenter.h>

#include <sys/param.h>
#include <sys/mount.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct da_mount_request {
	char			*mountpoint;	/* NULL: /Volumes/<name> */
	char			*options;
	int			 automatic;
	DARequestMountCallback	 callback;
	void			*context;
};

static void
request_free(struct da_mount_request *request)
{
	free(request->mountpoint);
	free(request->options);
	free(request);
}

/* DAMountCallback: errno-style status from the executor. */
static void
mount_done(int status, const char *mountpoint, void *context)
{
	struct da_mount_request *request = context;

	request->callback(status == 0 ? 0 : (int32_t)unix_err(status),
	    mountpoint, request->context);
	request_free(request);
}

static void
mount_now(struct da_disk *disk, struct da_mount_request *request)
{
	DAMountWithArguments(disk, request->mountpoint, request->automatic,
	    request->options, mount_done, request);
}

/* __DARequestMountApprovalCallback. */
static void
approval_done(struct da_disk *disk, CFTypeRef dissenter, void *context)
{
	struct da_mount_request *request = context;
	int32_t status;

	if (disk == NULL) {
		request->callback((int32_t)unix_err(ENXIO), NULL,
		    request->context);
		request_free(request);
		return;
	}
	if (dissenter != NULL) {
		status = (int32_t)___CFDictionaryGetIntegerValue(dissenter,
		    _kDADissenterStatusKey);
		DALog("DA-MOUNT: %s not mounted: a client dissented "
		    "(status 0x%08x)", disk->id, (unsigned)status);
		request->callback(status != 0 ? status :
		    (int32_t)unix_err(EPERM), NULL, request->context);
		request_free(request);
		return;
	}
	mount_now(disk, request);
}

void
DARequestMount(struct da_disk *disk, const char *mountpoint, int automatic,
    const char *options, DARequestMountCallback callback, void *context)
{
	struct da_mount_request *request;

	request = calloc(1, sizeof(*request));
	if (request == NULL ||
	    (mountpoint != NULL &&
	    (request->mountpoint = strdup(mountpoint)) == NULL) ||
	    (options != NULL && (request->options = strdup(options)) == NULL)) {
		if (request != NULL)
			request_free(request);
		callback((int32_t)unix_err(ENOMEM), NULL, context);
		return;
	}
	request->automatic = automatic;
	request->callback = callback;
	request->context = context;

	if (!automatic)
		DAMountMapListRefresh1();

	if (disk->filesystem == NULL ||
	    DADiskGetDescription(disk, kDADiskDescriptionVolumeMountableKey) !=
	    kCFBooleanTrue ||
	    DADiskGetDescription(disk, kDADiskDescriptionVolumePathKey) !=
	    NULL) {
		mount_now(disk, request);
		return;
	}
	DADiskMountApprovalCallback(disk, approval_done, request);
}

/* --- unmount and eject (#198) --------------------------------------------- */

/* Apple's kDARequestState* bit. */
#define kDARequestStateStagedApprove	0x00000001

struct da_request {
	TAILQ_ENTRY(da_request)	 link;		/* gDARequestList */
	_DARequestKind		 kind;
	char			*disk_id;
	uint64_t		 disk_serial;
	int32_t			 options;
	uid_t			 uid;
	gid_t			 gid;
	uint32_t		 state;		/* kDARequestState* */
	int			 active;	/* a round or a thread is out */
	struct da_callback	*callback;	/* NULL for a subrequest */
	CFDataRef		 serialization;	/* the disk, for the callback */
	CFTypeRef		 dissenter;	/* DARequestGetDissenter */

	/* Apple's DARequestGetLink, from either end. */
	struct da_request	*parent;
	int			 linked;	/* subrequests were made */
	int			 pending;	/* subrequests not finished */
	CFTypeRef		 link_dissenter; /* the first one of those */

	/* What the worker thread reads and writes. */
	char			*path;		/* mount point, or device */
	char			**names;	/* eject: device names */
	int			 nnames;
	char			 how[256];	/* eject: what was done */
	int64_t			 blocksize;	/* tickle */
	pid_t			 pid;		/* who keeps it busy */
};

static TAILQ_HEAD(, da_request) gDARequestList =
    TAILQ_HEAD_INITIALIZER(gDARequestList);

static const char *
request_name(const struct da_request *request)
{
	return (request->kind == _kDADiskEject ? "eject" : "unmount");
}

/* The disk the request is for, if it is still the same disk. */
static struct da_disk *
request_disk(const struct da_request *request)
{
	struct da_disk *disk;

	disk = DADiskListFind(request->disk_id);
	if (disk != NULL && disk->serial != request->disk_serial)
		disk = NULL;
	return (disk);
}

static const struct da_disk *
unit_of(const struct da_disk *disk)
{
	return (disk->whole != NULL ? disk->whole : disk);
}

/* DAUnitGetStateRecursively(disk, kDAUnitStateCommandActive). */
static int
unit_busy(const struct da_disk *disk)
{
	const struct da_disk *sub;

	TAILQ_FOREACH(sub, &gDADiskList, link)
		if (unit_of(sub) == unit_of(disk) &&
		    (sub->state & kDADiskStateCommandActive) != 0)
			return (1);
	return (0);
}

/* DAVolumePath as a path; -1 if the disk has none. */
static int
volume_path(const struct da_disk *disk, char *buf, size_t len)
{
	CFTypeRef url = DADiskGetDescription(disk,
	    kDADiskDescriptionVolumePathKey);

	buf[0] = '\0';
	if (url == NULL || CFGetTypeID(url) != CFURLGetTypeID() ||
	    !CFURLGetFileSystemRepresentation(url, TRUE, (UInt8 *)buf,
	    (CFIndex)len))
		return (-1);
	return (0);
}

static void
request_set_dissenter(struct da_request *request, CFTypeRef dissenter)
{
	if (request->dissenter != NULL)
		CFRelease(request->dissenter);
	request->dissenter = dissenter != NULL ? CFRetain(dissenter) : NULL;
}

static void
queued_request_free(struct da_request *request)
{
	int i;

	if (request->callback != NULL)
		DACallbackFree(request->callback);
	if (request->serialization != NULL)
		CFRelease(request->serialization);
	if (request->dissenter != NULL)
		CFRelease(request->dissenter);
	if (request->link_dissenter != NULL)
		CFRelease(request->link_dissenter);
	for (i = 0; i < request->nnames; i++)
		free(request->names[i]);
	free(request->names);
	free(request->path);
	free(request->disk_id);
	free(request);
}

/*
 * __DARequestDispatchCallback: the client's completion, with `dissenter`,
 * or for a request with subrequests its own dissenter or the first linked
 * one.
 */
static void
request_callback(struct da_request *request, CFTypeRef dissenter)
{
	struct da_disk *disk;
	CFDataRef serialization = NULL;

	if (request->callback == NULL)
		return;
	if (request->linked)
		dissenter = request->dissenter != NULL ? request->dissenter :
		    request->link_dissenter;
	disk = request_disk(request);
	if (disk != NULL)
		serialization = DADiskCopySerialization(disk);
	if (serialization == NULL && request->serialization != NULL)
		serialization = CFRetain(request->serialization);
	if (serialization == NULL)
		return;
	DAQueueRequestCallback(request->callback, serialization, dissenter);
	CFRelease(serialization);
}

/* DARequestDispatchCallback: the same, from a status. */
static void
request_dispatch_callback(struct da_request *request, int32_t status)
{
	CFDictionaryRef dissenter = NULL;

	if (status != 0)
		dissenter = DADissenterCreate(status);
	request_callback(request, dissenter);
	if (dissenter != NULL)
		CFRelease(dissenter);
}

/* The request is done: off the list, its dissenter to its parent. */
static void
request_finish(struct da_request *request)
{
	struct da_request *parent = request->parent;

	TAILQ_REMOVE(&gDARequestList, request, link);
	if (parent != NULL) {
		parent->pending--;
		if (parent->link_dissenter == NULL &&
		    request->dissenter != NULL)
			parent->link_dissenter = CFRetain(request->dissenter);
	}
	queued_request_free(request);
	DAStageSignal();
}

static void
log_dissent(const struct da_request *request, CFTypeRef dissenter)
{
	DALog("DA-REQUEST: %s %s: dissented, status 0x%08x, pid %d",
	    request_name(request), request->disk_id,
	    (unsigned)___CFDictionaryGetIntegerValue(dissenter,
	    _kDADissenterStatusKey),
	    (int)___CFDictionaryGetIntegerValue(dissenter,
	    _kDADissenterProcessIDKey));
}

/* --- unmount ---------------------------------------------------------- */

/* __DARequestUnmountApprovalCallback. */
static void
unmount_approval_done(struct da_disk *disk, CFTypeRef dissenter,
    void *context)
{
	struct da_request *request = context;

	if (dissenter != NULL) {
		if ((request->options & kDADiskUnmountOptionForce) == 0)
			request_set_dissenter(request, dissenter);
		else
			DALog("DA-REQUEST: unmount %s: a client dissented; "
			    "forced, going ahead", request->disk_id);
	}
	if (disk != NULL)
		disk->state &= ~kDADiskStateCommandActive;
	request->active = 0;
	DAStageSignal();
}

/* __DARequestUnmountTickle: read one block so a sleeping disk spins up. */
static int
unmount_tickle(void *context)
{
	struct da_request *request = context;
	char *buffer;
	int fd;

	if (request->blocksize <= 0 || request->blocksize > 65536)
		return (0);
	buffer = malloc((size_t)request->blocksize);
	if (buffer == NULL)
		return (0);
	fd = open(request->path, O_RDONLY);
	if (fd >= 0) {
		(void)read(fd, buffer, (size_t)request->blocksize);
		(void)close(fd);
	}
	free(buffer);
	return (0);
}

/* __DARequestUnmountTickleCallback. */
static void
unmount_tickle_done(int status __unused, void *context)
{
	struct da_request *request = context;
	struct da_disk *disk;

	free(request->path);
	request->path = NULL;
	disk = request_disk(request);
	if (disk == NULL) {
		unmount_approval_done(NULL, NULL, request);
		return;
	}
	DADiskUnmountApprovalCallback(disk, unmount_approval_done, request);
}

/* __DARequestUnmountUnmount, on a worker thread. */
static int
unmount_unmount(void *context)
{
	struct da_request *request = context;
	int flags = 0;

	if (request->options & kDADiskUnmountOptionForce)
		flags |= MNT_FORCE;
	if (unmount(request->path, flags) == 0)
		return (0);
	return (errno != 0 ? errno : EIO);
}

/* __DARequestUnmountGetProcessID, on a worker thread. */
static int
unmount_get_pid(void *context)
{
	struct da_request *request = context;

	request->pid = DADeviceFindProcessOnVolume(request->path);
	return (0);
}

static int
still_mounted(const char *path)
{
	struct statfs *list;
	int i, n;

	n = getmntinfo(&list, MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (strcmp(list[i].f_mntonname, path) == 0)
			return (1);
	return (0);
}

/* The end of an unmount, success or not. */
static void
unmount_complete(struct da_request *request, int status)
{
	struct da_disk *disk;

	disk = request_disk(request);
	if (status == 0) {
		DALog("DA-REQUEST: unmount %s: unmounted \"%s\"%s",
		    request->disk_id, request->path,
		    (request->options & kDADiskUnmountOptionForce) ?
		    " (forced)" : "");
		DAMountRemoveMountPoint(request->path);
		if (disk != NULL && disk->volume_only) {
			/* A mount and nothing else: the disk goes with it. */
			if (request->serialization != NULL)
				CFRelease(request->serialization);
			DADiskSetDescription(disk,
			    kDADiskDescriptionVolumePathKey, NULL);
			request->serialization = DADiskCopySerialization(disk);
			DALog("DA-VOLUME-GONE: %s", disk->id);
			DADiskListRemove(disk);
			disk = NULL;
		} else if (disk != NULL)
			/*
			 * StagedMount stays set: the volume is not mounted
			 * again behind the user's back (DAVolume.c).
			 */
			DADiskSetDescription(disk,
			    kDADiskDescriptionVolumePathKey, NULL);
		request_callback(request, NULL);
	} else if (request->dissenter == NULL)
		request_dispatch_callback(request, (int32_t)unix_err(status));
	else {
		if (request->pid != 0)
			DADissenterSetProcessID(request->dissenter,
			    request->pid);
		log_dissent(request, request->dissenter);
		request_callback(request, request->dissenter);
	}
	if (disk != NULL)
		disk->state &= ~kDADiskStateCommandActive;
	request_finish(request);
}

static void
unmount_get_pid_done(int status __unused, void *context)
{
	unmount_complete(context, EBUSY);
}

/* __DARequestUnmountCallback. */
static void
unmount_done(int status, void *context)
{
	struct da_request *request = context;
	CFDictionaryRef dissenter;

	/* Gone anyway (the filesystem went with something else): fine. */
	if (status != 0 && !still_mounted(request->path)) {
		DALogDebug("DA-REQUEST: unmount %s: \"%s\" is not mounted; "
		    "ignoring error %d", request->disk_id, request->path,
		    status);
		status = 0;
	}
	if (status != 0) {
		DALog("DA-REQUEST: unmount %s: unmount(\"%s\") failed: %s",
		    request->disk_id, request->path, strerror(status));
		dissenter = DADissenterCreate((int32_t)unix_err(status));
		request_set_dissenter(request, dissenter);
		if (dissenter != NULL)
			CFRelease(dissenter);
		if (request->dissenter != NULL && status == EBUSY) {
			DAThreadExecute(unmount_get_pid, request,
			    unmount_get_pid_done, request);
			return;
		}
	}
	unmount_complete(request, status);
}

/* __DARequestUnmount. */
static void
request_unmount(struct da_request *request, struct da_disk *disk)
{
	char path[MAXPATHLEN];
	int32_t status = 0;

	if ((request->state & kDARequestStateStagedApprove) == 0) {
		if (DADiskGetDescription(disk,
		    kDADiskDescriptionVolumeMountableKey) == kCFBooleanFalse)
			status = kDAReturnUnsupported;
		if (volume_path(disk, path, sizeof(path)) != 0)
			status = kDAReturnNotMounted;
		else if (strcmp(path, "/") == 0) {
			CFDictionaryRef dissenter;

			status = kDAReturnBusy;
			dissenter = DADissenterCreate(status);
			request_set_dissenter(request, dissenter);
			if (dissenter != NULL)
				CFRelease(dissenter);
		}
		if (status != 0) {
			if (request->parent == NULL ||
			    status != kDAReturnNotMounted)
				DALog("DA-REQUEST: unmount %s: %s",
				    request->disk_id,
				    status == kDAReturnBusy ? "the root "
				    "volume is busy" :
				    status == kDAReturnNotMounted ?
				    "not mounted" : "not mountable");
			request_dispatch_callback(request, status);
			request_finish(request);
			return;
		}
		request->state |= kDARequestStateStagedApprove;
		request->active = 1;
		disk->state |= kDADiskStateCommandActive;
		if (DADiskGetDescription(disk,
		    kDADiskDescriptionMediaWritableKey) == kCFBooleanTrue &&
		    !disk->volume_only) {
			CFTypeRef size = DADiskGetDescription(disk,
			    kDADiskDescriptionMediaBlockSizeKey);

			request->blocksize = size != NULL &&
			    CFGetTypeID(size) == CFNumberGetTypeID() ?
			    ___CFNumberGetIntegerValue(size) : 0;
			free(request->path);
			request->path = strdup(disk->id);
			if (request->path != NULL) {
				DAThreadExecute(unmount_tickle, request,
				    unmount_tickle_done, request);
				return;
			}
		}
		DADiskUnmountApprovalCallback(disk, unmount_approval_done,
		    request);
		return;
	}

	if (request->dissenter != NULL) {
		log_dissent(request, request->dissenter);
		request_callback(request, request->dissenter);
		request_finish(request);
		return;
	}

	/* Commence the unmount. */
	if (unit_busy(disk))
		return;
	if (volume_path(disk, path, sizeof(path)) != 0) {
		/* Unmounted meanwhile, by someone else. */
		request_dispatch_callback(request, kDAReturnNotMounted);
		request_finish(request);
		return;
	}
	free(request->path);
	request->path = strdup(path);
	if (request->path == NULL) {
		request_dispatch_callback(request, kDAReturnNoResources);
		request_finish(request);
		return;
	}
	if (request->serialization != NULL)
		CFRelease(request->serialization);
	request->serialization = DADiskCopySerialization(disk);
	request->active = 1;
	disk->state |= kDADiskStateCommandActive;
	DALog("DA-REQUEST: unmount %s: unmounting \"%s\"%s", disk->id, path,
	    (request->options & kDADiskUnmountOptionForce) ? " (force)" : "");
	DAThreadExecute(unmount_unmount, request, unmount_done, request);
}

/* --- eject ------------------------------------------------------------ */

/* __DARequestEjectApprovalCallback. */
static void
eject_approval_done(struct da_disk *disk, CFTypeRef dissenter, void *context)
{
	struct da_request *request = context;

	if (dissenter != NULL)
		request_set_dissenter(request, dissenter);
	if (disk != NULL)
		disk->state &= ~kDADiskStateCommandActive;
	request->active = 0;
	DAStageSignal();
}

/* __DARequestEjectEject, on a worker thread. */
static int
eject_eject(void *context)
{
	struct da_request *request = context;
	char how[256];
	int status;

	status = DADeviceEject(request->path, how, sizeof(how));
	/* Logged on the daemon queue. */
	(void)strlcpy(request->how, how, sizeof(request->how));
	return (status);
}

/* __DARequestEjectGetProcessID, on a worker thread. */
static int
eject_get_pid(void *context)
{
	struct da_request *request = context;

	request->pid = DADeviceFindProcessWithDevice(request->names,
	    request->nnames);
	return (0);
}

static void
eject_complete(struct da_request *request, int status)
{
	struct da_disk *disk;

	disk = request_disk(request);
	if (status == 0) {
		DALog("DA-REQUEST: eject %s: ejected (%s)", request->disk_id,
		    request->how);
		request_callback(request, NULL);
	} else if (request->dissenter == NULL)
		request_dispatch_callback(request, (int32_t)unix_err(status));
	else {
		if (request->pid != 0)
			DADissenterSetProcessID(request->dissenter,
			    request->pid);
		log_dissent(request, request->dissenter);
		request_callback(request, request->dissenter);
	}
	if (disk != NULL)
		disk->state &= ~kDADiskStateCommandActive;
	request_finish(request);
}

static void
eject_get_pid_done(int status __unused, void *context)
{
	/* Only a failed eject looks for the process. */
	eject_complete(context, EIO);
}

/* __DARequestEjectCallback. */
static void
eject_done(int status, void *context)
{
	struct da_request *request = context;
	CFDictionaryRef dissenter;

	if (status != 0) {
		DALog("DA-REQUEST: eject %s: failed: %s (%s)",
		    request->disk_id, strerror(status), request->how);
		dissenter = DADissenterCreate((int32_t)unix_err(status));
		request_set_dissenter(request, dissenter);
		if (dissenter != NULL)
			CFRelease(dissenter);
		if (request->dissenter != NULL) {
			DAThreadExecute(eject_get_pid, request,
			    eject_get_pid_done, request);
			return;
		}
	}
	eject_complete(request, status);
}

/* The device names of the whole disk and its partitions ("da0", "da0p1"). */
static int
eject_names(struct da_request *request, const struct da_disk *disk)
{
	const struct da_disk *sub;
	int n = 0;

	TAILQ_FOREACH(sub, &gDADiskList, link)
		if (!sub->volume_only && unit_of(sub) == disk)
			n++;
	request->names = calloc((size_t)n + 1, sizeof(*request->names));
	if (request->names == NULL)
		return (-1);
	request->nnames = 0;
	TAILQ_FOREACH(sub, &gDADiskList, link) {
		const char *name = sub->id;

		if (sub->volume_only || unit_of(sub) != disk ||
		    request->nnames >= n)
			continue;
		if (strncmp(name, "/dev/", 5) == 0)
			name += 5;
		request->names[request->nnames] = strdup(name);
		if (request->names[request->nnames] != NULL)
			request->nnames++;
	}
	return (0);
}

/* __DARequestEject. */
static void
request_eject(struct da_request *request, struct da_disk *disk)
{
	if ((request->state & kDARequestStateStagedApprove) == 0) {
		if (DADiskGetDescription(disk,
		    kDADiskDescriptionMediaWholeKey) != kCFBooleanTrue) {
			DALog("DA-REQUEST: eject %s: not a whole disk",
			    disk->id);
			request->linked = 0;	/* answer this status */
			request_dispatch_callback(request,
			    kDAReturnUnsupported);
			request_finish(request);
			return;
		}
		if (request->link_dissenter != NULL) {
			DALog("DA-REQUEST: eject %s: a volume of the disk did "
			    "not unmount; not ejecting", disk->id);
			request_callback(request, NULL);
			request_finish(request);
			return;
		}
		request->state |= kDARequestStateStagedApprove;
		request->active = 1;
		disk->state |= kDADiskStateCommandActive;
		DADiskEjectApprovalCallback(disk, eject_approval_done,
		    request);
		return;
	}

	if (request->dissenter != NULL) {
		log_dissent(request, request->dissenter);
		request_callback(request, request->dissenter);
		request_finish(request);
		return;
	}

	/* Commence the eject. */
	if (unit_busy(disk))
		return;
	free(request->path);
	request->path = strdup(disk->id);
	if (request->path == NULL || eject_names(request, disk) != 0) {
		request_dispatch_callback(request, kDAReturnNoResources);
		request_finish(request);
		return;
	}
	if (request->serialization != NULL)
		CFRelease(request->serialization);
	request->serialization = DADiskCopySerialization(disk);
	request->active = 1;
	disk->state |= kDADiskStateCommandActive;
	DALog("DA-REQUEST: eject %s: ejecting", disk->id);
	DAThreadExecute(eject_eject, request, eject_done, request);
}

/* --- the request list -------------------------------------------------- */

static struct da_request *
request_create(_DARequestKind kind, struct da_disk *disk, int32_t options,
    uid_t uid, gid_t gid, struct da_callback *callback)
{
	struct da_request *request;

	request = calloc(1, sizeof(*request));
	if (request == NULL || (request->disk_id = strdup(disk->id)) == NULL) {
		free(request);
		return (NULL);
	}
	request->kind = kind;
	request->disk_serial = disk->serial;
	request->options = options;
	request->uid = uid;
	request->gid = gid;
	request->callback = callback;
	request->serialization = DADiskCopySerialization(disk);
	return (request);
}

void
DAQueueRequest(_DARequestKind kind, struct da_disk *disk, int32_t options,
    uid_t uid, gid_t gid, struct da_callback *callback)
{
	struct da_request *request, *subrequest;
	struct da_disk *sub;
	int whole;

	request = request_create(kind, disk, options, uid, gid, callback);
	if (request == NULL) {
		CFDataRef serialization = DADiskCopySerialization(disk);
		CFDictionaryRef dissenter =
		    DADissenterCreate(kDAReturnNoResources);

		if (serialization != NULL)
			DAQueueRequestCallback(callback, serialization,
			    dissenter);
		if (serialization != NULL)
			CFRelease(serialization);
		if (dissenter != NULL)
			CFRelease(dissenter);
		DACallbackFree(callback);
		return;
	}

	/* DAQueueRequest's Whole expansion; an eject always expands. */
	whole = kind == _kDADiskEject ||
	    (options & kDADiskUnmountOptionWhole) != 0;
	if (whole) {
		if (DADiskGetDescription(disk,
		    kDADiskDescriptionMediaWholeKey) != kCFBooleanTrue) {
			DALog("DA-REQUEST: %s %s: not a whole disk",
			    request_name(request), disk->id);
			TAILQ_INSERT_TAIL(&gDARequestList, request, link);
			request_dispatch_callback(request,
			    kDAReturnUnsupported);
			request_finish(request);
			return;
		}
		request->linked = 1;
		TAILQ_FOREACH(sub, &gDADiskList, link) {
			if (sub->volume_only || unit_of(sub) != disk)
				continue;
			/* An eject unmounts the disk itself too. */
			if (sub == disk && kind != _kDADiskEject)
				continue;
			subrequest = request_create(_kDADiskUnmount, sub,
			    kind == _kDADiskEject ? 0 : options &
			    ~kDADiskUnmountOptionWhole, uid, gid, NULL);
			if (subrequest == NULL)
				continue;
			subrequest->parent = request;
			request->pending++;
			TAILQ_INSERT_TAIL(&gDARequestList, subrequest, link);
		}
	}
	TAILQ_INSERT_TAIL(&gDARequestList, request, link);
	DAStageSignal();
}

/* DARequestDispatch for one request. */
static void
request_dispatch(struct da_request *request)
{
	struct da_disk *disk;

	disk = request_disk(request);
	if (disk == NULL) {
		DALog("DA-REQUEST: %s %s: the disk went away",
		    request_name(request), request->disk_id);
		request_dispatch_callback(request, kDAReturnNotFound);
		request_finish(request);
		return;
	}
	if ((disk->state & kDADiskStateCommandActive) != 0 ||
	    (disk->state & kDADiskStateStagedAppear) == 0)
		return;
	if (request->kind == _kDADiskEject)
		request_eject(request, disk);
	else
		request_unmount(request, disk);
}

int
DARequestListDispatch(void)
{
	struct da_request *request, *next, *earlier;

	TAILQ_FOREACH_SAFE(request, &gDARequestList, link, next) {
		if (request->active || request->pending > 0)
			continue;
		/* Apple's dependency set: an earlier request on the disk. */
		TAILQ_FOREACH(earlier, &gDARequestList, link) {
			if (earlier == request ||
			    strcmp(earlier->disk_id, request->disk_id) == 0)
				break;
		}
		if (earlier != request)
			continue;
		request_dispatch(request);
	}
	return (!TAILQ_EMPTY(&gDARequestList));
}
