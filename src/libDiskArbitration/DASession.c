/*
 * DASession.c — libDiskArbitration's session object (nextbsd-userland#197).
 *
 * Port of Apple's DiskArbitration/DASession.c (DiskArbitration-535.120.1.0.1).
 * A session is a send right to the daemon's per-session port, obtained with
 * _DAServerSessionCreate on the com.apple.DiskArbitration.diskarbitrationd
 * service, plus a client port the daemon rings (an empty message) when the
 * session's callback queue goes from empty to non-empty; the session then
 * pulls the whole queue with one _DAServerSessionCopyCallbackQueue and runs
 * the callbacks.
 *
 * What differs from Apple, and why:
 *   - Apple's run-loop mode wraps the client port in a CFMachPort. NextBSD's
 *     CoreFoundation does not build CFMachPort, so the port is watched by a
 *     DISPATCH_SOURCE_TYPE_MACH_RECV source on a private queue, which signals
 *     a version 0 CFRunLoopSource on the scheduled run loops; the callbacks
 *     still run on the run loop's thread. libSystemConfiguration's
 *     SCDynamicStore run-loop source works the same way.
 *   - The session name sent to the daemon is getprogname(3), not
 *     _dyld_get_image_name(0) (there is no dyld).
 *   - No Authorization Services: there is nothing to create, and the
 *     SystemUIServer special case goes with it.
 *   - os_log goes to syslog(3); OSAtomic counters are gone.
 *   - Callback table indexes are never reused, so a stale index from the
 *     daemon cannot name a newer callback.
 */
#include "DAClient.h"
#include "DAServerUser.h"	/* MIG: _DAServer* user stubs */

#include <dispatch/dispatch.h>

#include <CoreFoundation/CFRuntime.h>

#include <mach/notify.h>
#include <servers/bootstrap.h>
#include <bootstrap_priv.h>

#include <notify.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

struct __DASession {
	CFRuntimeBase		 _base;
	char			*_name;
	pid_t			 _pid;
	mach_port_t		 _server;	/* send right: our daemon session */
	mach_port_t		 _client;	/* receive right: the doorbell */
	dispatch_source_t	 _source2;	/* MACH_RECV on _client */
	dispatch_queue_t	 _sourceQueue;	/* run-loop mode's private queue */
	CFRunLoopSourceRef	 _source;	/* run-loop mode */
	CFMutableArrayRef	 _runLoops;	/* where _source is scheduled */
	UInt32			 _sourceCount;
	CFMutableDictionaryRef	 _register;	/* index -> callback */
	SInt32			 _registerIndex;
	pthread_mutex_t		 _registerLock;
	bool			 _keepAlive;
	int			 _token;
	dispatch_queue_t	 _queue;	/* keep-alive / dispatch mode */
};

typedef struct __DASession __DASession;

static CFTypeID __kDASessionTypeID = _kCFRuntimeNotATypeID;

static CFStringRef
__DASessionCopyDescription(CFTypeRef object)
{
	DASessionRef session = (DASessionRef)object;

	return (CFStringCreateWithFormat(CFGetAllocator(object), NULL,
	    CFSTR("<DASession %p [%p]>{id = %s [%d]:%u}"), object,
	    CFGetAllocator(object), session->_name != NULL ? session->_name :
	    "?", session->_pid, (unsigned)session->_server));
}

static CFStringRef
__DASessionCopyFormattingDescription(CFTypeRef object,
    CFDictionaryRef options __unused)
{
	DASessionRef session = (DASessionRef)object;

	return (CFStringCreateWithFormat(CFGetAllocator(object), NULL,
	    CFSTR("%s [%d]:%u"), session->_name != NULL ? session->_name : "?",
	    session->_pid, (unsigned)session->_server));
}

static void
__DASessionDeallocate(CFTypeRef object)
{
	DASessionRef session = (DASessionRef)object;

	if (session->_token != -1)
		(void)notify_cancel(session->_token);
	if (session->_source2 != NULL) {
		/* The cancel handler drops the client port. */
		dispatch_source_cancel(session->_source2);
		dispatch_release(session->_source2);
	}
	if (session->_source != NULL) {
		CFRunLoopSourceInvalidate(session->_source);
		CFRelease(session->_source);
	}
	if (session->_runLoops != NULL)
		CFRelease(session->_runLoops);
	if (session->_sourceQueue != NULL)
		dispatch_release(session->_sourceQueue);
	if (session->_queue != NULL)
		dispatch_release(session->_queue);
	free(session->_name);
	if (session->_server != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), session->_server);
	if (session->_register != NULL)
		CFRelease(session->_register);
	pthread_mutex_destroy(&session->_registerLock);
}

static Boolean
__DASessionEqual(CFTypeRef object1, CFTypeRef object2)
{
	return (((DASessionRef)object1)->_server ==
	    ((DASessionRef)object2)->_server);
}

static CFHashCode
__DASessionHash(CFTypeRef object)
{
	return ((CFHashCode)((DASessionRef)object)->_server);
}

static const CFRuntimeClass __DASessionClass = {
	.version		= 0,
	.className		= "DASession",
	.finalize		= __DASessionDeallocate,
	.equal			= __DASessionEqual,
	.hash			= __DASessionHash,
	.copyFormattingDesc	= __DASessionCopyFormattingDescription,
	.copyDebugDesc		= __DASessionCopyDescription,
};

void
_DASessionInitialize(void)
{
	__kDASessionTypeID = _CFRuntimeRegisterClass(&__DASessionClass);
}

static DASessionRef
__DASessionCreate(CFAllocatorRef allocator)
{
	__DASession *session;

	session = (void *)_CFRuntimeCreateInstance(allocator,
	    __kDASessionTypeID, sizeof(__DASession) - sizeof(CFRuntimeBase),
	    NULL);
	if (session == NULL)
		return (NULL);
	session->_name = NULL;
	session->_pid = 0;
	session->_server = MACH_PORT_NULL;
	session->_client = MACH_PORT_NULL;
	session->_source2 = NULL;
	session->_sourceQueue = NULL;
	session->_source = NULL;
	session->_runLoops = NULL;
	session->_sourceCount = 0;
	session->_register = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	session->_registerIndex = 0;
	session->_keepAlive = false;
	session->_token = -1;
	session->_queue = NULL;
	pthread_mutex_init(&session->_registerLock, NULL);
	if (session->_register == NULL) {
		CFRelease(session);
		return (NULL);
	}
	return (session);
}

mach_port_t
_DASessionGetID(DASessionRef session)
{
	return (session->_server);
}

bool
_DASessionIsKeepAlive(DASessionRef session)
{
	return (session->_keepAlive);
}

/* --- callback delivery ---------------------------------------------------- */

/* Pull the whole queue and run it (Apple's _DASessionCallback). */
static void
__DASessionPull(DASessionRef session)
{
	vm_address_t _queue = 0;
	mach_msg_type_number_t _queueSize = 0;
	CFArrayRef queue;
	CFIndex count, index;

	if (session->_server == MACH_PORT_NULL)
		return;
	if (_DAServerSessionCopyCallbackQueue(session->_server, &_queue,
	    &_queueSize) != KERN_SUCCESS)
		return;
	queue = _DAUnserializeWithBytes(CFGetAllocator(session), _queue,
	    _queueSize);
	if (queue != NULL && CFGetTypeID(queue) == CFArrayGetTypeID()) {
		count = CFArrayGetCount(queue);
		for (index = 0; index < count; index++) {
			CFDictionaryRef callback;

			callback = CFArrayGetValueAtIndex(queue, index);
			if (callback == NULL ||
			    CFGetTypeID(callback) != CFDictionaryGetTypeID())
				continue;
			_DADispatchCallback(session,
			    (void *)(uintptr_t)___CFDictionaryGetIntegerValue(
			    callback, _kDACallbackAddressKey),
			    (void *)(uintptr_t)___CFDictionaryGetIntegerValue(
			    callback, _kDACallbackContextKey),
			    (_DACallbackKind)___CFDictionaryGetIntegerValue(
			    callback, _kDACallbackKindKey),
			    CFDictionaryGetValue(callback,
			    _kDACallbackArgument0Key),
			    CFDictionaryGetValue(callback,
			    _kDACallbackArgument1Key));
		}
	}
	if (queue != NULL)
		CFRelease(queue);
	if (_queue != 0)
		(void)vm_deallocate(mach_task_self(), _queue, _queueSize);
}

/* The daemon went away: drop the session's right to it. */
static void
__DASessionLost(DASessionRef session)
{
	syslog(LOG_NOTICE, "DiskArbitration: diskarbitrationd exited "
	    "(session %s [%d])", session->_name != NULL ? session->_name : "?",
	    (int)session->_pid);
	free(session->_name);
	session->_name = NULL;
	if (session->_server != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), session->_server);
	session->_server = MACH_PORT_NULL;
}

/*
 * Drain the client port: doorbells (msgh_id 0, at most one queued) and the
 * no-senders notification the kernel posts when the daemon's send right
 * dies with it. Returns 1 if a doorbell was seen.
 */
static int
__DASessionDrain(DASessionRef session, int *lost)
{
	union {
		mach_msg_header_t	header;
		char			buf[256];
	} message;
	int rang = 0;

	*lost = 0;
	for (;;) {
		memset(&message, 0, sizeof(message.header));
		if (mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(message), session->_client, 0, MACH_PORT_NULL) !=
		    MACH_MSG_SUCCESS)
			break;
		if (message.header.msgh_id == MACH_NOTIFY_NO_SENDERS)
			*lost = 1;
		else
			rang = 1;
		mach_msg_destroy(&message.header);
	}
	return (rang);
}

/* Dispatch mode: runs on the caller's queue. */
static void
__DASessionSourceEvent(void *context)
{
	DASessionRef session = context;
	int lost;

	if (__DASessionDrain(session, &lost) && !lost)
		__DASessionPull(session);
	if (lost)
		__DASessionLost(session);
}

/* Run-loop mode: runs on the private queue, the work on the run loop. */
static void
__DASessionSourceSignal(void *context)
{
	DASessionRef session = context;
	CFIndex i, n;
	int lost;

	(void)__DASessionDrain(session, &lost);
	if (lost) {
		__DASessionLost(session);
		return;
	}
	if (session->_source == NULL)
		return;
	CFRunLoopSourceSignal(session->_source);
	n = session->_runLoops != NULL ? CFArrayGetCount(session->_runLoops) : 0;
	for (i = 0; i < n; i++)
		CFRunLoopWakeUp((CFRunLoopRef)CFArrayGetValueAtIndex(
		    session->_runLoops, i));
}

static void
__DASessionRunLoopPerform(void *info)
{
	__DASessionPull((DASessionRef)info);
}

struct __DASessionPort {
	DASessionRef	session;
	mach_port_t	port;
};

static void
__DASessionSourceCancel(void *context)
{
	struct __DASessionPort *p = context;

	(void)mach_port_mod_refs(mach_task_self(), p->port,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	CFRelease(p->session);
	free(p);
}

/*
 * A new client port (queue limit 1, no-senders armed) and its MACH_RECV
 * source on `queue`, then hand the daemon a send right to it. Apple's
 * DASessionSetDispatchQueue body, shared by both modes.
 */
static int
__DASessionStartClient(DASessionRef session, dispatch_queue_t queue,
    dispatch_function_t handler)
{
	mach_port_limits_t limits;
	mach_port_t client = MACH_PORT_NULL, prev = MACH_PORT_NULL;
	dispatch_source_t source;
	struct __DASessionPort *p;

	if (session->_server == MACH_PORT_NULL)
		return (-1);
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &client) != KERN_SUCCESS)
		return (-1);
	memset(&limits, 0, sizeof(limits));
	limits.mpl_qlimit = 1;
	(void)mach_port_set_attributes(mach_task_self(), client,
	    MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits,
	    MACH_PORT_LIMITS_INFO_COUNT);
	/*
	 * The daemon will hold the only send right; when it exits the kernel
	 * posts no-senders here.
	 */
	if (mach_port_request_notification(mach_task_self(), client,
	    MACH_NOTIFY_NO_SENDERS, 1, client, MACH_MSG_TYPE_MAKE_SEND_ONCE,
	    &prev) != KERN_SUCCESS)
		goto fail;
	if (prev != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), prev);
	p = malloc(sizeof(*p));
	if (p == NULL)
		goto fail;
	source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,
	    client, 0, queue);
	if (source == NULL) {
		free(p);
		goto fail;
	}
	/* Both handlers get `p`; it holds a reference on the session. */
	p->session = (DASessionRef)CFRetain(session);
	p->port = client;
	dispatch_set_context(source, p);
	dispatch_source_set_event_handler_f(source, handler);
	dispatch_source_set_cancel_handler_f(source, __DASessionSourceCancel);
	session->_client = client;
	session->_source2 = source;
	dispatch_resume(source);
	(void)_DAServerSessionSetClientPort(session->_server, client);
	return (0);
fail:
	(void)mach_port_mod_refs(mach_task_self(), client,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	return (-1);
}

static void
__DASessionStopClient(DASessionRef session)
{
	if (session->_source2 != NULL) {
		dispatch_source_cancel(session->_source2);
		dispatch_release(session->_source2);
		session->_source2 = NULL;
	}
	session->_client = MACH_PORT_NULL;
}

/* Event handlers receive the source context (struct __DASessionPort). */
static void
__DASessionDispatchEvent(void *context)
{
	__DASessionSourceEvent(((struct __DASessionPort *)context)->session);
}

static void
__DASessionRunLoopEvent(void *context)
{
	__DASessionSourceSignal(((struct __DASessionPort *)context)->session);
}

/* --- creation ------------------------------------------------------------- */

static bool
DASessionEstablish(DASessionRef session)
{
	mach_port_t bootstrapPort = MACH_PORT_NULL, masterPort, server;
	const char *name;
	kern_return_t status;

	status = task_get_bootstrap_port(mach_task_self(), &bootstrapPort);
	if (status != KERN_SUCCESS || bootstrapPort == MACH_PORT_NULL)
		bootstrapPort = bootstrap_port;
	status = bootstrap_look_up2(bootstrapPort, _kDADaemonName, &masterPort,
	    0, BOOTSTRAP_PRIVILEGED_SERVER);
	if (status != KERN_SUCCESS)
		return (false);
	name = getprogname();
	if (name == NULL)
		name = "?";
	status = _DAServerSessionCreate(masterPort, (caddr_t)name, &server);
	(void)mach_port_deallocate(mach_task_self(), masterPort);
	if (status != KERN_SUCCESS)
		return (false);
	free(session->_name);
	session->_name = strdup(name);
	session->_pid = getpid();
	session->_server = server;
	return (true);
}

DASessionRef
DASessionCreate(CFAllocatorRef allocator)
{
	DASessionRef session;

	_DAInitialize();
	session = __DASessionCreate(allocator);
	if (session == NULL)
		return (NULL);
	if (!DASessionEstablish(session)) {
		CFRelease(session);
		return (NULL);
	}
	return (session);
}

CFTypeID
DASessionGetTypeID(void)
{
	_DAInitialize();
	return (__kDASessionTypeID);
}

/* --- scheduling ----------------------------------------------------------- */

void
DASessionScheduleWithRunLoop(DASessionRef session, CFRunLoopRef runLoop,
    CFStringRef runLoopMode)
{
	CFRunLoopSourceContext context;

	if (session == NULL)
		return;
	session->_sourceCount++;
	if (session->_sourceCount == 1) {
		memset(&context, 0, sizeof(context));
		context.version = 0;
		context.info = session;
		context.perform = __DASessionRunLoopPerform;
		session->_source = CFRunLoopSourceCreate(
		    CFGetAllocator(session), 0, &context);
		session->_runLoops = CFArrayCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeArrayCallBacks);
		session->_sourceQueue = dispatch_queue_create(
		    "org.nextbsd.DiskArbitration.session", NULL);
		if (session->_source == NULL || session->_runLoops == NULL ||
		    session->_sourceQueue == NULL ||
		    __DASessionStartClient(session, session->_sourceQueue,
		    __DASessionRunLoopEvent) != 0) {
			syslog(LOG_ERR, "DiskArbitration: cannot schedule "
			    "session %s on a run loop", session->_name != NULL ?
			    session->_name : "?");
		}
	}
	if (session->_source != NULL && session->_runLoops != NULL) {
		CFRunLoopAddSource(runLoop, session->_source, runLoopMode);
		CFArrayAppendValue(session->_runLoops, runLoop);
		/* Anything queued before we were scheduled. */
		CFRunLoopSourceSignal(session->_source);
		CFRunLoopWakeUp(runLoop);
	}
}

void
DASessionUnscheduleFromRunLoop(DASessionRef session, CFRunLoopRef runLoop,
    CFStringRef runLoopMode)
{
	CFIndex index;

	if (session == NULL)
		return;
	if (session->_source != NULL) {
		CFRunLoopRemoveSource(runLoop, session->_source, runLoopMode);
		if (session->_runLoops != NULL) {
			index = CFArrayGetFirstIndexOfValue(session->_runLoops,
			    CFRangeMake(0, CFArrayGetCount(session->_runLoops)),
			    runLoop);
			if (index != kCFNotFound)
				CFArrayRemoveValueAtIndex(session->_runLoops,
				    index);
		}
	}
	if (session->_sourceCount == 1) {
		__DASessionStopClient(session);
		if (session->_source != NULL) {
			CFRunLoopSourceInvalidate(session->_source);
			CFRelease(session->_source);
			session->_source = NULL;
		}
		if (session->_runLoops != NULL) {
			CFRelease(session->_runLoops);
			session->_runLoops = NULL;
		}
		if (session->_sourceQueue != NULL) {
			dispatch_release(session->_sourceQueue);
			session->_sourceQueue = NULL;
		}
	}
	if (session->_sourceCount > 0)
		session->_sourceCount--;
}

void
DASessionSetDispatchQueue(DASessionRef session, dispatch_queue_t queue)
{
	if (session == NULL)
		return;
	__DASessionStopClient(session);
	if (session->_queue != NULL && session->_queue != queue) {
		dispatch_release(session->_queue);
		session->_queue = NULL;
	}
	if (queue == NULL)
		return;
	if (session->_queue != queue) {
		dispatch_retain(queue);
		session->_queue = queue;
	}
	if (__DASessionStartClient(session, queue, __DASessionDispatchEvent) !=
	    0)
		syslog(LOG_ERR, "DiskArbitration: cannot schedule session %s "
		    "on a dispatch queue", session->_name != NULL ?
		    session->_name : "?");
}

/* Apple's approval sessions are ordinary sessions. */
DAApprovalSessionRef
DAApprovalSessionCreate(CFAllocatorRef allocator)
{
	return (DASessionCreate(allocator));
}

CFTypeID
DAApprovalSessionGetTypeID(void)
{
	return (DASessionGetTypeID());
}

void
DAApprovalSessionScheduleWithRunLoop(DAApprovalSessionRef session,
    CFRunLoopRef runLoop, CFStringRef runLoopMode)
{
	DASessionScheduleWithRunLoop(session, runLoop, runLoopMode);
}

void
DAApprovalSessionUnscheduleFromRunLoop(DAApprovalSessionRef session,
    CFRunLoopRef runLoop, CFStringRef runLoopMode)
{
	DASessionUnscheduleFromRunLoop(session, runLoop, runLoopMode);
}

/* --- the callback table --------------------------------------------------- */

CFMutableDictionaryRef
DACallbackCreate(CFAllocatorRef allocator, mach_vm_offset_t address,
    mach_vm_offset_t context, _DACallbackKind kind, CFIndex order,
    CFDataRef match, CFDataRef watch)
{
	CFMutableDictionaryRef callback;

	callback = CFDictionaryCreateMutable(allocator, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (callback == NULL)
		return (NULL);
	___CFDictionarySetIntegerValue(callback, _kDACallbackAddressKey,
	    (SInt64)address);
	___CFDictionarySetIntegerValue(callback, _kDACallbackContextKey,
	    (SInt64)context);
	___CFDictionarySetIntegerValue(callback, _kDACallbackKindKey,
	    (SInt64)kind);
	___CFDictionarySetIntegerValue(callback, _kDACallbackOrderKey,
	    (SInt64)order);
	if (match != NULL)
		CFDictionarySetValue(callback, _kDACallbackMatchKey, match);
	if (watch != NULL)
		CFDictionarySetValue(callback, _kDACallbackWatchKey, watch);
	return (callback);
}

static CFNumberRef
index_number(SInt32 index)
{
	return (CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
	    &index));
}

SInt32
DAAddCallbackToSession(DASessionRef session, CFMutableDictionaryRef callback)
{
	CFNumberRef key;
	SInt32 index;

	if (session == NULL || callback == NULL)
		return (0);
	pthread_mutex_lock(&session->_registerLock);
	/* 0 means "no callback" to the daemon; never hand it out. */
	do {
		index = ++session->_registerIndex;
		if (index <= 0) {
			session->_registerIndex = 0;
			index = ++session->_registerIndex;
		}
		key = index_number(index);
		if (key != NULL && !CFDictionaryContainsKey(session->_register,
		    key))
			break;
		if (key != NULL)
			CFRelease(key);
		key = NULL;
	} while (1);
	CFDictionarySetValue(session->_register, key, callback);
	CFRelease(key);
	pthread_mutex_unlock(&session->_registerLock);
	return (index);
}

void
DARemoveCallbackFromSessionWithKey(DASessionRef session, SInt32 index)
{
	CFNumberRef key;

	if (session == NULL)
		return;
	key = index_number(index);
	if (key == NULL)
		return;
	pthread_mutex_lock(&session->_registerLock);
	CFDictionaryRemoveValue(session->_register, key);
	pthread_mutex_unlock(&session->_registerLock);
	CFRelease(key);
}

struct remove_match {
	mach_vm_offset_t	address;
	mach_vm_offset_t	context;
	SInt32			index;
};

static void
remove_find(const void *key, const void *value, void *context)
{
	struct remove_match *m = context;
	SInt32 index = 0;

	if (m->index != 0)
		return;
	if ((mach_vm_offset_t)___CFDictionaryGetIntegerValue(value,
	    _kDACallbackAddressKey) == m->address &&
	    (mach_vm_offset_t)___CFDictionaryGetIntegerValue(value,
	    _kDACallbackContextKey) == m->context &&
	    ___CFDictionaryGetIntegerValue(value, _kDACallbackKindKey) !=
	    UINT32_MAX &&
	    CFNumberGetValue(key, kCFNumberSInt32Type, &index))
		m->index = index;
}

SInt32
DARemoveCallbackFromSession(DASessionRef session, mach_vm_offset_t address,
    mach_vm_offset_t context)
{
	struct remove_match m = { address, context, 0 };

	if (session == NULL)
		return (0);
	pthread_mutex_lock(&session->_registerLock);
	CFDictionaryApplyFunction(session->_register, remove_find, &m);
	pthread_mutex_unlock(&session->_registerLock);
	if (m.index != 0)
		DARemoveCallbackFromSessionWithKey(session, m.index);
	return (m.index);
}

CFDictionaryRef
DACopyCallbackFromSession(DASessionRef session, SInt32 index)
{
	CFDictionaryRef callback = NULL;
	CFNumberRef key;

	if (session == NULL)
		return (NULL);
	key = index_number(index);
	if (key == NULL)
		return (NULL);
	pthread_mutex_lock(&session->_registerLock);
	callback = CFDictionaryGetValue(session->_register, key);
	if (callback != NULL)
		CFRetain(callback);
	pthread_mutex_unlock(&session->_registerLock);
	CFRelease(key);
	return (callback);
}

/* --- keep-alive ----------------------------------------------------------- */

struct recreate_ctx {
	DASessionRef	session;
};

static void
recreate_one(const void *key, const void *value, void *context)
{
	struct recreate_ctx *ctx = context;
	CFDictionaryRef callback = value;
	CFDataRef match, watch;
	SInt32 index = 0;
	SInt64 kind;

	kind = ___CFDictionaryGetIntegerValue(callback, _kDACallbackKindKey);
	/* Requests in flight died with the daemon; only registrations return. */
	if (kind > _kDADiskLastKind || kind < 0)
		return;
	if (!CFNumberGetValue(key, kCFNumberSInt32Type, &index))
		return;
	match = CFDictionaryGetValue(callback, _kDACallbackMatchKey);
	watch = CFDictionaryGetValue(callback, _kDACallbackWatchKey);
	(void)_DAServerSessionRegisterCallback(ctx->session->_server,
	    (mach_vm_offset_t)index, (mach_vm_offset_t)index, (uint32_t)kind,
	    (int32_t)___CFDictionaryGetIntegerValue(callback,
	    _kDACallbackOrderKey),
	    (vm_address_t)(match != NULL ? CFDataGetBytePtr(match) : 0),
	    (mach_msg_type_number_t)(match != NULL ? CFDataGetLength(match) : 0),
	    (vm_address_t)(watch != NULL ? CFDataGetBytePtr(watch) : 0),
	    (mach_msg_type_number_t)(watch != NULL ? CFDataGetLength(watch) :
	    0));
}

DAReturn
_DASessionRecreate(DASessionRef session)
{
	struct recreate_ctx ctx = { session };
	CFDictionaryRef copy;

	if (!DASessionEstablish(session)) {
		syslog(LOG_NOTICE, "DiskArbitration: failed to establish a "
		    "session with diskarbitrationd");
		return (kDAReturnBadArgument);
	}
	if (session->_queue != NULL)
		DASessionSetDispatchQueue(session, session->_queue);
	pthread_mutex_lock(&session->_registerLock);
	copy = CFDictionaryCreateCopy(kCFAllocatorDefault, session->_register);
	pthread_mutex_unlock(&session->_registerLock);
	if (copy != NULL) {
		CFDictionaryApplyFunction(copy, recreate_one, &ctx);
		CFRelease(copy);
	}
	syslog(LOG_NOTICE, "DiskArbitration: session %s [%d] re-established",
	    session->_name != NULL ? session->_name : "?", (int)session->_pid);
	return (_DAServerSessionSetKeepAlive(session->_server));
}

DAReturn
DASessionKeepAlive(DASessionRef session, dispatch_queue_t queue)
{
	if (session == NULL || queue == NULL)
		return (kDAReturnBadArgument);
	session->_keepAlive = true;
	if (session->_queue != queue) {
		dispatch_retain(queue);
		if (session->_queue != NULL)
			dispatch_release(session->_queue);
		session->_queue = queue;
	}
	if (session->_token == -1)
		(void)notify_register_dispatch(_kDADaemonLaunchedNotification,
		    &session->_token, queue, ^(int token __unused) {
			syslog(LOG_NOTICE, "DiskArbitration: "
			    "diskarbitrationd relaunched");
			if (session->_server == MACH_PORT_NULL)
				(void)_DASessionRecreate(session);
		});
	return (_DAServerSessionSetKeepAlive(session->_server));
}
