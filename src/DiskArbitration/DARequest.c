/*
 * DARequest.c — the mount request (nextbsd-userland#197).
 *
 * Port of __DARequestMount and its approval step from Apple's
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
 */
#include "DARequest.h"
#include "DAInternal.h"
#include "DAMount.h"
#include "DAQueue.h"
#include "DASupport.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

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
