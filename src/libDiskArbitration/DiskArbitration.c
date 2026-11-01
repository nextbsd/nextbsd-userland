/*
 * DiskArbitration.c — libDiskArbitration's registration, request and
 * callback dispatch (nextbsd-userland#197).
 *
 * Port of Apple's DiskArbitration/DiskArbitration.c
 * (DiskArbitration-535.120.1.0.1), plus the description key constants
 * Apple defines in the shared DAInternal.c. Every call goes to
 * diskarbitrationd over DAServer.defs; callbacks come back through the
 * session's queue (DASession.c) and run here (_DADispatchCallback).
 *
 * NextBSD differences:
 *   - No Authorization Services. _DAAuthorize lets root and the disk's
 *     owner through and refuses anyone else with kDAReturnNotPrivileged;
 *     the daemon applies the same rule to the caller's audit token.
 *   - No block variants, no _DAServerSessionQueueRequestWithUserToken and
 *     no FSKit calls.
 *   - The daemon serves mount, unmount and eject requests (#197, #198).
 *     Rename and claim are sent the same way and come back with a
 *     kDAReturnUnsupported dissenter until the daemon implements them.
 *   - A request the daemon refuses outright drops its callback table entry
 *     (Apple leaves it behind).
 */
#include "DAClient.h"
#include "DAServerUser.h"	/* MIG: _DAServer* user stubs */

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

/* --- constants ------------------------------------------------------------ */

const CFStringRef kDADiskDescriptionVolumeKindKey	= CFSTR("DAVolumeKind");
const CFStringRef kDADiskDescriptionVolumeMountableKey	= CFSTR("DAVolumeMountable");
const CFStringRef kDADiskDescriptionVolumeNameKey	= CFSTR("DAVolumeName");
const CFStringRef kDADiskDescriptionVolumeNetworkKey	= CFSTR("DAVolumeNetwork");
const CFStringRef kDADiskDescriptionVolumePathKey	= CFSTR("DAVolumePath");
const CFStringRef kDADiskDescriptionVolumeTypeKey	= CFSTR("DAVolumeType");
const CFStringRef kDADiskDescriptionVolumeUUIDKey	= CFSTR("DAVolumeUUID");

const CFStringRef kDADiskDescriptionMediaBlockSizeKey	= CFSTR("DAMediaBlockSize");
const CFStringRef kDADiskDescriptionMediaBSDMajorKey	= CFSTR("DAMediaBSDMajor");
const CFStringRef kDADiskDescriptionMediaBSDMinorKey	= CFSTR("DAMediaBSDMinor");
const CFStringRef kDADiskDescriptionMediaBSDNameKey	= CFSTR("DAMediaBSDName");
const CFStringRef kDADiskDescriptionMediaBSDUnitKey	= CFSTR("DAMediaBSDUnit");
const CFStringRef kDADiskDescriptionMediaContentKey	= CFSTR("DAMediaContent");
const CFStringRef kDADiskDescriptionMediaEjectableKey	= CFSTR("DAMediaEjectable");
const CFStringRef kDADiskDescriptionMediaIconKey	= CFSTR("DAMediaIcon");
const CFStringRef kDADiskDescriptionMediaKindKey	= CFSTR("DAMediaKind");
const CFStringRef kDADiskDescriptionMediaLeafKey	= CFSTR("DAMediaLeaf");
const CFStringRef kDADiskDescriptionMediaNameKey	= CFSTR("DAMediaName");
const CFStringRef kDADiskDescriptionMediaPathKey	= CFSTR("DAMediaPath");
const CFStringRef kDADiskDescriptionMediaRemovableKey	= CFSTR("DAMediaRemovable");
const CFStringRef kDADiskDescriptionMediaSizeKey	= CFSTR("DAMediaSize");
const CFStringRef kDADiskDescriptionMediaTypeKey	= CFSTR("DAMediaType");
const CFStringRef kDADiskDescriptionMediaUUIDKey	= CFSTR("DAMediaUUID");
const CFStringRef kDADiskDescriptionMediaWholeKey	= CFSTR("DAMediaWhole");
const CFStringRef kDADiskDescriptionMediaWritableKey	= CFSTR("DAMediaWritable");
const CFStringRef kDADiskDescriptionMediaEncryptedKey	= CFSTR("DAMediaEncrypted");
const CFStringRef kDADiskDescriptionMediaEncryptionDetailKey = CFSTR("DAMediaEncryptionDetail");

const CFStringRef kDADiskDescriptionDeviceGUIDKey	= CFSTR("DADeviceGUID");
const CFStringRef kDADiskDescriptionDeviceInternalKey	= CFSTR("DADeviceInternal");
const CFStringRef kDADiskDescriptionDeviceModelKey	= CFSTR("DADeviceModel");
const CFStringRef kDADiskDescriptionDevicePathKey	= CFSTR("DADevicePath");
const CFStringRef kDADiskDescriptionDeviceProtocolKey	= CFSTR("DADeviceProtocol");
const CFStringRef kDADiskDescriptionDeviceRevisionKey	= CFSTR("DADeviceRevision");
const CFStringRef kDADiskDescriptionDeviceUnitKey	= CFSTR("DADeviceUnit");
const CFStringRef kDADiskDescriptionDeviceVendorKey	= CFSTR("DADeviceVendor");
const CFStringRef kDADiskDescriptionDeviceTDMLockedKey	= CFSTR("DADeviceTDMLocked");

const CFStringRef kDADiskDescriptionBusNameKey		= CFSTR("DABusName");
const CFStringRef kDADiskDescriptionBusPathKey		= CFSTR("DABusPath");

const CFStringRef kDADiskDescriptionAppearanceTimeKey	= CFSTR("DAAppearanceTime");
const CFStringRef kDADiskDescriptionMediaMatchKey	= CFSTR("DAMediaMatch");
const CFStringRef kDADiskDescriptionFSKitPrefix		= CFSTR("FS");
const CFStringRef kDADiskDescriptionRepairRunningKey	= CFSTR("DARepairRunning");

CFDictionaryRef kDADiskDescriptionMatchMediaUnformatted   = NULL;
CFDictionaryRef kDADiskDescriptionMatchMediaWhole         = NULL;
CFDictionaryRef kDADiskDescriptionMatchVolumeMountable    = NULL;
CFDictionaryRef kDADiskDescriptionMatchVolumeUnrecognized = NULL;

CFArrayRef kDADiskDescriptionWatchVolumeName = NULL;
CFArrayRef kDADiskDescriptionWatchVolumePath = NULL;

/* --- initialisation ------------------------------------------------------- */

static CFDictionaryRef
match_one(CFStringRef key, CFTypeRef value)
{
	return (CFDictionaryCreate(kCFAllocatorDefault, (const void **)&key,
	    (const void **)&value, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks));
}

static CFArrayRef
watch_one(CFStringRef key)
{
	return (CFArrayCreate(kCFAllocatorDefault, (const void **)&key, 1,
	    &kCFTypeArrayCallBacks));
}

static void
__DAInitialize(void)
{
	CFNumberRef zero;
	SInt64 value = 0;

	_DADiskInitialize();
	_DASessionInitialize();

	zero = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
	kDADiskDescriptionMatchMediaUnformatted = match_one(
	    kDADiskDescriptionMediaSizeKey, zero);
	CFRelease(zero);
	kDADiskDescriptionMatchMediaWhole = match_one(
	    kDADiskDescriptionMediaWholeKey, kCFBooleanTrue);
	kDADiskDescriptionMatchVolumeMountable = match_one(
	    kDADiskDescriptionVolumeMountableKey, kCFBooleanTrue);
	kDADiskDescriptionMatchVolumeUnrecognized = match_one(
	    kDADiskDescriptionVolumeMountableKey, kCFBooleanFalse);
	kDADiskDescriptionWatchVolumeName = watch_one(
	    kDADiskDescriptionVolumeNameKey);
	kDADiskDescriptionWatchVolumePath = watch_one(
	    kDADiskDescriptionVolumePathKey);
}

void
_DAInitialize(void)
{
	static pthread_once_t initialize = PTHREAD_ONCE_INIT;

	(void)pthread_once(&initialize, __DAInitialize);
}

/* --- requests and responses ----------------------------------------------- */

static DAReturn
__DAQueueRequest(DASessionRef session, _DARequestKind kind,
    DADiskRef argument0, CFIndex argument1, CFTypeRef argument2,
    CFTypeRef argument3, void *address, void *context)
{
	CFMutableDictionaryRef callback;
	CFDataRef _argument2 = NULL, _argument3 = NULL;
	DAReturn status;
	SInt32 index;

	if (session == NULL || argument0 == NULL)
		return (kDAReturnBadArgument);
	DA_SESSION_REVIVE(session, return (kDAReturnBadArgument));
	if (argument2 != NULL)
		_argument2 = _DASerialize(kCFAllocatorDefault, argument2);
	if (argument3 != NULL)
		_argument3 = _DASerialize(kCFAllocatorDefault, argument3);

	/*
	 * The callback stays here; the daemon only gets its table index
	 * (Apple: "for security reasons").
	 */
	callback = DACallbackCreate(kCFAllocatorDefault,
	    (mach_vm_offset_t)(uintptr_t)address,
	    (mach_vm_offset_t)(uintptr_t)context, UINT32_MAX, 0, NULL, NULL);
	index = DAAddCallbackToSession(session, callback);
	if (callback != NULL)
		CFRelease(callback);

	status = _DAServerSessionQueueRequest(_DASessionGetID(session),
	    (uint32_t)kind, _DADiskGetID(argument0), (int32_t)argument1,
	    (vm_address_t)(_argument2 != NULL ? CFDataGetBytePtr(_argument2) :
	    0),
	    (mach_msg_type_number_t)(_argument2 != NULL ?
	    CFDataGetLength(_argument2) : 0),
	    (vm_address_t)(_argument3 != NULL ? CFDataGetBytePtr(_argument3) :
	    0),
	    (mach_msg_type_number_t)(_argument3 != NULL ?
	    CFDataGetLength(_argument3) : 0),
	    (mach_vm_offset_t)index, (mach_vm_offset_t)index);
	if (status != kDAReturnSuccess)
		DARemoveCallbackFromSessionWithKey(session, index);

	if (_argument2 != NULL)
		CFRelease(_argument2);
	if (_argument3 != NULL)
		CFRelease(_argument3);
	return (status);
}

static void
__DAQueueResponse(DASessionRef session, SInt32 index, _DACallbackKind kind,
    DADiskRef disk, CFTypeRef response, SInt32 responseID)
{
	CFDataRef _response = NULL;

	DA_SESSION_REVIVE(session, return);
	if (response != NULL)
		_response = _DASerialize(kCFAllocatorDefault, response);
	(void)_DAServerSessionQueueResponse(_DASessionGetID(session),
	    (mach_vm_offset_t)index, (mach_vm_offset_t)index, (uint32_t)kind,
	    disk != NULL ? _DADiskGetID(disk) : "",
	    (vm_address_t)(_response != NULL ? CFDataGetBytePtr(_response) : 0),
	    (mach_msg_type_number_t)(_response != NULL ?
	    CFDataGetLength(_response) : 0), (int32_t)responseID);
	if (_response != NULL)
		CFRelease(_response);
}

/*
 * Apple's _DAAuthorize with _kDAAuthorizeOptionIsOwner and no
 * Authorization Services: root, or the disk's owner.
 */
static DAReturn
_DAAuthorize(DADiskRef disk)
{
	uid_t diskUID = 0;
	uid_t euid = geteuid();
	DAReturn status;

	if (euid == 0)
		return (kDAReturnSuccess);
	DA_SESSION_REVIVE(_DADiskGetSession(disk), return (kDAReturnBadArgument));
	status = _DAServerDiskGetUserUID(_DADiskGetSessionID(disk),
	    _DADiskGetID(disk), &diskUID);
	if (status != kDAReturnSuccess)
		return (status);
	return (diskUID == euid ? kDAReturnSuccess : kDAReturnNotPrivileged);
}

/* A request the daemon never took: answer the caller with its status. */
static void
__DARequestFailed(DADiskRef disk, DAReturn status, void *callback,
    void *context)
{
	DADissenterRef dissenter;

	if (callback == NULL)
		return;
	dissenter = DADissenterCreate(kCFAllocatorDefault, status, NULL);
	((void (*)(DADiskRef, DADissenterRef, void *))callback)(disk, dissenter,
	    context);
	if (dissenter != NULL)
		CFRelease(dissenter);
}

static bool
DARequestKindWithResponse(_DARequestKind kind)
{
	switch (kind) {
	case _kDADiskClaimCallback:
	case _kDADiskEjectCallback:
	case _kDADiskMountCallback:
	case _kDADiskProbeCallback:
	case _kDADiskRenameCallback:
	case _kDADiskUnmountCallback:
	case _kDADiskSetFSKitAdditionsCallback:
		return (true);
	default:
		return (false);
	}
}

void
_DADispatchCallback(DASessionRef session, void *address,
    void *context __unused, _DACallbackKind kind, CFTypeRef argument0,
    CFTypeRef argument1)
{
	CFDictionaryRef callback;
	CFTypeRef response = NULL;
	DADiskRef disk = NULL;
	SInt32 index = (SInt32)(intptr_t)address;
	void *function;
	void *data;

	if (argument0 != NULL)
		disk = _DADiskCreateFromSerialization(CFGetAllocator(session),
		    session, argument0);

	/* `address` is the index of the callback in the session's table. */
	callback = DACopyCallbackFromSession(session, index);
	if (callback == NULL)
		goto exit;
	function = (void *)(uintptr_t)___CFDictionaryGetIntegerValue(callback,
	    _kDACallbackAddressKey);
	data = (void *)(uintptr_t)___CFDictionaryGetIntegerValue(callback,
	    _kDACallbackContextKey);
	CFRelease(callback);
	if (function == NULL)
		goto exit;

	switch (kind) {
	case _kDADiskAppearedCallback:
		if (disk != NULL)
			((DADiskAppearedCallback)function)(disk, data);
		break;
	case _kDADiskClaimCallback:
		if (disk != NULL)
			((DADiskClaimCallback)function)(disk,
			    (DADissenterRef)argument1, data);
		break;
	case _kDADiskClaimReleaseCallback:
		if (disk != NULL)
			response = ((DADiskClaimReleaseCallback)function)(disk,
			    data);
		response = response != NULL ? response : kCFNull;
		break;
	case _kDADiskDescriptionChangedCallback:
		if (disk != NULL && argument1 != NULL)
			((DADiskDescriptionChangedCallback)function)(disk,
			    (CFArrayRef)argument1, data);
		break;
	case _kDADiskDisappearedCallback:
		if (disk != NULL)
			((DADiskDisappearedCallback)function)(disk, data);
		break;
	case _kDADiskEjectCallback:
		if (disk != NULL)
			((DADiskEjectCallback)function)(disk,
			    (DADissenterRef)argument1, data);
		break;
	case _kDADiskEjectApprovalCallback:
		if (disk != NULL)
			response = ((DADiskEjectApprovalCallback)function)(disk,
			    data);
		response = response != NULL ? response : kCFNull;
		break;
	case _kDADiskMountCallback:
		if (disk != NULL)
			((DADiskMountCallback)function)(disk,
			    (DADissenterRef)argument1, data);
		break;
	case _kDADiskMountApprovalCallback:
		if (disk != NULL)
			response = ((DADiskMountApprovalCallback)function)(disk,
			    data);
		response = response != NULL ? response : kCFNull;
		break;
	case _kDADiskPeekCallback:
		if (disk != NULL)
			((DADiskPeekCallback)function)(disk, data);
		response = kCFNull;
		break;
	case _kDADiskRenameCallback:
		if (disk != NULL)
			((DADiskRenameCallback)function)(disk,
			    (DADissenterRef)argument1, data);
		break;
	case _kDADiskUnmountCallback:
		if (disk != NULL)
			((DADiskUnmountCallback)function)(disk,
			    (DADissenterRef)argument1, data);
		break;
	case _kDADiskUnmountApprovalCallback:
		if (disk != NULL)
			response = ((DADiskUnmountApprovalCallback)function)(
			    disk, data);
		response = response != NULL ? response : kCFNull;
		break;
	case _kDAIdleCallback:
		((DAIdleCallback)function)(data);
		break;
	case _kDADiskListCompleteCallback:
		((DADiskListCompleteCallback)function)(data);
		break;
	default:
		break;
	}

	if (response != NULL) {
		/* argument1 carried the response ID the daemon waits for. */
		__DAQueueResponse(session, index, kind, disk,
		    response == kCFNull ? NULL : response,
		    (SInt32)___CFNumberGetIntegerValue(argument1));
		if (response != kCFNull)
			CFRelease(response);
	}
exit:
	/* A request's completion entry is used once. */
	if (DARequestKindWithResponse(kind))
		DARemoveCallbackFromSessionWithKey(session, index);
	if (disk != NULL)
		CFRelease(disk);
}

/* --- registration --------------------------------------------------------- */

static void
_DARegisterCallback(DASessionRef session, void *address, void *context,
    _DACallbackKind kind, CFIndex order, CFDictionaryRef match,
    CFArrayRef watch)
{
	CFMutableDictionaryRef callback;
	CFDataRef _match = NULL, _watch = NULL;
	SInt32 index;

	if (session == NULL)
		return;
	DA_SESSION_REVIVE(session, return);
	if (match != NULL)
		_match = _DASerializeDiskDescription(kCFAllocatorDefault, match);
	if (watch != NULL)
		_watch = _DASerialize(kCFAllocatorDefault, watch);
	callback = DACallbackCreate(kCFAllocatorDefault,
	    (mach_vm_offset_t)(uintptr_t)address,
	    (mach_vm_offset_t)(uintptr_t)context, kind, order, _match, _watch);
	index = DAAddCallbackToSession(session, callback);
	if (callback != NULL)
		CFRelease(callback);
	(void)_DAServerSessionRegisterCallback(_DASessionGetID(session),
	    (mach_vm_offset_t)index, (mach_vm_offset_t)index, (uint32_t)kind,
	    (int32_t)order,
	    (vm_address_t)(_match != NULL ? CFDataGetBytePtr(_match) : 0),
	    (mach_msg_type_number_t)(_match != NULL ? CFDataGetLength(_match) :
	    0),
	    (vm_address_t)(_watch != NULL ? CFDataGetBytePtr(_watch) : 0),
	    (mach_msg_type_number_t)(_watch != NULL ? CFDataGetLength(_watch) :
	    0));
	if (_match != NULL)
		CFRelease(_match);
	if (_watch != NULL)
		CFRelease(_watch);
}

static void
_DAUnregisterCallback(DASessionRef session, void *address, void *context)
{
	SInt32 index;

	if (session == NULL)
		return;
	DA_SESSION_REVIVE(session, return);
	index = DARemoveCallbackFromSession(session,
	    (mach_vm_offset_t)(uintptr_t)address,
	    (mach_vm_offset_t)(uintptr_t)context);
	if (index != 0)
		(void)_DAServerSessionUnregisterCallback(
		    _DASessionGetID(session), (mach_vm_offset_t)index,
		    (mach_vm_offset_t)index);
}

void
DARegisterDiskAppearedCallback(DASessionRef session, CFDictionaryRef match,
    DADiskAppearedCallback callback, void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskAppearedCallback, 0, match, NULL);
}

void
DARegisterDiskDescriptionChangedCallback(DASessionRef session,
    CFDictionaryRef match, CFArrayRef watch,
    DADiskDescriptionChangedCallback callback, void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskDescriptionChangedCallback, 0, match, watch);
}

void
DARegisterDiskDisappearedCallback(DASessionRef session, CFDictionaryRef match,
    DADiskDisappearedCallback callback, void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskDisappearedCallback, 0, match, NULL);
}

void
DARegisterDiskEjectApprovalCallback(DASessionRef session,
    CFDictionaryRef match, DADiskEjectApprovalCallback callback,
    void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskEjectApprovalCallback, 0, match, NULL);
}

void
DARegisterDiskPeekCallback(DASessionRef session, CFDictionaryRef match,
    CFIndex order, DADiskPeekCallback callback, void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskPeekCallback, order, match, NULL);
}

void
DARegisterDiskMountApprovalCallback(DASessionRef session,
    CFDictionaryRef match, DADiskMountApprovalCallback callback,
    void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskMountApprovalCallback, 0, match, NULL);
}

void
DARegisterDiskUnmountApprovalCallback(DASessionRef session,
    CFDictionaryRef match, DADiskUnmountApprovalCallback callback,
    void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskUnmountApprovalCallback, 0, match, NULL);
}

void
DARegisterIdleCallback(DASessionRef session, DAIdleCallback callback,
    void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDAIdleCallback, 0, NULL, NULL);
}

void
DARegisterDiskListCompleteCallback(DASessionRef session,
    DADiskListCompleteCallback callback, void *context)
{
	_DARegisterCallback(session, (void *)callback, context,
	    _kDADiskListCompleteCallback, 0, NULL, NULL);
}

void
DAUnregisterCallback(DASessionRef session, void *callback, void *context)
{
	_DAUnregisterCallback(session, callback, context);
}

void
DAUnregisterApprovalCallback(DASessionRef session, void *callback,
    void *context)
{
	_DAUnregisterCallback(session, callback, context);
}

/* --- disk requests -------------------------------------------------------- */

void
DADiskClaim(DADiskRef disk, DADiskClaimOptions options,
    DADiskClaimReleaseCallback release, void *releaseContext,
    DADiskClaimCallback callback, void *callbackContext)
{
	CFNumberRef _release = NULL, _releaseContext = NULL;
	CFMutableDictionaryRef releaseCallback;
	DAReturn status = kDAReturnBadArgument;
	SInt32 index;

	if (disk != NULL) {
		releaseCallback = DACallbackCreate(kCFAllocatorDefault,
		    (mach_vm_offset_t)(uintptr_t)release,
		    (mach_vm_offset_t)(uintptr_t)releaseContext, UINT32_MAX, 0,
		    NULL, NULL);
		index = DAAddCallbackToSession(_DADiskGetSession(disk),
		    releaseCallback);
		if (releaseCallback != NULL)
			CFRelease(releaseCallback);
		_release = ___CFNumberCreateWithIntegerValue(kCFAllocatorDefault,
		    index);
		_releaseContext = ___CFNumberCreateWithIntegerValue(
		    kCFAllocatorDefault, index);
		status = __DAQueueRequest(_DADiskGetSession(disk), _kDADiskClaim,
		    disk, options, _release, _releaseContext, (void *)callback,
		    callbackContext);
		if (status != kDAReturnSuccess)
			DARemoveCallbackFromSessionWithKey(
			    _DADiskGetSession(disk), index);
		if (_release != NULL)
			CFRelease(_release);
		if (_releaseContext != NULL)
			CFRelease(_releaseContext);
	}
	if (status != kDAReturnSuccess)
		__DARequestFailed(disk, status, (void *)callback,
		    callbackContext);
}

void
DADiskEject(DADiskRef disk, DADiskEjectOptions options,
    DADiskEjectCallback callback, void *context)
{
	DAReturn status = kDAReturnBadArgument;

	if (disk != NULL) {
		status = _DAAuthorize(disk);
		if (status == kDAReturnSuccess)
			status = __DAQueueRequest(_DADiskGetSession(disk),
			    _kDADiskEject, disk, options, NULL, NULL,
			    (void *)callback, context);
	}
	if (status != kDAReturnSuccess)
		__DARequestFailed(disk, status, (void *)callback, context);
}

DADiskOptions
DADiskGetOptions(DADiskRef disk)
{
	int32_t options = kDADiskOptionDefault;

	if (disk != NULL) {
		DA_SESSION_REVIVE(_DADiskGetSession(disk), return (options));
		(void)_DAServerDiskGetOptions(_DADiskGetSessionID(disk),
		    _DADiskGetID(disk), &options);
	}
	return ((DADiskOptions)options);
}

Boolean
DADiskIsClaimed(DADiskRef disk)
{
	boolean_t claimed = FALSE;

	if (disk != NULL) {
		DA_SESSION_REVIVE(_DADiskGetSession(disk), return (FALSE));
		(void)_DAServerDiskIsClaimed(_DADiskGetSessionID(disk),
		    _DADiskGetID(disk), &claimed);
	}
	return (claimed ? TRUE : FALSE);
}

void
DADiskMount(DADiskRef disk, CFURLRef path, DADiskMountOptions options,
    DADiskMountCallback callback, void *context)
{
	DADiskMountWithArguments(disk, path, options, callback, context, NULL);
}

void
DADiskMountWithArguments(DADiskRef disk, CFURLRef path,
    DADiskMountOptions options, DADiskMountCallback callback, void *context,
    CFStringRef arguments[])
{
	CFMutableStringRef argument = NULL;
	CFURLRef absolute = NULL;
	DAReturn status = kDAReturnBadArgument;
	CFIndex index;

	/* The arguments, comma-separated, as Apple sends them. */
	if (arguments != NULL && arguments[0] != NULL) {
		argument = CFStringCreateMutableCopy(kCFAllocatorDefault, 0,
		    arguments[0]);
		for (index = 1; argument != NULL && arguments[index] != NULL;
		    index++) {
			CFStringAppend(argument, CFSTR(","));
			CFStringAppend(argument, arguments[index]);
		}
	}
	if (path != NULL) {
		absolute = CFURLCopyAbsoluteURL(path);
		if (absolute == NULL)
			absolute = CFRetain(path);
	}
	if (disk != NULL) {
		status = _DAAuthorize(disk);
		if (status == kDAReturnSuccess)
			status = __DAQueueRequest(_DADiskGetSession(disk),
			    _kDADiskMount, disk, options,
			    absolute != NULL ? CFURLGetString(absolute) : NULL,
			    argument, (void *)callback, context);
	}
	if (argument != NULL)
		CFRelease(argument);
	if (absolute != NULL)
		CFRelease(absolute);
	if (status != kDAReturnSuccess)
		__DARequestFailed(disk, status, (void *)callback, context);
}

void
DADiskRename(DADiskRef disk, CFStringRef name, DADiskRenameOptions options,
    DADiskRenameCallback callback, void *context)
{
	DAReturn status = kDAReturnBadArgument;

	if (disk != NULL && name != NULL &&
	    CFGetTypeID(name) == CFStringGetTypeID()) {
		status = _DAAuthorize(disk);
		if (status == kDAReturnSuccess)
			status = __DAQueueRequest(_DADiskGetSession(disk),
			    _kDADiskRename, disk, options, name, NULL,
			    (void *)callback, context);
	}
	if (status != kDAReturnSuccess)
		__DARequestFailed(disk, status, (void *)callback, context);
}

DAReturn
DADiskSetOptions(DADiskRef disk, DADiskOptions options, Boolean value)
{
	if (disk == NULL)
		return (kDAReturnBadArgument);
	DA_SESSION_REVIVE(_DADiskGetSession(disk), return (kDAReturnBadArgument));
	return (_DAServerDiskSetOptions(_DADiskGetSessionID(disk),
	    _DADiskGetID(disk), (int32_t)options, value));
}

void
DADiskUnmount(DADiskRef disk, DADiskUnmountOptions options,
    DADiskUnmountCallback callback, void *context)
{
	DAReturn status = kDAReturnBadArgument;

	if (disk != NULL) {
		status = _DAAuthorize(disk);
		if (status == kDAReturnSuccess)
			status = __DAQueueRequest(_DADiskGetSession(disk),
			    _kDADiskUnmount, disk, options, NULL, NULL,
			    (void *)callback, context);
	}
	if (status != kDAReturnSuccess)
		__DARequestFailed(disk, status, (void *)callback, context);
}

void
DADiskUnclaim(DADiskRef disk)
{
	if (disk == NULL)
		return;
	DA_SESSION_REVIVE(_DADiskGetSession(disk), return);
	(void)_DAServerDiskUnclaim(_DADiskGetSessionID(disk),
	    _DADiskGetID(disk));
}
