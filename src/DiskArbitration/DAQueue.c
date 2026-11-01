/*
 * DAQueue.c — callback delivery to client sessions and their responses
 * (nextbsd-userland#197).
 *
 * Port of the callback half of Apple's diskarbitrationd/DAQueue.c
 * (DiskArbitration-535.120.1.0.1): __DAQueueCallbacks, DAQueueCallback,
 * DAQueueCallbacks, the response round (__DAResponsePrepare,
 * __DAResponseComplete, _DAResponseDispatch) and its timer
 * (__DAResponseTimerRefresh: 10 s plus 1 s of grace, a session that misses
 * it is logged "not responding" and skipped for responses until it pulls
 * its queue again; a timeout is not a dissent). The request half of
 * Apple's file (DAQueueRequest, the kDADiskUnmountOptionWhole expansion)
 * is in DARequest.c (#198).
 *
 * Apple keeps the round on the disk (DADiskSetContextRe) and references
 * disks by CF retain; here the round is its own allocation and names its
 * disk by ID and serial, so a disk that vanishes mid-round is simply not
 * found when the round ends.
 */
#include "DAQueue.h"

#include <stdlib.h>
#include <string.h>

/* Apple's DAQueue.c __kDAResponseTimerLimit / __kDAResponseTimerGrace. */
#define DA_RESPONSE_TIMER_LIMIT	10
#define DA_RESPONSE_TIMER_GRACE	1

struct da_round {
	char			*disk_id;
	uint64_t		 disk_serial;
	int			 outstanding;
	int			 open;		/* still being queued */
	CFTypeRef		 dissenter;	/* the first one */
	DAResponseCallback	 callback;
	void			*context;
};

struct da_response {
	TAILQ_ENTRY(da_response) link;
	struct da_round		*round;
	struct da_session	*session;	/* retained */
	int32_t			 id;
	_DACallbackKind		 kind;
	CFAbsoluteTime		 deadline;
};

static TAILQ_HEAD(, da_response) gDAResponseList =
    TAILQ_HEAD_INITIALIZER(gDAResponseList);

static dispatch_queue_t	g_queue;
static struct da_round	*g_round;	/* between prepare and complete */
static int32_t		 g_response_id;
static int		 g_timer_armed;
static int		 g_changes_pending;

void
DAQueueInit(dispatch_queue_t queue)
{
	g_queue = queue;
}

CFDictionaryRef
DADissenterCreate(int32_t status)
{
	CFMutableDictionaryRef dissenter;

	dissenter = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (dissenter != NULL)
		___CFDictionarySetIntegerValue(dissenter, _kDADissenterStatusKey,
		    (SInt64)(uint32_t)status);
	return (dissenter);
}

void
DADissenterSetProcessID(CFDictionaryRef dissenter, pid_t pid)
{
	/* DADissenterCreate's dictionaries are mutable. */
	___CFDictionarySetIntegerValue((CFMutableDictionaryRef)dissenter,
	    _kDADissenterProcessIDKey, (SInt64)pid);
}

/* --- the response round --------------------------------------------------- */

static void
round_finish(struct da_round *round)
{
	struct da_disk *disk;

	disk = DADiskListFind(round->disk_id);
	if (disk != NULL && disk->serial != round->disk_serial)
		disk = NULL;
	round->callback(disk, round->dissenter, round->context);
	if (round->dissenter != NULL)
		CFRelease(round->dissenter);
	free(round->disk_id);
	free(round);
}

/* __DAResponsePrepare. */
static void
round_prepare(struct da_disk *disk, DAResponseCallback callback,
    void *context)
{
	struct da_round *round;

	round = calloc(1, sizeof(*round));
	if (round != NULL && (round->disk_id = strdup(disk->id)) == NULL) {
		free(round);
		round = NULL;
	}
	if (round == NULL) {
		/* Out of memory: answer at once, with no dissent. */
		DALog("DA-QUEUE: out of memory preparing a response round");
		g_round = NULL;
		callback(disk, NULL, context);
		return;
	}
	round->disk_serial = disk->serial;
	round->open = 1;
	round->callback = callback;
	round->context = context;
	g_round = round;
}

/* __DAResponseComplete. */
static void
round_complete(void)
{
	struct da_round *round = g_round;

	g_round = NULL;
	if (round == NULL)
		return;
	round->open = 0;
	if (round->outstanding == 0)
		round_finish(round);
}

static void response_timer(void *ctx);

/* __DAResponseTimerRefresh: one dispatch_after for the earliest deadline. */
static void
response_timer_refresh(void)
{
	struct da_response *response;
	CFAbsoluteTime first = 0, now;
	int64_t delay;

	if (g_timer_armed || g_queue == NULL)
		return;
	TAILQ_FOREACH(response, &gDAResponseList, link)
		if (first == 0 || response->deadline < first)
			first = response->deadline;
	if (first == 0)
		return;
	now = CFAbsoluteTimeGetCurrent();
	delay = first > now ? (int64_t)((first - now) * NSEC_PER_SEC) : 0;
	g_timer_armed = 1;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, delay), g_queue,
	    NULL, response_timer);
}

/* Drop one outstanding response; end its round if it was the last. */
static void
response_remove(struct da_response *response)
{
	struct da_round *round = response->round;

	TAILQ_REMOVE(&gDAResponseList, response, link);
	DASessionRelease(response->session);
	free(response);
	if (--round->outstanding == 0 && !round->open)
		round_finish(round);
}

/* __DAResponseTimerCallback. */
static void
response_timer(void *ctx __unused)
{
	struct da_response *response;
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

	g_timer_armed = 0;
restart:
	TAILQ_FOREACH(response, &gDAResponseList, link) {
		if (response->deadline > now)
			continue;
		DALog("DA-SESSION: " DA_SESSION_FMT " not responding "
		    "(%s callback)", DA_SESSION_ARGS(response->session),
		    _DACallbackKindGetName(response->kind));
		response->session->state |= kDASessionStateTimeout;
		response_remove(response);
		goto restart;
	}
	response_timer_refresh();
}

int
DAResponseDispatch(CFTypeRef response, int32_t responseID)
{
	struct da_response *entry;
	struct da_round *round;

	TAILQ_FOREACH(entry, &gDAResponseList, link)
		if (entry->id == responseID)
			break;
	if (entry == NULL)
		return (0);
	round = entry->round;
	switch (entry->kind) {
	case _kDADiskClaimReleaseCallback:
	case _kDADiskEjectApprovalCallback:
	case _kDADiskMountApprovalCallback:
	case _kDADiskUnmountApprovalCallback:
		if (response != NULL && CFGetTypeID(response) ==
		    CFDictionaryGetTypeID() && round->dissenter == NULL)
			round->dissenter = CFRetain(response);
		break;
	default:
		break;
	}
	response_remove(entry);
	return (1);
}

void
DAQueueReleaseSession(struct da_session *session)
{
	struct da_response *response;

restart:
	TAILQ_FOREACH(response, &gDAResponseList, link) {
		if (response->session == session) {
			response_remove(response);
			goto restart;
		}
	}
}

/* --- queueing ------------------------------------------------------------- */

/* address/context/kind, plus argument0 (the disk) and argument1. */
static CFMutableDictionaryRef
callback_dictionary(const struct da_callback *callback, CFDataRef argument0,
    CFTypeRef argument1)
{
	CFMutableDictionaryRef dict;

	dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (NULL);
	___CFDictionarySetIntegerValue(dict, _kDACallbackAddressKey,
	    (SInt64)callback->address);
	___CFDictionarySetIntegerValue(dict, _kDACallbackContextKey,
	    (SInt64)callback->context);
	___CFDictionarySetIntegerValue(dict, _kDACallbackKindKey,
	    (SInt64)callback->kind);
	if (argument0 != NULL)
		CFDictionarySetValue(dict, _kDACallbackArgument0Key, argument0);
	if (argument1 != NULL)
		CFDictionarySetValue(dict, _kDACallbackArgument1Key, argument1);
	return (dict);
}

static void
queue_one(struct da_callback *callback, struct da_disk *disk,
    CFTypeRef argument1)
{
	CFMutableDictionaryRef dict;
	CFDataRef serialization = NULL;

	if (disk != NULL) {
		serialization = DADiskCopySerialization(disk);
		if (serialization == NULL) {
			DALog("DA-QUEUE: cannot serialize %s", disk->id);
			return;
		}
	}
	dict = callback_dictionary(callback, serialization, argument1);
	if (serialization != NULL)
		CFRelease(serialization);
	if (dict == NULL)
		return;
	DASessionQueueCallback(callback->session, dict);
	CFRelease(dict);
	DALogDebug("DA-QUEUE: " DA_SESSION_FMT " <- %s%s%s",
	    DA_SESSION_ARGS(callback->session),
	    _DACallbackKindGetName(callback->kind), disk != NULL ? " " : "",
	    disk != NULL ? disk->id : "");
}

void
DAQueueRequestCallback(struct da_callback *callback, CFDataRef serialization,
    CFTypeRef dissenter)
{
	CFMutableDictionaryRef dict;

	if (callback->session->state & kDASessionStateZombie)
		return;
	dict = callback_dictionary(callback, serialization, dissenter);
	if (dict == NULL)
		return;
	DASessionQueueCallback(callback->session, dict);
	CFRelease(dict);
}

/* A peek or approval callback: queue it with a response ID to answer. */
static void
queue_with_response(struct da_callback *callback, struct da_disk *disk)
{
	struct da_response *response;
	CFNumberRef id;

	if (callback->session->state & kDASessionStateTimeout)
		return;
	if (g_round == NULL)
		return;
	response = calloc(1, sizeof(*response));
	if (response == NULL)
		return;
	id = ___CFNumberCreateWithIntegerValue(kCFAllocatorDefault,
	    g_response_id);
	if (id == NULL) {
		free(response);
		return;
	}
	response->round = g_round;
	response->session = callback->session;
	DASessionRetain(response->session);
	response->id = g_response_id++;
	response->kind = callback->kind;
	response->deadline = CFAbsoluteTimeGetCurrent() +
	    DA_RESPONSE_TIMER_LIMIT + DA_RESPONSE_TIMER_GRACE;
	TAILQ_INSERT_TAIL(&gDAResponseList, response, link);
	g_round->outstanding++;
	queue_one(callback, disk, id);
	CFRelease(id);
	response_timer_refresh();
}

/* The keys of `changed` that `watch` names (all of them without a watch). */
static CFArrayRef
watch_intersect(CFArrayRef changed, CFArrayRef watch)
{
	CFMutableArrayRef keys;
	CFIndex i, n;

	if (watch == NULL)
		return (CFRetain(changed));
	keys = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	if (keys == NULL)
		return (NULL);
	n = CFArrayGetCount(changed);
	for (i = 0; i < n; i++) {
		CFTypeRef key = CFArrayGetValueAtIndex(changed, i);

		if (CFArrayContainsValue(watch,
		    CFRangeMake(0, CFArrayGetCount(watch)), key))
			CFArrayAppendValue(keys, key);
	}
	return (keys);
}

void
DAQueueCallback(struct da_callback *callback, struct da_disk *disk,
    CFTypeRef argument1)
{
	CFArrayRef keys;

	if (callback->session->state & kDASessionStateZombie)
		return;
	if (callback->address == 0)
		return;
	if (callback->match != NULL && disk != NULL &&
	    !DADiskMatch(disk, callback->match))
		return;

	switch (callback->kind) {
	case _kDADiskAppearedCallback:
	case _kDADiskDisappearedCallback:
		queue_one(callback, disk, NULL);
		break;
	case _kDADiskClaimCallback:
	case _kDADiskEjectCallback:
	case _kDADiskMountCallback:
	case _kDADiskProbeCallback:
	case _kDADiskRenameCallback:
	case _kDADiskSetFSKitAdditionsCallback:
	case _kDADiskUnmountCallback:
		queue_one(callback, disk, argument1);
		break;
	case _kDADiskClaimReleaseCallback:
	case _kDADiskEjectApprovalCallback:
	case _kDADiskMountApprovalCallback:
	case _kDADiskPeekCallback:
	case _kDADiskUnmountApprovalCallback:
		queue_with_response(callback, disk);
		break;
	case _kDADiskDescriptionChangedCallback:
		if (disk == NULL || argument1 == NULL ||
		    (disk->state & kDADiskStateZombie))
			break;
		keys = watch_intersect(argument1, callback->watch);
		if (keys != NULL) {
			if (CFArrayGetCount(keys) > 0)
				queue_one(callback, disk, keys);
			CFRelease(keys);
		}
		break;
	case _kDAIdleCallback:
	case _kDADiskListCompleteCallback:
		queue_one(callback, NULL, NULL);
		break;
	default:
		break;
	}
}

void
DAQueueCallbacks(struct da_session *session, _DACallbackKind kind,
    struct da_disk *disk, CFTypeRef argument1)
{
	struct da_callback *callback, *next;

	TAILQ_FOREACH_SAFE(callback, &session->callbacks, link, next)
		if (callback->kind == kind)
			DAQueueCallback(callback, disk, argument1);
}

/* __DAQueueCallbacks: every session. */
static void
queue_all(_DACallbackKind kind, struct da_disk *disk, CFTypeRef argument1)
{
	struct da_session *session, *next;

	TAILQ_FOREACH_SAFE(session, &gDASessionList, link, next) {
		if (kind == _kDAIdleCallback &&
		    (session->state & kDASessionStateIdle))
			continue;
		DAQueueCallbacks(session, kind, disk, argument1);
		if (kind == _kDAIdleCallback)
			session->state |= kDASessionStateIdle;
	}
}

void
DADiskAppearedCallback(struct da_disk *disk)
{
	queue_all(_kDADiskAppearedCallback, disk, NULL);
}

void
DADiskDisappearedCallback(struct da_disk *disk)
{
	queue_all(_kDADiskDisappearedCallback, disk, NULL);
}

void
DADiskDescriptionChangedCallback(struct da_disk *disk, CFArrayRef keys)
{
	queue_all(_kDADiskDescriptionChangedCallback, disk, keys);
}

void
DADiskMountApprovalCallback(struct da_disk *disk, DAResponseCallback response,
    void *context)
{
	round_prepare(disk, response, context);
	if (g_round == NULL)
		return;
	queue_all(_kDADiskMountApprovalCallback, disk, NULL);
	round_complete();
}

void
DADiskUnmountApprovalCallback(struct da_disk *disk,
    DAResponseCallback response, void *context)
{
	round_prepare(disk, response, context);
	if (g_round == NULL)
		return;
	queue_all(_kDADiskUnmountApprovalCallback, disk, NULL);
	round_complete();
}

void
DADiskEjectApprovalCallback(struct da_disk *disk, DAResponseCallback response,
    void *context)
{
	round_prepare(disk, response, context);
	if (g_round == NULL)
		return;
	queue_all(_kDADiskEjectApprovalCallback, disk, NULL);
	round_complete();
}

void
DADiskPeekCallback(struct da_disk *disk, struct da_callback *callback,
    DAResponseCallback response, void *context)
{
	round_prepare(disk, response, context);
	if (g_round == NULL)
		return;
	DAQueueCallback(callback, disk, NULL);
	round_complete();
}

void
DAIdleCallback(void)
{
	queue_all(_kDAIdleCallback, NULL, NULL);
}

/* --- description changes -------------------------------------------------- */

static void
changes_flush(void *ctx __unused)
{
	struct da_disk *disk, *next;
	CFMutableArrayRef keys;

	g_changes_pending = 0;
	TAILQ_FOREACH_SAFE(disk, &gDADiskList, link, next) {
		if (disk->changed == NULL)
			continue;
		keys = disk->changed;
		disk->changed = NULL;
		if (CFArrayGetCount(keys) > 0)
			DADiskDescriptionChangedCallback(disk, keys);
		CFRelease(keys);
	}
}

void
DAQueueDescriptionChanged(struct da_disk *disk, CFStringRef key)
{
	if (disk->changed == NULL) {
		disk->changed = CFArrayCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeArrayCallBacks);
		if (disk->changed == NULL)
			return;
	}
	if (!CFArrayContainsValue(disk->changed,
	    CFRangeMake(0, CFArrayGetCount(disk->changed)), key))
		CFArrayAppendValue(disk->changed, key);
	if (!g_changes_pending && g_queue != NULL) {
		g_changes_pending = 1;
		dispatch_async_f(g_queue, NULL, changes_flush);
	}
}
