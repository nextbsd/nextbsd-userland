/*
 * DAVolume.h — adopt mounts the daemon did not make (E15 C3,
 * nextbsd-userland#193).
 *
 * Apple's daemon turns every mount it learns about into a DADisk, so
 * clients see NFS, tmpfs, nullfs and hand-made mounts as well as its own
 * (DAServer.c _DAVolumeMountedCallback / _DAVolumeUnmountedCallback,
 * DADisk.c DADiskCreateFromVolumePath). The trigger is the embedded build's
 * EVFILT_FS source; here it is a kqueue registered for EVFILT_FS, read by a
 * dedicated thread that hands events to the daemon queue.
 */
#ifndef DISKARBITRATION_DAVOLUME_H
#define DISKARBITRATION_DAVOLUME_H

#include <dispatch/dispatch.h>

/*
 * Arm the EVFILT_FS watch on `queue`. Events are ignored until the first
 * DAVolumeRefresh(), which the first GEOM walk makes, so mounts of GEOM
 * media attach to their disk instead of a stand-in.
 */
void	DAVolumeInit(dispatch_queue_t queue);

/*
 * Diff getmntinfo(3) against the disk list (on the daemon queue): new mounts
 * of known media set DAVolumePath on that disk, other new mounts become
 * disks of their own, and vanished mounts are dropped.
 */
void	DAVolumeRefresh(void);

#endif /* DISKARBITRATION_DAVOLUME_H */
