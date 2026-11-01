/*
 * DASession.c — diskarbitrationd's client sessions (nextbsd-userland#197).
 *
 * Port of Apple's diskarbitrationd/DASession.c and DACallback.c
 * (DiskArbitration-535.120.1.0.1). Apple serves each session on its own
 * dispatch_mach channel; here each session's receive right joins the
 * daemon's port set and DAServer.c's receive loop serves them all, the
 * way configd serves its sessions. The client's death is the session
 * port's MACH_NOTIFY_NO_SENDERS (Apple's channel does the same).
 */
#include "DASession.h"
#include "DADisk.h"
#include "DAServer.h"

#include <mach/notify.h>

#include <stdlib.h>
#include <string.h>

struct da_session_list gDASessionList = TAILQ_HEAD_INITIALIZER(gDASessionList);

struct da_session *
DASessionCreate(const char *name, pid_t pid, uid_t uid, gid_t gid)
{
	struct da_session *session;
	mach_port_t port = MACH_PORT_NULL, prev = MACH_PORT_NULL;

	session = calloc(1, sizeof(*session));
	if (session == NULL)
		return (NULL);
	TAILQ_INIT(&session->callbacks);
	session->name = strdup(name != NULL && name[0] != '\0' ? name : "?");
	session->pid = pid;
	session->uid = uid;
	session->gid = gid;
	session->refs = 1;
	session->queue = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	if (session->name == NULL || session->queue == NULL)
		goto fail;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &port) != KERN_SUCCESS)
		goto fail;
	if (mach_port_move_member(mach_task_self(), port, gDAServerPortSet) !=
	    KERN_SUCCESS) {
		(void)mach_port_mod_refs(mach_task_self(), port,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		goto fail;
	}
	/*
	 * The client's last send right going away (it exited, or released
	 * the session) posts MACH_NOTIFY_NO_SENDERS to the port itself; the
	 * receive loop then cancels the session. No send right exists yet,
	 * so with sync 1 this only arms.
	 */
	(void)mach_port_request_notification(mach_task_self(), port,
	    MACH_NOTIFY_NO_SENDERS, 1, port, MACH_MSG_TYPE_MAKE_SEND_ONCE,
	    &prev);
	if (prev != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), prev);
	session->server = port;
	TAILQ_INSERT_TAIL(&gDASessionList, session, link);
	return (session);

fail:
	if (session->queue != NULL)
		CFRelease(session->queue);
	free(session->name);
	free(session);
	return (NULL);
}

void
DASessionRetain(struct da_session *session)
{
	session->refs++;
}

void
DASessionRelease(struct da_session *session)
{
	if (--session->refs > 0)
		return;
	if (session->client != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), session->client);
	if (session->server != MACH_PORT_NULL)
		(void)mach_port_mod_refs(mach_task_self(), session->server,
		    MACH_PORT_RIGHT_RECEIVE, -1);
	CFRelease(session->queue);
	free(session->name);
	free(session);
}

struct da_session *
DASessionListGetSession(mach_port_t port)
{
	struct da_session *session;

	if (port == MACH_PORT_NULL)
		return (NULL);
	TAILQ_FOREACH(session, &gDASessionList, link)
		if (session->server == port)
			return (session);
	return (NULL);
}

/* One empty message, msgh_id 0, never blocking (Apple's doorbell). */
static void
ring(struct da_session *session)
{
	mach_msg_header_t message;
	kern_return_t status;

	if (session->client == MACH_PORT_NULL)
		return;
	memset(&message, 0, sizeof(message));
	message.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	message.msgh_id = 0;
	message.msgh_local_port = MACH_PORT_NULL;
	message.msgh_remote_port = session->client;
	message.msgh_size = sizeof(message);
	status = mach_msg(&message, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    message.msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
	/*
	 * MACH_SEND_TIMED_OUT: the client's port (queue limit 1) already
	 * holds an unread doorbell, which is just as good.
	 */
	if (status != MACH_MSG_SUCCESS && status != MACH_SEND_TIMED_OUT)
		DALogDebug("DA-SESSION: " DA_SESSION_FMT " doorbell: 0x%x",
		    DA_SESSION_ARGS(session), (unsigned)status);
}

void
DASessionSetClientPort(struct da_session *session, mach_port_t client)
{
	if (session->client != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), session->client);
	session->client = client;
	if (CFArrayGetCount(session->queue) > 0)
		ring(session);
}

void
DASessionQueueCallback(struct da_session *session, CFDictionaryRef callback)
{
	session->state &= ~kDASessionStateIdle;
	CFArrayAppendValue(session->queue, callback);
	if (CFArrayGetCount(session->queue) == 1)
		ring(session);
}

struct da_callback *
DACallbackCreate(struct da_session *session, uint64_t address,
    uint64_t context, _DACallbackKind kind, int32_t order,
    CFDictionaryRef match, CFArrayRef watch)
{
	struct da_callback *callback;

	callback = calloc(1, sizeof(*callback));
	if (callback == NULL)
		return (NULL);
	DASessionRetain(session);
	callback->session = session;
	callback->address = address;
	callback->context = context;
	callback->kind = kind;
	callback->order = order;
	if (match != NULL)
		callback->match = CFRetain(match);
	if (watch != NULL)
		callback->watch = CFRetain(watch);
	return (callback);
}

void
DACallbackFree(struct da_callback *callback)
{
	if (callback == NULL)
		return;
	if (callback->match != NULL)
		CFRelease(callback->match);
	if (callback->watch != NULL)
		CFRelease(callback->watch);
	DASessionRelease(callback->session);
	free(callback);
}
