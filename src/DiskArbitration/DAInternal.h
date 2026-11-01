/*
 * DAInternal.h — what libDiskArbitration and diskarbitrationd share
 * (nextbsd-userland#197).
 *
 * The C counterpart of Apple's diskarbitrationd/DAInternal.h
 * (DiskArbitration-535.120.1.0.1), which both of Apple's targets compile
 * too: the Mach service name, the callback kinds (their numbers cross the
 * wire), the keys of a serialized callback, and the property-list
 * serialization of callback queues and disk descriptions. The library
 * builds DAInternal.c from this directory.
 *
 * Apple declares the keys as `const CFStringRef` variables; here they are
 * CFSTR() macros, as the daemon's kDADiskDescription* keys are (DADisk.h),
 * so neither side needs a file-scope initialiser.
 */
#ifndef DISKARBITRATION_DAINTERNAL_H
#define DISKARBITRATION_DAINTERNAL_H

#include <CoreFoundation/CoreFoundation.h>

#include <mach/mach.h>
#include <mach/error.h>

/* The Mach service (Apple's DAInternal.h _kDADaemonName). */
#define _kDADaemonName	"com.apple.DiskArbitration.diskarbitrationd"

/*
 * Posted by the daemon once it has checked in; a keep-alive session
 * re-creates itself when it sees it (Apple's DAMain.c / DASession.c).
 */
#define _kDADaemonLaunchedNotification	"com.apple.diskarbitrationd.launched"

/* Callback kinds, in Apple's order: the number is on the wire. */
enum {
	_kDADiskAppearedCallback,
	_kDADiskClaimCallback,
	_kDADiskClaimReleaseCallback,
	_kDADiskDescriptionChangedCallback,
	_kDADiskDisappearedCallback,
	_kDADiskEjectCallback,
	_kDADiskEjectApprovalCallback,
	_kDADiskMountCallback,
	_kDADiskMountApprovalCallback,
	_kDADiskPeekCallback,
	_kDADiskProbeCallback,
	_kDADiskRefreshCallback,
	_kDADiskRenameCallback,
	_kDADiskUnmountCallback,
	_kDADiskUnmountApprovalCallback,
	_kDAIdleCallback,
	_kDADiskListCompleteCallback,
	_kDADiskSetFSKitAdditionsCallback,
	_kDADiskLastKind = _kDADiskSetFSKitAdditionsCallback
};

typedef UInt32 _DACallbackKind;

/* Request kinds are the callback kinds of their completions. */
enum {
	_kDADiskClaim			= _kDADiskClaimCallback,
	_kDADiskEject			= _kDADiskEjectCallback,
	_kDADiskMount			= _kDADiskMountCallback,
	_kDADiskProbe			= _kDADiskProbeCallback,
	_kDADiskRefresh			= _kDADiskRefreshCallback,
	_kDADiskRename			= _kDADiskRenameCallback,
	_kDADiskUnmount			= _kDADiskUnmountCallback,
	_kDADiskSetFSKitAdditions	= _kDADiskSetFSKitAdditionsCallback
};

typedef UInt32 _DARequestKind;

/* Keys of a callback dictionary (Apple's DAInternal.c values). */
#define _kDACallbackAddressKey		CFSTR("DACallbackAddress")
#define _kDACallbackArgument0Key	CFSTR("DACallbackArgument0")
#define _kDACallbackArgument1Key	CFSTR("DACallbackArgument1")
#define _kDACallbackContextKey		CFSTR("DACallbackContext")
#define _kDACallbackKindKey		CFSTR("DACallbackKind")
#define _kDACallbackMatchKey		CFSTR("DACallbackMatch")
#define _kDACallbackOrderKey		CFSTR("DACallbackOrder")
#define _kDACallbackBlockKey		CFSTR("DACallbackBlock")
#define _kDACallbackTimeKey		CFSTR("DACallbackTime")
#define _kDACallbackWatchKey		CFSTR("DACallbackWatch")

/* The disk ID inside a serialized description (CFData, NUL-terminated). */
#define _kDADiskIDKey			CFSTR("DADiskID")

/* A dissenter is a dictionary with these keys. */
#define _kDADissenterProcessIDKey	CFSTR("DAProcessID")
#define _kDADissenterStatusKey		CFSTR("DAStatus")
#define _kDADissenterStatusStringKey	CFSTR("DAStatusString")

/* Apple's <mach/error.h> macro; libmach's header lacks it. */
#ifndef unix_err
#define unix_err(errno)	(err_kern | err_sub(3) | (errno))
#endif

/* Apple's ___CF helpers, as DAInternal.c has them. */
SInt64		___CFDictionaryGetIntegerValue(CFDictionaryRef dictionary,
		    const void *key);
void		___CFDictionarySetIntegerValue(CFMutableDictionaryRef dictionary,
		    const void *key, SInt64 value);
CFNumberRef	___CFNumberCreateWithIntegerValue(CFAllocatorRef allocator,
		    SInt64 value);
SInt64		___CFNumberGetIntegerValue(CFNumberRef number);
/* The bytes of `data` in new vm_allocate'd memory, for a MIG out-of-line. */
vm_address_t	___CFDataCopyBytes(CFDataRef data,
		    mach_msg_type_number_t *length);

const char	*_DACallbackKindGetName(_DACallbackKind kind);

/*
 * Binary property lists, as Apple's (which write them with the private
 * __CFBinaryPlistWriteToStream). A disk description travels with its CFUUID
 * values as strings and DAVolumePath as a POSIX path string, because a
 * property list cannot hold either type; the unserializer turns them back.
 */
CFDataRef	_DASerialize(CFAllocatorRef allocator, CFTypeRef object);
CFDataRef	_DASerializeDiskDescription(CFAllocatorRef allocator,
		    CFDictionaryRef description);
CFTypeRef	_DAUnserialize(CFAllocatorRef allocator, CFDataRef data);
CFMutableDictionaryRef _DAUnserializeDiskDescription(CFAllocatorRef allocator,
		    CFDataRef data);
CFMutableDictionaryRef _DAUnserializeDiskDescriptionWithBytes(
		    CFAllocatorRef allocator, vm_address_t bytes,
		    vm_size_t length);
CFTypeRef	_DAUnserializeWithBytes(CFAllocatorRef allocator,
		    vm_address_t bytes, vm_size_t length);

#endif /* DISKARBITRATION_DAINTERNAL_H */
