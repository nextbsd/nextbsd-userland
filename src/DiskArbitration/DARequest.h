/*
 * DARequest.h — requests: mount (nextbsd-userland#197), unmount and eject
 * (#198).
 *
 * The C counterpart of Apple's diskarbitrationd/DARequest.c
 * (DiskArbitration-535.120.1.0.1). Mount: the stage machine's automatic
 * mount and a client's DADiskMount both ask the sessions that registered a
 * mount approval callback first, then run the mount executor (DAMount.c).
 * Unmount and eject: a client's request joins the request list
 * (DAQueueRequest), which the stage machine dispatches (DARequestDispatch);
 * each asks the approval sessions, then unmounts or ejects on a worker
 * thread and answers the client's callback with the first dissenter.
 */
#ifndef DISKARBITRATION_DAREQUEST_H
#define DISKARBITRATION_DAREQUEST_H

#include <stdint.h>

#include "DADisk.h"
#include "DASession.h"

/* Apple's DiskArbitration.h option bits (the daemon does not include it). */
#define kDADiskUnmountOptionForce	0x00080000
#define kDADiskUnmountOptionWhole	0x00000001

/*
 * `status` is 0 on success, else a DAReturn: a dissenter's status, or
 * unix_err(errno) for a mount that failed. `mountpoint` is set on success.
 * Runs on the daemon queue, possibly before DARequestMount returns.
 */
typedef void (*DARequestMountCallback)(int32_t status, const char *mountpoint,
	    void *context);

/*
 * Mount `disk`: `mountpoint` NULL for /Volumes/<name>, `automatic` for the
 * stage machine's mount, `options` comma-separated extras (may be NULL).
 */
void	DARequestMount(struct da_disk *disk, const char *mountpoint,
	    int automatic, const char *options, DARequestMountCallback callback,
	    void *context);

/*
 * Queue a client's unmount or eject of `disk` (Apple's DARequestCreate +
 * DAQueueRequest): `kind` is _kDADiskUnmount or _kDADiskEject, `options`
 * the client's DADiskUnmountOptions (0 for eject), `callback` the
 * completion (taken over; freed when the request ends). An eject, and an
 * unmount with kDADiskUnmountOptionWhole, first unmount the other volumes
 * of the whole disk as linked subrequests. Answers through `callback`,
 * possibly before it returns.
 */
void	DAQueueRequest(_DARequestKind kind, struct da_disk *disk,
	    int32_t options, uid_t uid, gid_t gid, struct da_callback *callback);

/*
 * Dispatch what the request list can run now (Apple's request loop in
 * __DAStageDispatch). Returns nonzero while requests remain, so the daemon
 * is not idle. Called by the stage machine on the daemon queue.
 */
int	DARequestListDispatch(void);

#endif /* DISKARBITRATION_DAREQUEST_H */
