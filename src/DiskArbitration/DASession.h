/*
 * DASession.h — diskarbitrationd's client sessions and their callbacks
 * (nextbsd-userland#197).
 *
 * The C counterpart of Apple's diskarbitrationd/DASession.c and DACallback.c
 * (DiskArbitration-535.120.1.0.1). A session is one DASessionRef in a client
 * process: a receive right the client sends its requests to (its "server
 * port"), a send right to the client's port for the doorbell, the callbacks
 * the client registered, and the queue of callbacks waiting for the client
 * to pull them with _DAServerSessionCopyCallbackQueue.
 *
 * A callback here is what the client registered (or the completion of a
 * request it queued): the session, the client-side address/context pair
 * (an index into the client's own table, as on Apple; the daemon never
 * sees a function pointer), the kind, the peek order and the optional
 * match dictionary and watch array. All access is on the daemon queue.
 */
#ifndef DISKARBITRATION_DASESSION_H
#define DISKARBITRATION_DASESSION_H

#include <CoreFoundation/CoreFoundation.h>

#include <mach/mach.h>

#include <sys/types.h>
#include <sys/queue.h>
#include <stdint.h>

#include "DAInternal.h"

/* Session state bits (Apple's kDASessionState*). */
enum {
	kDASessionStateIdle	= 0x00000001,	/* told it the daemon is idle */
	kDASessionStateTimeout	= 0x00000002,	/* missed a response deadline */
	kDASessionStateZombie	= 0x00000004	/* released; refs remain */
};

struct da_session;

struct da_callback {
	TAILQ_ENTRY(da_callback) link;		/* session's register */
	struct da_session	*session;	/* holds a reference */
	uint64_t		 address;
	uint64_t		 context;
	_DACallbackKind		 kind;
	int32_t			 order;
	CFDictionaryRef		 match;		/* unserialized description */
	CFArrayRef		 watch;		/* description keys */
};

TAILQ_HEAD(da_callback_list, da_callback);

struct da_session {
	TAILQ_ENTRY(da_session)	 link;		/* gDASessionList */
	char			*name;		/* client program name */
	pid_t			 pid;		/* from the audit token */
	uid_t			 uid;		/* effective uid of the client */
	gid_t			 gid;
	mach_port_t		 server;	/* receive right, in the port set */
	mach_port_t		 client;	/* send right for the doorbell */
	struct da_callback_list	 callbacks;	/* registered */
	CFMutableArrayRef	 queue;		/* pending callback dictionaries */
	uint32_t		 state;
	int			 keepalive;
	int			 refs;
};

TAILQ_HEAD(da_session_list, da_session);
extern struct da_session_list gDASessionList;

/* A new session on a fresh receive right; NULL on failure. */
struct da_session	*DASessionCreate(const char *name, pid_t pid, uid_t uid,
			    gid_t gid);
void			 DASessionRetain(struct da_session *session);
void			 DASessionRelease(struct da_session *session);
/* The live session whose server port is `port`, or NULL. */
struct da_session	*DASessionListGetSession(mach_port_t port);

void	DASessionSetClientPort(struct da_session *session, mach_port_t client);
/*
 * Append one callback dictionary to the session's queue and ring the
 * doorbell when the queue goes from empty to one (Apple's
 * DASessionQueueCallback): one empty message, never more, however many
 * callbacks pile up before the client pulls.
 */
void	DASessionQueueCallback(struct da_session *session,
	    CFDictionaryRef callback);

/* A callback for `session`; the session is retained. */
struct da_callback	*DACallbackCreate(struct da_session *session,
			    uint64_t address, uint64_t context,
			    _DACallbackKind kind, int32_t order,
			    CFDictionaryRef match, CFArrayRef watch);
void			 DACallbackFree(struct da_callback *callback);

/* Session log line (DA-SESSION:), via DALog. */
#define DA_SESSION_FMT		"%s [%d]"
#define DA_SESSION_ARGS(s)	((s)->name != NULL ? (s)->name : "?"), \
				(int)(s)->pid

#endif /* DISKARBITRATION_DASESSION_H */
