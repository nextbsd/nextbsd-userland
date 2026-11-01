/*
 * DADisk.c — libDiskArbitration's disk object (nextbsd-userland#197).
 *
 * Port of Apple's DiskArbitration/DADisk.c (DiskArbitration-535.120.1.0.1).
 * A DADisk is a disk ID ("/dev/vtbd0p2", or "<mount point>?owner=<uid>" for
 * a mount without a device) and the session to ask about it; a disk handed
 * to a callback also carries the description as of that event.
 *
 * NextBSD differences: no I/O Kit, so DADiskCreateFromIOMedia and
 * DADiskCopyIOMedia do not exist, and DADiskCopyWholeDisk asks the daemon
 * instead of walking the IOMedia parents (see there).
 */
#include "DAClient.h"
#include "DAServerUser.h"	/* MIG: _DAServer* user stubs */

#include <CoreFoundation/CFRuntime.h>

#include <sys/param.h>
#include <sys/mount.h>

#include <paths.h>
#include <stdlib.h>
#include <string.h>

struct __DADisk {
	CFRuntimeBase	 _base;
	CFDictionaryRef	 _description;
	char		*_device;
	char		*_id;
	DASessionRef	 _session;
};

typedef struct __DADisk __DADisk;

static CFTypeID __kDADiskTypeID = _kCFRuntimeNotATypeID;

static CFStringRef
__DADiskCopyDescription(CFTypeRef object)
{
	DADiskRef disk = (DADiskRef)object;

	return (CFStringCreateWithFormat(CFGetAllocator(object), NULL,
	    CFSTR("<DADisk %p [%p]>{id = %s}"), object, CFGetAllocator(object),
	    disk->_id));
}

static CFStringRef
__DADiskCopyFormattingDescription(CFTypeRef object,
    CFDictionaryRef options __unused)
{
	return (CFStringCreateWithFormat(CFGetAllocator(object), NULL,
	    CFSTR("%s"), ((DADiskRef)object)->_id));
}

static void
__DADiskDeallocate(CFTypeRef object)
{
	DADiskRef disk = (DADiskRef)object;

	if (disk->_description != NULL)
		CFRelease(disk->_description);
	free(disk->_device);
	free(disk->_id);
	if (disk->_session != NULL)
		CFRelease(disk->_session);
}

static Boolean
__DADiskEqual(CFTypeRef object1, CFTypeRef object2)
{
	return (strcmp(((DADiskRef)object1)->_id, ((DADiskRef)object2)->_id) ==
	    0);
}

static CFHashCode
__DADiskHash(CFTypeRef object)
{
	const char *id = ((DADiskRef)object)->_id;
	CFHashCode hash = 5381;
	size_t i, n = strlen(id);

	/* Apple hashes the first 16 bytes of the ID (CFHashBytes). */
	for (i = 0; i < n && i < 16; i++)
		hash = hash * 33 + (unsigned char)id[i];
	return (hash);
}

static const CFRuntimeClass __DADiskClass = {
	.version		= 0,
	.className		= "DADisk",
	.finalize		= __DADiskDeallocate,
	.equal			= __DADiskEqual,
	.hash			= __DADiskHash,
	.copyFormattingDesc	= __DADiskCopyFormattingDescription,
	.copyDebugDesc		= __DADiskCopyDescription,
};

void
_DADiskInitialize(void)
{
	__kDADiskTypeID = _CFRuntimeRegisterClass(&__DADiskClass);
}

static DADiskRef
__DADiskCreate(CFAllocatorRef allocator, DASessionRef session, const char *id)
{
	__DADisk *disk;

	disk = (void *)_CFRuntimeCreateInstance(allocator, __kDADiskTypeID,
	    sizeof(__DADisk) - sizeof(CFRuntimeBase), NULL);
	if (disk == NULL)
		return (NULL);
	disk->_description = NULL;
	disk->_device = NULL;
	disk->_id = strdup(id);
	disk->_session = (DASessionRef)CFRetain(session);
	if (disk->_id == NULL) {
		CFRelease(disk);
		return (NULL);
	}
	return (disk);
}

static DADiskRef
_DADiskCreate(CFAllocatorRef allocator, DASessionRef session, const char *id)
{
	DADiskRef disk;

	if (session == NULL)
		return (NULL);
	disk = __DADiskCreate(allocator, session, id);
	if (disk != NULL && strncmp(id, _PATH_DEV, strlen(_PATH_DEV)) == 0)
		disk->_device = strdup(id + strlen(_PATH_DEV));
	return (disk);
}

DADiskRef
_DADiskCreateFromSerialization(CFAllocatorRef allocator, DASessionRef session,
    CFDataRef serialization)
{
	CFMutableDictionaryRef description;
	CFDataRef data;
	DADiskRef disk = NULL;
	const char *id;

	if (serialization == NULL ||
	    CFGetTypeID(serialization) != CFDataGetTypeID())
		return (NULL);
	description = _DAUnserializeDiskDescription(allocator, serialization);
	if (description == NULL)
		return (NULL);
	data = CFDictionaryGetValue(description, _kDADiskIDKey);
	if (data != NULL && CFGetTypeID(data) == CFDataGetTypeID() &&
	    CFDataGetLength(data) > 0) {
		id = (const char *)CFDataGetBytePtr(data);
		if (id[CFDataGetLength(data) - 1] == '\0')
			disk = _DADiskCreate(allocator, session, id);
		if (disk != NULL) {
			CFDictionaryRemoveValue(description, _kDADiskIDKey);
			disk->_description = CFRetain(description);
		}
	}
	CFRelease(description);
	return (disk);
}

char *
_DADiskGetID(DADiskRef disk)
{
	return (disk->_id);
}

DASessionRef
_DADiskGetSession(DADiskRef disk)
{
	return (disk->_session);
}

mach_port_t
_DADiskGetSessionID(DADiskRef disk)
{
	return (_DASessionGetID(disk->_session));
}

CFDictionaryRef
DADiskCopyDescription(DADiskRef disk)
{
	CFMutableDictionaryRef description;
	vm_address_t _description = 0;
	mach_msg_type_number_t _descriptionSize = 0;

	if (disk == NULL)
		return (NULL);
	if (disk->_description != NULL)
		return (CFRetain(disk->_description));
	DA_SESSION_REVIVE(disk->_session, return (NULL));
	if (_DAServerDiskCopyDescription(_DADiskGetSessionID(disk), disk->_id,
	    &_description, &_descriptionSize) != KERN_SUCCESS)
		return (NULL);
	description = _DAUnserializeDiskDescriptionWithBytes(
	    CFGetAllocator(disk), _description, _descriptionSize);
	if (description != NULL)
		CFDictionaryRemoveValue(description, _kDADiskIDKey);
	(void)vm_deallocate(mach_task_self(), _description, _descriptionSize);
	return (description);
}

/*
 * Apple walks the IOMedia's parents to the one with Whole set. GEOM names
 * a partition after its whole disk (vtbd0p2, da0s1a, ada0p1), so ask the
 * daemon for each ever shorter prefix of the BSD name until one describes a
 * whole disk.
 */
DADiskRef
DADiskCopyWholeDisk(DADiskRef disk)
{
	CFDictionaryRef description;
	char name[MAXPATHLEN];
	size_t len;
	DADiskRef whole;

	if (disk == NULL)
		return (NULL);
	description = DADiskCopyDescription(disk);
	if (description == NULL)
		return (NULL);
	if (CFDictionaryGetValue(description,
	    kDADiskDescriptionMediaWholeKey) == kCFBooleanTrue) {
		CFRelease(description);
		return (_DADiskCreate(CFGetAllocator(disk), disk->_session,
		    disk->_id));
	}
	CFRelease(description);
	if (disk->_device == NULL)
		return (NULL);
	(void)strlcpy(name, disk->_device, sizeof(name));
	for (len = strlen(name) - 1; len > 0; len--) {
		name[len] = '\0';
		whole = DADiskCreateFromBSDName(CFGetAllocator(disk),
		    disk->_session, name);
		if (whole == NULL)
			continue;
		description = DADiskCopyDescription(whole);
		if (description != NULL) {
			Boolean is_whole = CFDictionaryGetValue(description,
			    kDADiskDescriptionMediaWholeKey) == kCFBooleanTrue;

			CFRelease(description);
			if (is_whole)
				return (whole);
		}
		CFRelease(whole);
	}
	return (NULL);
}

DADiskRef
DADiskCreateFromBSDName(CFAllocatorRef allocator, DASessionRef session,
    const char *name)
{
	char id[MAXPATHLEN];

	if (name == NULL)
		return (NULL);
	if (strncmp(name, _PATH_DEV, strlen(_PATH_DEV)) != 0) {
		(void)strlcpy(id, _PATH_DEV, sizeof(id));
		(void)strlcat(id, name, sizeof(id));
	} else
		(void)strlcpy(id, name, sizeof(id));
	return (_DADiskCreate(allocator, session, id));
}

/* Apple's ___statfs: the mount on `path`, preferring one we own. */
static int
___statfs(const char *path, struct statfs *buf)
{
	struct statfs *list;
	int count, i, status = -1;

	count = getfsstat(NULL, 0, MNT_NOWAIT);
	if (count <= 0)
		return (-1);
	list = calloc((size_t)count, sizeof(*list));
	if (list == NULL)
		return (-1);
	count = getfsstat(list, (long)count * (long)sizeof(*list), MNT_NOWAIT);
	for (i = 0; i < count; i++) {
		if (strcmp(list[i].f_mntonname, path) != 0)
			continue;
		status = 0;
		*buf = list[i];
		if (list[i].f_owner == geteuid())
			break;
	}
	free(list);
	return (status);
}

DADiskRef
DADiskCreateFromVolumePath(CFAllocatorRef allocator, DASessionRef session,
    CFURLRef path)
{
	char _path[MAXPATHLEN], real[MAXPATHLEN], *id = NULL;
	struct statfs fs;
	DADiskRef disk = NULL;
	int status;

	if (path == NULL || !CFURLGetFileSystemRepresentation(path, TRUE,
	    (UInt8 *)_path, sizeof(_path)))
		return (NULL);
	status = ___statfs(_path, &fs);
	if (status != 0 && realpath(_path, real) != NULL)
		status = ___statfs(real, &fs);
	if (status != 0)
		return (NULL);
	/* Apple's _DAVolumeCopyID, as the daemon's DAVolume.c names mounts. */
	if (strncmp(fs.f_mntfromname, _PATH_DEV, strlen(_PATH_DEV)) == 0)
		id = strdup(fs.f_mntfromname);
	else if (asprintf(&id, "%s?owner=%u", fs.f_mntonname,
	    (unsigned)fs.f_owner) < 0)
		id = NULL;
	if (id != NULL) {
		disk = _DADiskCreate(allocator, session, id);
		free(id);
	}
	/*
	 * NextBSD mounts often name a GEOM label (/dev/ufs/ROOTFS,
	 * /dev/gpt/EFI). The daemon knows the disk by its provider; use that
	 * name, so DADiskGetBSDName and CFEqual agree with the disks callbacks
	 * hand out.
	 */
	if (disk != NULL && disk->_device != NULL &&
	    strchr(disk->_device, '/') != NULL) {
		CFDictionaryRef description;
		CFStringRef name;
		char bsd[MAXPATHLEN];
		DADiskRef media;

		description = DADiskCopyDescription(disk);
		name = description != NULL ? CFDictionaryGetValue(description,
		    kDADiskDescriptionMediaBSDNameKey) : NULL;
		if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID() &&
		    CFStringGetCString(name, bsd, sizeof(bsd),
		    kCFStringEncodingUTF8)) {
			media = DADiskCreateFromBSDName(allocator, session, bsd);
			if (media != NULL) {
				CFRelease(disk);
				disk = media;
			}
		}
		if (description != NULL)
			CFRelease(description);
	}
	return (disk);
}

const char *
DADiskGetBSDName(DADiskRef disk)
{
	return (disk != NULL ? disk->_device : NULL);
}

CFTypeID
DADiskGetTypeID(void)
{
	_DAInitialize();
	return (__kDADiskTypeID);
}
