/*
 * DAQueue.h — callback delivery to client sessions and their responses
 * (nextbsd-userland#197).
 *
 * The C counterpart of Apple's diskarbitrationd/DAQueue.c
 * (DiskArbitration-535.120.1.0.1): each event is offered to every session's
 * registered callbacks of that kind (filtered by the callback's match
 * dictionary and, for description changes, its watch keys) and queued on
 * the session with the disk's serialized description. Peek and approval
 * callbacks also expect a response; the caller is told once every session
 * asked has answered or timed out (10 s, Apple's __kDAResponseTimerLimit,
 * plus one second of grace). The first dissenter wins.
 *
 * All of it runs on the daemon queue.
 */
#ifndef DISKARBITRATION_DAQUEUE_H
#define DISKARBITRATION_DAQUEUE_H

#include <dispatch/dispatch.h>

#include "DADisk.h"
#include "DASession.h"

/* Set the daemon queue (the response timer runs there). Call once. */
void	DAQueueInit(dispatch_queue_t queue);

/*
 * The answer to a peek or approval round: NULL, or the first dissenter (a
 * CFDictionary, Apple's DADissenter) some session returned. `disk` is the
 * disk the round was for, or NULL if it went away meanwhile.
 */
typedef void (*DAResponseCallback)(struct da_disk *disk, CFTypeRef dissenter,
	    void *context);

void	DADiskAppearedCallback(struct da_disk *disk);
void	DADiskDisappearedCallback(struct da_disk *disk);
void	DADiskDescriptionChangedCallback(struct da_disk *disk, CFArrayRef keys);
void	DADiskMountApprovalCallback(struct da_disk *disk,
	    DAResponseCallback response, void *context);
void	DADiskUnmountApprovalCallback(struct da_disk *disk,
	    DAResponseCallback response, void *context);
void	DADiskEjectApprovalCallback(struct da_disk *disk,
	    DAResponseCallback response, void *context);
void	DADiskPeekCallback(struct da_disk *disk, struct da_callback *callback,
	    DAResponseCallback response, void *context);
void	DAIdleCallback(void);

/*
 * Queue `callback` (one registered callback, or a request's completion)
 * for `disk` with `argument1` (a dissenter, a key array, or NULL), applying
 * the callback's match and watch (Apple's DAQueueCallback).
 */
void	DAQueueCallback(struct da_callback *callback, struct da_disk *disk,
	    CFTypeRef argument1);
/*
 * Queue a request's completion: `callback` with the disk as serialized
 * (DADiskCopySerialization) and its dissenter, or NULL on success. The
 * serialization may predate the disk's disappearance.
 */
void	DAQueueRequestCallback(struct da_callback *callback,
	    CFDataRef serialization, CFTypeRef dissenter);
/* Every registered callback of `kind` in `session` (DAQueueCallbacks). */
void	DAQueueCallbacks(struct da_session *session, _DACallbackKind kind,
	    struct da_disk *disk, CFTypeRef argument1);

/* A client answered response `responseID` (Apple's _DAResponseDispatch). */
int	DAResponseDispatch(CFTypeRef response, int32_t responseID);

/*
 * The session is going away: its outstanding responses count as answered
 * with no dissent (Apple's DAQueueReleaseSession).
 */
void	DAQueueReleaseSession(struct da_session *session);

/*
 * A description key of an appeared disk changed; the changes of one daemon
 * queue turn go out as one DescriptionChanged per disk. DADisk.c calls it.
 */
void	DAQueueDescriptionChanged(struct da_disk *disk, CFStringRef key);

/* A dissenter dictionary for `status` (Apple's daemon DADissenterCreate). */
CFDictionaryRef	DADissenterCreate(int32_t status);
/* Name the process that dissented (a dissenter DADissenterCreate made). */
void		DADissenterSetProcessID(CFDictionaryRef dissenter, pid_t pid);

#endif /* DISKARBITRATION_DAQUEUE_H */
