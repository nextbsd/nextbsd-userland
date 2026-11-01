/*
 * DAProbe.h — the probe stage (E15 C4, nextbsd-userland#194).
 *
 * Apple's DAProbe (DAProbe.c, DAFileSystem.c) runs each matching bundle's
 * helper with -p until one recognises the volume, then asks it for the
 * volume UUID (-k) and asks the repair executable whether the volume is
 * clean (-q). The result becomes DAVolumeKind, DAVolumeName, DAVolumeUUID,
 * DAVolumeMountable and kDADiskStateRequireRepair. Nothing is mounted.
 */
#ifndef DISKARBITRATION_DAPROBE_H
#define DISKARBITRATION_DAPROBE_H

#include <dispatch/dispatch.h>

#include "DADisk.h"

/* Set the daemon queue probe results are applied on. Call once, first. */
void	DAProbeInit(dispatch_queue_t queue);

/*
 * Probe `disk` (on the daemon queue). The helpers run on a background
 * queue; the result is applied back on the daemon queue if the disk is
 * still the same one.
 */
void	DAProbeDisk(struct da_disk *disk);

#endif /* DISKARBITRATION_DAPROBE_H */
