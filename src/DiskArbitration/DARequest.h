/*
 * DARequest.h — requests: the mount request (nextbsd-userland#197).
 *
 * The C counterpart of the mount half of Apple's diskarbitrationd/
 * DARequest.c (DiskArbitration-535.120.1.0.1, __DARequestMount): the stage
 * machine's automatic mount and a client's DADiskMount both ask the
 * sessions that registered a mount approval callback first, then run the
 * mount executor (DAMount.c). Unmount, eject and their approvals are #198.
 */
#ifndef DISKARBITRATION_DAREQUEST_H
#define DISKARBITRATION_DAREQUEST_H

#include <stdint.h>

#include "DADisk.h"

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

#endif /* DISKARBITRATION_DAREQUEST_H */
