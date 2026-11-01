/*
 * DAStage.h — the disk stage machine (E15 C5, nextbsd-userland#195).
 *
 * Apple's DAStage.c: every disk walks Probe -> Peek -> Mount -> Appeared,
 * one stage per dispatch, driven by DAStageSignal(). The stage bits live in
 * the disk's state (DADisk.h); a disk with kDADiskStateCommandActive set is
 * waiting for a stage to finish and is skipped.
 */
#ifndef DISKARBITRATION_DASTAGE_H
#define DISKARBITRATION_DASTAGE_H

#include <dispatch/dispatch.h>

/* Set the daemon queue the stages run on. Call once, first. */
void	DAStageInit(dispatch_queue_t queue);

/*
 * Something changed (a disk appeared, a stage finished): run the stage
 * machine on the daemon queue. Bursts coalesce into one dispatch. Call it
 * on the daemon queue.
 */
void	DAStageSignal(void);

#endif /* DISKARBITRATION_DASTAGE_H */
