/*
 * DAGeom.h — build the DADisk model from GEOM (E15 C2, nextbsd-userland#192).
 *
 * A storage arrival or departure from the kernel registry (/dev/ioregistry,
 * da_iokit_subscribe.c) does not say which GEOM providers changed: the
 * registry reports the newbus storage driver (vtblk, ahcich, umass, ...),
 * and CAM probing plus GEOM tasting finish asynchronously after it. So every
 * event schedules a re-walk of the whole libgeom tree, diffed against
 * gDADiskList: new providers become disks, vanished ones are removed. The
 * walk repeats on a short settle schedule after the last event so a USB disk
 * whose CAM probe or partition taste lands a few seconds late is still seen.
 */
#ifndef DISKARBITRATION_DAGEOM_H
#define DISKARBITRATION_DAGEOM_H

#include <dispatch/dispatch.h>

/* Set the serial queue all disk-model work runs on. Call once, first. */
void	DAGeomInit(dispatch_queue_t queue);

/*
 * A storage device arrived or departed (or the daemon just started).
 * Thread-safe: coalesces bursts and schedules the walk on the DA queue.
 */
void	DAGeomStorageChanged(void);

#endif /* DISKARBITRATION_DAGEOM_H */
