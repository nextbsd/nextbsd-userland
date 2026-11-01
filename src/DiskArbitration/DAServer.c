/*
 * DAServer.c — diskarbitrationd's MIG server (nextbsd-userland#197).
 *
 * Port of the session routines of Apple's diskarbitrationd/DAServer.c
 * (DiskArbitration-535.120.1.0.1). What each routine does on NextBSD:
 *
 *   _DAServerSessionCreate            a session for the caller; its pid and
 *                                     euid come from the audit trailer the
 *                                     receive loop asks the kernel for
 *                                     (ServerAuditToken), as configd's do
 *   _DAServerSessionSetClientPort     the doorbell port
 *   _DAServerSessionRegisterCallback  as Apple: registering for appearances
 *                                     replays every appeared disk, then
 *                                     DiskListComplete, then Idle if the
 *                                     daemon is idle; registering for Idle
 *                                     alone fires at once when idle
 *   _DAServerSessionCopyCallbackQueue the whole queue in one reply
 *   _DAServerSessionQueueResponse     a peek/approval answer (DAQueue.c)
 *   _DAServerSessionQueueRequest      mount, unmount and eject
 *                                     (DARequest.c, #197/#198), for the
 *                                     disk's owner, root, or the console
 *                                     user on removable and external
 *                                     media (#199); rename, claim,
 *                                     probe and refresh answer
 *                                     kDAReturnUnsupported until later
 *                                     tickets bring them
 *   _DAServerDiskCopyDescription      the disk's serialized description
 *   _DAServerDiskGetUserUID           the disk's owner
 *   _DAServerDiskGetOptions / IsClaimed / Unclaim
 *                                     no options or claims yet: 0, false,
 *                                     nothing to do
 *   _DAServerDiskSetOptions / SetAdoption
 *                                     kDAReturnUnsupported
 *
 * A disk is named by its ID ("/dev/vtbd0p2") or, for a client that made
 * its disk from a mount's f_mntfromname, by one of its GEOM label aliases
 * ("/dev/ufs/ROOTFS"); both find it.
 */
#define __DISKARBITRATIOND__ 1

#include "DAServer.h"
#include "DADisk.h"
#include "DAInternal.h"
#include "DAQueue.h"
#include "DARequest.h"
#include "DASession.h"
#include "DAMount.h"
#include "DAStage.h"
#include "DASupport.h"

#include <DiskArbitration/DADissenter.h>

#include <SystemConfiguration/SCDynamicStoreCopySpecificPrivate.h>

#include "DAServerServer.h"	/* MIG: DAServer_server() + routine protos */

#include <mach/notify.h>

#include <sys/param.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Apple's DiskArbitration.h kDADiskMountOption*. */
#define kDADiskMountOptionWhole	0x00000001

/*
 * Receive buffer: requests carry their property lists out of line, so a
 * request is a header, descriptors, two c_strings of at most 1 KiB and
 * the audit trailer.
 */
#define DA_MSG_BUFSZ	16384

#define DA_RCV_TRAILER							\
	(MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |		\
	 MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT))

mach_port_t		gDAServerPortSet = MACH_PORT_NULL;
static mach_port_t	g_service = MACH_PORT_NULL;
static dispatch_queue_t	g_queue;

/* A media disk by ID or label alias, else a mount-only disk by ID. */
static struct da_disk *
disk_lookup(const char *id)
{
	struct da_disk *disk;

	if (id == NULL || id[0] == '\0')
		return (NULL);
	disk = DADiskListFind(id);
	if (disk == NULL)
		disk = DADiskListFindMedia(id);
	return (disk);
}

static void
token_ids(audit_token_t token, pid_t *pid, uid_t *uid, gid_t *gid)
{
	uint32_t p = 0, u = 0, g = 0;

	audit_token_to_au32(token, NULL, &u, &g, NULL, NULL, &p, NULL, NULL);
	*pid = (pid_t)p;
	*uid = (uid_t)u;
	*gid = (gid_t)g;
}

/* __DASessionCancel / _DAServerSessionRelease. */
static void
session_cancel(struct da_session *session, const char *why)
{
	struct da_callback *callback;

	TAILQ_REMOVE(&gDASessionList, session, link);
	session->state |= kDASessionStateZombie;
	DALog("DA-SESSION: " DA_SESSION_FMT " released (%s)",
	    DA_SESSION_ARGS(session), why);
	while ((callback = TAILQ_FIRST(&session->callbacks)) != NULL) {
		TAILQ_REMOVE(&session->callbacks, callback, link);
		DACallbackFree(callback);
	}
	CFArrayRemoveAllValues(session->queue);
	DAQueueReleaseSession(session);
	if (session->server != MACH_PORT_NULL) {
		(void)mach_port_mod_refs(mach_task_self(), session->server,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		session->server = MACH_PORT_NULL;
	}
	if (session->client != MACH_PORT_NULL) {
		(void)mach_port_deallocate(mach_task_self(), session->client);
		session->client = MACH_PORT_NULL;
	}
	DASessionRelease(session);
}

/* --- disk routines -------------------------------------------------------- */

kern_return_t
_DAServerDiskCopyDescription(mach_port_t _session, caddr_t _disk,
    vm_address_t *_description, mach_msg_type_number_t *_descriptionCnt)
{
	struct da_disk *disk;
	CFDataRef data;

	*_description = 0;
	*_descriptionCnt = 0;
	if (DASessionListGetSession(_session) == NULL)
		return (kDAReturnBadArgument);
	disk = disk_lookup(_disk);
	if (disk == NULL) {
		DALogDebug("DA-SESSION: no disk %s to describe", _disk);
		return (kDAReturnBadArgument);
	}
	data = DADiskCopySerialization(disk);
	if (data == NULL)
		return (kDAReturnNoResources);
	*_description = ___CFDataCopyBytes(data, _descriptionCnt);
	CFRelease(data);
	return (*_description != 0 ? kDAReturnSuccess : kDAReturnNoResources);
}

kern_return_t
_DAServerDiskGetOptions(mach_port_t _session, caddr_t _disk,
    int32_t *_options)
{
	*_options = 0;
	if (DASessionListGetSession(_session) == NULL || disk_lookup(_disk) ==
	    NULL)
		return (kDAReturnBadArgument);
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerDiskGetUserUID(mach_port_t _session, caddr_t _disk, uid_t *_userUID)
{
	struct da_disk *disk;

	*_userUID = 0;
	if (DASessionListGetSession(_session) == NULL)
		return (kDAReturnBadArgument);
	disk = disk_lookup(_disk);
	if (disk == NULL)
		return (kDAReturnBadArgument);
	*_userUID = disk->user_uid;
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerDiskIsClaimed(mach_port_t _session, caddr_t _disk,
    boolean_t *_claimed)
{
	*_claimed = FALSE;
	if (DASessionListGetSession(_session) == NULL || disk_lookup(_disk) ==
	    NULL)
		return (kDAReturnBadArgument);
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerDiskSetAdoption(mach_port_t _session, caddr_t _disk __unused,
    boolean_t _adoption __unused, audit_token_t _token __unused)
{
	if (DASessionListGetSession(_session) == NULL)
		return (kDAReturnBadArgument);
	return (kDAReturnUnsupported);
}

kern_return_t
_DAServerDiskSetOptions(mach_port_t _session, caddr_t _disk __unused,
    int32_t _options __unused, int32_t _value __unused)
{
	if (DASessionListGetSession(_session) == NULL)
		return (kDAReturnBadArgument);
	return (kDAReturnUnsupported);
}

kern_return_t
_DAServerDiskUnclaim(mach_port_t _session __unused, caddr_t _disk __unused)
{
	return (kDAReturnSuccess);
}

/* --- session routines ----------------------------------------------------- */

kern_return_t
_DAServerSessionCopyCallbackQueue(mach_port_t _session, vm_address_t *_queue,
    mach_msg_type_number_t *_queueCnt)
{
	struct da_session *session;
	CFDataRef data;
	CFIndex count;

	*_queue = 0;
	*_queueCnt = 0;
	session = DASessionListGetSession(_session);
	if (session == NULL)
		return (kDAReturnBadArgument);
	count = CFArrayGetCount(session->queue);
	data = _DASerialize(kCFAllocatorDefault, session->queue);
	CFArrayRemoveAllValues(session->queue);
	/* Pulling the queue makes a timed-out session eligible again. */
	session->state &= ~kDASessionStateTimeout;
	if (data == NULL)
		return (kDAReturnNoResources);
	*_queue = ___CFDataCopyBytes(data, _queueCnt);
	CFRelease(data);
	DALog("DA-SESSION: " DA_SESSION_FMT " pulled %ld callback%s in one "
	    "queue copy", DA_SESSION_ARGS(session), (long)count,
	    count == 1 ? "" : "s");
	return (*_queue != 0 ? kDAReturnSuccess : kDAReturnNoResources);
}

kern_return_t
_DAServerSessionCreate(mach_port_t _session, caddr_t _name,
    audit_token_t _token, mach_port_t *_server)
{
	struct da_session *session;
	pid_t pid;
	uid_t uid;
	gid_t gid;

	*_server = MACH_PORT_NULL;
	if (_session != g_service)
		return (kDAReturnBadArgument);
	token_ids(_token, &pid, &uid, &gid);
	session = DASessionCreate(_name, pid, uid, gid);
	if (session == NULL) {
		DALog("DA-SESSION: cannot create a session for %s [%d]",
		    _name != NULL ? _name : "?", (int)pid);
		return (kDAReturnNoResources);
	}
	*_server = session->server;
	DALog("DA-SESSION: " DA_SESSION_FMT " created (uid %u)",
	    DA_SESSION_ARGS(session), (unsigned)uid);
	return (kDAReturnSuccess);
}

/* A client's mount request, from DARequestMount to its callback. */
struct client_mount {
	struct da_callback	*callback;	/* completion, for the session */
	char			*disk_id;
	uint64_t		 disk_serial;
	CFDataRef		 serialization;	/* the disk at request time */
};

static void
client_mount_done(int32_t status, const char *mountpoint, void *context)
{
	struct client_mount *request = context;
	struct da_disk *disk;
	CFDictionaryRef dissenter = NULL;
	CFDataRef serialization = NULL;

	disk = DADiskListFind(request->disk_id);
	if (disk != NULL && disk->serial == request->disk_serial)
		serialization = DADiskCopySerialization(disk);
	if (serialization == NULL && request->serialization != NULL)
		serialization = CFRetain(request->serialization);
	if (status != 0)
		dissenter = DADissenterCreate(status);
	DALog("DA-REQUEST: mount %s for " DA_SESSION_FMT ": %s%s%s",
	    request->disk_id, DA_SESSION_ARGS(request->callback->session),
	    status == 0 ? "mounted on " : "failed",
	    status == 0 && mountpoint != NULL ? mountpoint : "",
	    status == 0 ? "" : " (dissenter sent)");
	if (serialization != NULL) {
		DAQueueRequestCallback(request->callback, serialization,
		    dissenter);
		CFRelease(serialization);
	}
	if (dissenter != NULL)
		CFRelease(dissenter);
	DACallbackFree(request->callback);
	if (request->serialization != NULL)
		CFRelease(request->serialization);
	free(request->disk_id);
	free(request);
}

/* The mount point a client passed (a CFURL string, serialized). */
static char *
url_path(vm_address_t bytes, mach_msg_type_number_t length)
{
	CFTypeRef object;
	CFURLRef url = NULL;
	char path[MAXPATHLEN];
	char *result = NULL;

	object = _DAUnserializeWithBytes(kCFAllocatorDefault, bytes, length);
	if (object == NULL)
		return (NULL);
	if (CFGetTypeID(object) == CFStringGetTypeID())
		url = CFURLCreateWithString(kCFAllocatorDefault, object, NULL);
	if (url != NULL) {
		if (CFURLGetFileSystemRepresentation(url, TRUE, (UInt8 *)path,
		    sizeof(path)))
			result = strdup(path);
		CFRelease(url);
	}
	CFRelease(object);
	return (result);
}

/* The mount arguments a client passed (a CFString, serialized). */
static char *
arguments_string(vm_address_t bytes, mach_msg_type_number_t length)
{
	CFTypeRef object;
	char buf[1024];
	char *result = NULL;

	object = _DAUnserializeWithBytes(kCFAllocatorDefault, bytes, length);
	if (object == NULL)
		return (NULL);
	if (CFGetTypeID(object) == CFStringGetTypeID() &&
	    CFStringGetCString(object, buf, sizeof(buf), kCFStringEncodingUTF8))
		result = strdup(buf);
	CFRelease(object);
	return (result);
}

kern_return_t
_DAServerSessionQueueRequest(mach_port_t _session, uint32_t _kind,
    caddr_t _argument0, int32_t _argument1, vm_address_t _argument2,
    mach_msg_type_number_t _argument2Cnt, vm_address_t _argument3,
    mach_msg_type_number_t _argument3Cnt, mach_vm_offset_t _address,
    mach_vm_offset_t _context, audit_token_t _token)
{
	struct da_session *session;
	struct da_disk *disk;
	struct client_mount *request;
	char *path = NULL, *options = NULL;
	pid_t pid;
	uid_t uid, cuid;
	gid_t gid, cgid;

	session = DASessionListGetSession(_session);
	if (session == NULL)
		return (kDAReturnBadArgument);
	disk = disk_lookup(_argument0);
	if (disk == NULL)
		return (kDAReturnBadArgument);

	if (_kind != _kDADiskMount && _kind != _kDADiskUnmount &&
	    _kind != _kDADiskEject) {
		DALog("DA-REQUEST: %s %s for " DA_SESSION_FMT ": not supported "
		    "yet", _DACallbackKindGetName(_kind), disk->id,
		    DA_SESSION_ARGS(session));
		return (kDAReturnUnsupported);
	}
	if (_kind == _kDADiskMount && (_argument1 & kDADiskMountOptionWhole)) {
		DALog("DA-REQUEST: mount %s for " DA_SESSION_FMT ": "
		    "kDADiskMountOptionWhole is not supported yet", disk->id,
		    DA_SESSION_ARGS(session));
		return (kDAReturnUnsupported);
	}

	/*
	 * Apple's DAAuthorize(_kDAAuthorizeOptionIsOwner): the disk's owner
	 * or root, for mount, unmount and eject alike. Apple then asks
	 * Authorization Services for system.volume.<class>.<right>; NextBSD
	 * has no authorization server, so in its place the console user
	 * (#199) is allowed on the media DAConsoleUserMayOwn accepts:
	 * removable or external disks that carry no system mount.
	 */
	token_ids(_token, &pid, &uid, &gid);
	if (uid != 0 && uid != disk->user_uid && !(DAConsoleUserGet(&cuid,
	    &cgid) && uid == cuid && DAConsoleUserMayOwn(disk))) {
		DALog("DA-REQUEST: %s %s for " DA_SESSION_FMT ": uid %u is "
		    "not the owner", _kind == _kDADiskMount ? "mount" :
		    _kind == _kDADiskUnmount ? "unmount" : "eject", disk->id,
		    DA_SESSION_ARGS(session), (unsigned)uid);
		return (kDAReturnNotPrivileged);
	}

	if (_kind != _kDADiskMount) {
		struct da_callback *callback;

		callback = DACallbackCreate(session, _address, _context, _kind,
		    0, NULL, NULL);
		if (callback == NULL)
			return (kDAReturnNoResources);
		DALog("DA-REQUEST: %s %s for " DA_SESSION_FMT " (uid %u)%s%s",
		    _kind == _kDADiskUnmount ? "unmount" : "eject", disk->id,
		    DA_SESSION_ARGS(session), (unsigned)uid,
		    _kind == _kDADiskUnmount &&
		    (_argument1 & kDADiskUnmountOptionForce) ? " force" : "",
		    _kind == _kDADiskUnmount &&
		    (_argument1 & kDADiskUnmountOptionWhole) ? " whole" : "");
		DAQueueRequest(_kind, disk,
		    _kind == _kDADiskUnmount ? _argument1 : 0, uid, gid,
		    callback);
		return (kDAReturnSuccess);
	}

	if (_argument2 != 0) {
		path = url_path(_argument2, _argument2Cnt);
		if (path == NULL)
			return (kDAReturnBadArgument);
	}
	if (_argument3 != 0)
		options = arguments_string(_argument3, _argument3Cnt);

	request = calloc(1, sizeof(*request));
	if (request == NULL || (request->disk_id = strdup(disk->id)) == NULL ||
	    (request->callback = DACallbackCreate(session, _address, _context,
	    _kDADiskMountCallback, 0, NULL, NULL)) == NULL) {
		if (request != NULL)
			free(request->disk_id);
		free(request);
		free(path);
		free(options);
		return (kDAReturnNoResources);
	}
	request->disk_serial = disk->serial;
	request->serialization = DADiskCopySerialization(disk);
	DALog("DA-REQUEST: mount %s for " DA_SESSION_FMT "%s%s%s%s",
	    disk->id, DA_SESSION_ARGS(session), path != NULL ? " on " : "",
	    path != NULL ? path : "", options != NULL ? " options " : "",
	    options != NULL ? options : "");
	DARequestMount(disk, path, 0, options, client_mount_done, request);
	free(path);
	free(options);
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerSessionQueueResponse(mach_port_t _session,
    mach_vm_offset_t _address __unused, mach_vm_offset_t _context __unused,
    uint32_t _kind, caddr_t _disk, vm_address_t _response,
    mach_msg_type_number_t _responseCnt, int32_t _responseID)
{
	CFTypeRef response = NULL;

	if (DASessionListGetSession(_session) == NULL)
		return (kDAReturnBadArgument);
	if (_response != 0)
		response = _DAUnserializeWithBytes(kCFAllocatorDefault,
		    _response, _responseCnt);
	if (!DAResponseDispatch(response, _responseID))
		DALogDebug("DA-SESSION: %s response for %s orphaned",
		    _DACallbackKindGetName(_kind), _disk);
	if (response != NULL)
		CFRelease(response);
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerSessionRegisterCallback(mach_port_t _session,
    mach_vm_offset_t _address, mach_vm_offset_t _context, uint32_t _kind,
    int32_t _order, vm_address_t _match, mach_msg_type_number_t _matchCnt,
    vm_address_t _watch, mach_msg_type_number_t _watchCnt)
{
	struct da_session *session;
	struct da_callback *callback;
	struct da_disk *disk, *next;
	CFDictionaryRef match = NULL;
	CFTypeRef watch = NULL;
	int replayed = 0;

	session = DASessionListGetSession(_session);
	if (session == NULL || _kind > _kDADiskLastKind)
		return (kDAReturnBadArgument);
	if (_match != 0)
		match = _DAUnserializeDiskDescriptionWithBytes(
		    kCFAllocatorDefault, _match, _matchCnt);
	if (_watch != 0) {
		watch = _DAUnserializeWithBytes(kCFAllocatorDefault, _watch,
		    _watchCnt);
		if (watch != NULL && CFGetTypeID(watch) != CFArrayGetTypeID()) {
			CFRelease(watch);
			watch = NULL;
		}
	}
	callback = DACallbackCreate(session, _address, _context, _kind, _order,
	    match, watch);
	if (match != NULL)
		CFRelease(match);
	if (watch != NULL)
		CFRelease(watch);
	if (callback == NULL)
		return (kDAReturnNoResources);
	TAILQ_INSERT_TAIL(&session->callbacks, callback, link);

	if (_kind == _kDADiskAppearedCallback) {
		/* Replay what has appeared, then say the list is complete. */
		TAILQ_FOREACH_SAFE(disk, &gDADiskList, link, next) {
			if (disk->state & kDADiskStateStagedAppear) {
				DAQueueCallback(callback, disk, NULL);
				replayed++;
			}
		}
		DAQueueCallbacks(session, _kDADiskListCompleteCallback, NULL,
		    NULL);
		if (gDAIdle) {
			DAQueueCallbacks(session, _kDAIdleCallback, NULL, NULL);
			session->state |= kDASessionStateIdle;
		}
		DALog("DA-SESSION: " DA_SESSION_FMT " registered for disk "
		    "appearances: %d disk%s replayed, list complete%s",
		    DA_SESSION_ARGS(session), replayed, replayed == 1 ? "" : "s",
		    gDAIdle ? ", idle" : "");
	} else if (_kind == _kDAIdleCallback) {
		if (gDAIdle) {
			DAQueueCallback(callback, NULL, NULL);
			session->state |= kDASessionStateIdle;
		} else
			session->state &= ~kDASessionStateIdle;
	}
	DALogDebug("DA-SESSION: " DA_SESSION_FMT " registered %s callback",
	    DA_SESSION_ARGS(session), _DACallbackKindGetName(_kind));
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerSessionRelease(mach_port_t _session)
{
	struct da_session *session;

	session = DASessionListGetSession(_session);
	if (session == NULL)
		return (kDAReturnBadArgument);
	session_cancel(session, "client request");
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerSessionSetKeepAlive(mach_port_t _session)
{
	struct da_session *session;

	session = DASessionListGetSession(_session);
	if (session == NULL)
		return (kDAReturnBadArgument);
	session->keepalive = 1;
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerSessionSetClientPort(mach_port_t _session, mach_port_t _client)
{
	struct da_session *session;

	session = DASessionListGetSession(_session);
	if (session == NULL) {
		/* serve_one destroys the request, and the right with it. */
		return (kDAReturnBadArgument);
	}
	DASessionSetClientPort(session, _client);
	return (kDAReturnSuccess);
}

kern_return_t
_DAServerSessionUnregisterCallback(mach_port_t _session,
    mach_vm_offset_t _address, mach_vm_offset_t _context)
{
	struct da_session *session;
	struct da_callback *callback, *next;

	session = DASessionListGetSession(_session);
	if (session == NULL)
		return (kDAReturnBadArgument);
	TAILQ_FOREACH_SAFE(callback, &session->callbacks, link, next) {
		if (callback->address == _address &&
		    callback->context == _context) {
			TAILQ_REMOVE(&session->callbacks, callback, link);
			DACallbackFree(callback);
		}
	}
	return (kDAReturnSuccess);
}

/* --- the console user (#199) --------------------------------------------- */

/* A deferred mount at login; the disk waits for it, as in the mount stage. */
struct login_mount {
	char		*id;
	uint64_t	 serial;
};

static void
login_mount_done(int32_t status, const char *mountpoint, void *context)
{
	struct login_mount *ctx = context;
	struct da_disk *disk;

	disk = DADiskListFind(ctx->id);
	if (disk != NULL && disk->serial == ctx->serial)
		disk->state &= ~kDADiskStateCommandActive;
	if (status == 0)
		DALog("DA-CONSOLE: deferred %s mounted on \"%s\"", ctx->id,
		    mountpoint != NULL ? mountpoint : "?");
	free(ctx->id);
	free(ctx);
	DAStageSignal();
}

static void
login_mount(struct da_disk *disk)
{
	struct login_mount *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL || (ctx->id = strdup(disk->id)) == NULL) {
		free(ctx);
		DALog("DA-CONSOLE: %s: out of memory for the deferred mount",
		    disk->id);
		return;
	}
	ctx->serial = disk->serial;
	disk->state |= kDADiskStateCommandActive;
	DALog("DA-CONSOLE: mounting deferred disk %s", disk->id);
	DARequestMount(disk, NULL, 1, NULL, login_mount_done, ctx);
}

static int
disk_mountable(const struct da_disk *disk)
{
	return (DADiskGetDescription(disk,
	    kDADiskDescriptionVolumeMountableKey) == kCFBooleanTrue);
}

static int
disk_mounted(const struct da_disk *disk)
{
	return (DADiskGetDescription(disk,
	    kDADiskDescriptionVolumePathKey) != NULL);
}

/* The uid of a console session dictionary, or -1. */
static long
session_uid(CFDictionaryRef session)
{
	CFTypeRef value;

	value = CFDictionaryGetValue(session, kSCConsoleSessionUID);
	if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID())
		return (-1);
	return ((long)___CFNumberGetIntegerValue(value));
}

/*
 * Apple's _DAConfigurationCallback (DAServer.c), less the SystemUIServer
 * wait, the /dev node chmod/chown (NextBSD gives the whole disk to the
 * console user instead, DAConsoleUserClaim), the encrypted-volume
 * MountedByUserUID case, CoreStorage and the .SoftwareUpdateAtLogout
 * cookie:
 *
 *   logout   the previous user has no session left: unmount (Force) every
 *            volume they own, then eject (Force) every whole disk they own;
 *   login    somebody is logged in: the removable and external whole disks
 *            are theirs, so are such volumes that mounted root's while
 *            nobody was logged in (AutomountDisksWithoutUserLogin), and if
 *            nobody was logged in before, every deferred volume is mounted
 *            (DARequestMount "automatic", so the mount approval round and
 *            the /etc/fstab map apply). System volumes and their disks
 *            (kDADiskStateMountSystem) are never handed over;
 *   nobody   the whole disks go back to root and every mounted volume that
 *            would have been deferred is unmounted.
 */
void
_DAConfigurationCallback(SCDynamicStoreRef session, CFArrayRef keys __unused,
    void *info __unused)
{
	CFStringRef previousUser;
	CFArrayRef previousUserList;
	uid_t previousUserUID;
	struct da_disk *disk, *next;
	CFDictionaryRef dictionary;
	CFIndex count, index;
	uid_t uid;
	gid_t gid;

	previousUser = gDAConsoleUser;
	previousUserUID = gDAConsoleUserUID;
	previousUserList = gDAConsoleUserList;

	gDAConsoleUser = ___SCDynamicStoreCopyConsoleUser(session, &uid, &gid);
	gDAConsoleUserUID = uid;
	gDAConsoleUserGID = gid;
	gDAConsoleUserList = ___SCDynamicStoreCopyConsoleInformation(session);
	DAConsoleUserLog("State:/Users/ConsoleUser changed");

	if (gDAConsoleUser == NULL) {
		/* A console user has logged out: is any session still theirs? */
		count = gDAConsoleUserList != NULL ?
		    CFArrayGetCount(gDAConsoleUserList) : 0;
		for (index = 0; index < count; index++) {
			dictionary = CFArrayGetValueAtIndex(gDAConsoleUserList,
			    index);
			if (CFDictionaryGetValue(dictionary,
			    CFSTR("kCGSSessionIsLoggingOutKey")) ==
			    kCFBooleanTrue)
				continue;
			if (session_uid(dictionary) == (long)previousUserUID)
				break;
		}
		if (index == count && previousUserUID != 0) {
			TAILQ_FOREACH_SAFE(disk, &gDADiskList, link, next) {
				if (!disk_mountable(disk) ||
				    !disk_mounted(disk) ||
				    disk->user_uid == 0 ||
				    disk->user_uid != previousUserUID)
					continue;
				DALog("DA-CONSOLE: logout of uid %u: "
				    "unmounting %s", (unsigned)previousUserUID,
				    disk->id);
				DAQueueRequest(_kDADiskUnmount, disk,
				    kDADiskUnmountOptionForce, 0, 0, NULL);
			}
			TAILQ_FOREACH_SAFE(disk, &gDADiskList, link, next) {
				if (DADiskGetDescription(disk,
				    kDADiskDescriptionMediaWholeKey) !=
				    kCFBooleanTrue || disk->volume_only ||
				    disk->user_uid == 0 ||
				    disk->user_uid != previousUserUID)
					continue;
				DALog("DA-CONSOLE: logout of uid %u: "
				    "ejecting %s", (unsigned)previousUserUID,
				    disk->id);
				DAQueueRequest(_kDADiskEject, disk,
				    kDADiskUnmountOptionForce, 0, 0, NULL);
			}
		}
	}

	if (gDAConsoleUserList != NULL) {
		/* A console user is logged in. */
		if (previousUserList == NULL)
			DALog("DA-CONSOLE: console user change: start mounting "
			    "disks");
		TAILQ_FOREACH_SAFE(disk, &gDADiskList, link, next) {
			DAConsoleUserClaim(disk);
			if (previousUserList != NULL || disk->volume_only ||
			    !disk_mountable(disk) || disk_mounted(disk) ||
			    disk->filesystem == NULL ||
			    (disk->state & kDADiskStateStagedMount) == 0 ||
			    (disk->state & kDADiskStateCommandActive) != 0)
				continue;
			if (!DAMountGetPreference(disk,
			    kDAMountPreferenceDisableAutoMount) &&
			    DAMountGetPreference(disk,
			    kDAMountPreferenceDefer))
				login_mount(disk);
		}
	} else {
		/* A console user is not logged in. */
		DAPreferenceListRefresh();
		TAILQ_FOREACH_SAFE(disk, &gDADiskList, link, next) {
			DAConsoleUserClaim(disk);
			if (!disk_mountable(disk) || !disk_mounted(disk) ||
			    disk->volume_only)
				continue;
			if (DAMountGetPreference(disk,
			    kDAMountPreferenceDefer) &&
			    (disk->state & _kDADiskStateMountAutomaticNoDefer)
			    == 0) {
				DALog("DA-CONSOLE: no console user: unmounting "
				    "%s", disk->id);
				DAQueueRequest(_kDADiskUnmount, disk, 0, 0, 0,
				    NULL);
			}
		}
	}

	if (previousUser != NULL)
		CFRelease(previousUser);
	if (previousUserList != NULL)
		CFRelease(previousUserList);
	DAStageSignal();
}

/* --- the receive loop ----------------------------------------------------- */

struct da_message {
	union {
		mach_msg_header_t	hdr;
		char			buf[DA_MSG_BUFSZ];
	} req, rep;
};

/* One message, on the daemon queue (configd's configd_serve body). */
static void
serve_one(void *context)
{
	struct da_message *m = context;
	struct da_session *session;
	mach_msg_return_t mr;
	int rejected;

	if (m->req.hdr.msgh_id == MACH_NOTIFY_NO_SENDERS) {
		/* A client exited (or dropped its session). */
		session = DASessionListGetSession(m->req.hdr.msgh_local_port);
		if (session != NULL)
			session_cancel(session, "client gone");
		return;
	}

	memset(&m->rep, 0, sizeof(m->rep));
	if (!DAServer_server(&m->req.hdr, &m->rep.hdr)) {
		DALog("DA-SESSION: unhandled message id %d",
		    (int)m->req.hdr.msgh_id);
		mach_msg_destroy(&m->req.hdr);
		return;
	}
	rejected = !(m->rep.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
	    ((mig_reply_error_t *)(void *)&m->rep.hdr)->RetCode != KERN_SUCCESS &&
	    ((mig_reply_error_t *)(void *)&m->rep.hdr)->RetCode != MIG_NO_REPLY;
	if (rejected) {
		/*
		 * MIG or the routine failed before taking ownership of the
		 * request's out-of-line memory and rights: release them, but
		 * keep the reply right for the error reply.
		 */
		m->req.hdr.msgh_remote_port = MACH_PORT_NULL;
		mach_msg_destroy(&m->req.hdr);
	}
	if (m->rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
		mr = mach_msg(&m->rep.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
		    m->rep.hdr.msgh_size, 0, MACH_PORT_NULL, 1000,
		    MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS) {
			DALogDebug("DA-SESSION: reply send failed: 0x%x",
			    (unsigned)mr);
			mach_msg_destroy(&m->rep.hdr);
		}
	}
	/*
	 * A simpleroutine that succeeded has no reply right to dispose of,
	 * and its routine owns whatever rights the request carried (the
	 * client port _DAServerSessionSetClientPort keeps): leave the request
	 * alone, as Apple's mach_msg_server does for MIG_NO_REPLY.
	 */
}

int
DAServerInit(mach_port_t service, dispatch_queue_t queue)
{
	kern_return_t kr;

	g_service = service;
	g_queue = queue;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
	    &gDAServerPortSet);
	if (kr != KERN_SUCCESS) {
		DALog("DA-SESSION: port set allocate failed: 0x%x",
		    (unsigned)kr);
		return (-1);
	}
	kr = mach_port_move_member(mach_task_self(), service, gDAServerPortSet);
	if (kr != KERN_SUCCESS) {
		DALog("DA-SESSION: port set move_member failed: 0x%x",
		    (unsigned)kr);
		return (-1);
	}
	return (0);
}

void
DAServerRun(volatile sig_atomic_t *stop)
{
	struct da_message *m;
	mach_msg_return_t mr;

	m = malloc(sizeof(*m));
	if (m == NULL) {
		DALog("DA-SESSION: out of memory for the receive buffer");
		return;
	}
	while (!*stop) {
		memset(&m->req, 0, sizeof(m->req));
		mr = mach_msg(&m->req.hdr,
		    MACH_RCV_MSG | MACH_RCV_TIMEOUT | DA_RCV_TRAILER, 0,
		    sizeof(m->req), gDAServerPortSet, 1000, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			DALog("DA-SESSION: mach_msg receive failed: 0x%x",
			    (unsigned)mr);
			continue;
		}
		dispatch_sync_f(g_queue, m, serve_one);
	}
	free(m);
}
