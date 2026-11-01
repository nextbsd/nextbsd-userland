/*
 * DAInternal.c — serialization and helpers libDiskArbitration and
 * diskarbitrationd share (nextbsd-userland#197).
 *
 * Port of the shared half of Apple's diskarbitrationd/DAInternal.c
 * (DiskArbitration-535.120.1.0.1), which Apple's framework compiles too.
 * Apple writes binary property lists with the private
 * __CFBinaryPlistWriteToStream; NextBSD's CoreFoundation has the public
 * CFPropertyListCreateData, which writes the same format.
 */
#include "DAInternal.h"

#include <stdlib.h>
#include <string.h>

/* The three description keys whose values a property list cannot hold. */
#define DA_KEY_MEDIA_UUID	CFSTR("DAMediaUUID")
#define DA_KEY_VOLUME_PATH	CFSTR("DAVolumePath")
#define DA_KEY_VOLUME_UUID	CFSTR("DAVolumeUUID")

static const char *__kDAKindNameList[] = {
	"disk appeared",
	"disk claim",
	"disk claim release",
	"disk description changed",
	"disk disappeared",
	"disk eject",
	"disk eject approval",
	"disk mount",
	"disk mount approval",
	"disk peek",
	"disk probe",
	"disk refresh",
	"disk rename",
	"disk unmount",
	"disk unmount approval",
	"idle",
	"disk list complete",
	"disk fskit additions changed",
};

vm_address_t
___CFDataCopyBytes(CFDataRef data, mach_msg_type_number_t *length)
{
	vm_address_t bytes = 0;

	*length = (mach_msg_type_number_t)CFDataGetLength(data);
	if (vm_allocate(mach_task_self(), &bytes, *length, TRUE) !=
	    KERN_SUCCESS)
		return (0);
	if (bytes != 0)
		memcpy((void *)bytes, CFDataGetBytePtr(data), *length);
	return (bytes);
}

SInt64
___CFDictionaryGetIntegerValue(CFDictionaryRef dictionary, const void *key)
{
	CFNumberRef number;
	SInt64 value = 0;

	number = CFDictionaryGetValue(dictionary, key);
	if (number != NULL && CFGetTypeID(number) == CFNumberGetTypeID())
		(void)CFNumberGetValue(number, kCFNumberSInt64Type, &value);
	return (value);
}

void
___CFDictionarySetIntegerValue(CFMutableDictionaryRef dictionary,
    const void *key, SInt64 value)
{
	CFNumberRef number;

	number = CFNumberCreate(CFGetAllocator(dictionary), kCFNumberSInt64Type,
	    &value);
	if (number != NULL) {
		CFDictionarySetValue(dictionary, key, number);
		CFRelease(number);
	}
}

CFNumberRef
___CFNumberCreateWithIntegerValue(CFAllocatorRef allocator, SInt64 value)
{
	return (CFNumberCreate(allocator, kCFNumberSInt64Type, &value));
}

SInt64
___CFNumberGetIntegerValue(CFNumberRef number)
{
	SInt64 value = 0;

	if (number != NULL && CFGetTypeID(number) == CFNumberGetTypeID())
		(void)CFNumberGetValue(number, kCFNumberSInt64Type, &value);
	return (value);
}

const char *
_DACallbackKindGetName(_DACallbackKind kind)
{
	if (kind <= _kDADiskLastKind)
		return (__kDAKindNameList[kind]);
	return ("Unknown Kind");
}

CFDataRef
_DASerialize(CFAllocatorRef allocator, CFTypeRef object)
{
	return (CFPropertyListCreateData(allocator, object,
	    kCFPropertyListBinaryFormat_v1_0, 0, NULL));
}

/* Replace `key`'s value in `copy` by fn(value), if the key is present. */
static void
convert(CFMutableDictionaryRef copy, CFStringRef key,
    CFTypeRef (*fn)(CFAllocatorRef, CFTypeRef), CFAllocatorRef allocator)
{
	CFTypeRef object, converted;

	object = CFDictionaryGetValue(copy, key);
	if (object == NULL)
		return;
	converted = fn(allocator, object);
	if (converted != NULL) {
		CFDictionarySetValue(copy, key, converted);
		CFRelease(converted);
	}
}

static CFTypeRef
uuid_to_string(CFAllocatorRef allocator, CFTypeRef object)
{
	if (CFGetTypeID(object) != CFUUIDGetTypeID())
		return (NULL);
	return (CFUUIDCreateString(allocator, (CFUUIDRef)object));
}

static CFTypeRef
string_to_uuid(CFAllocatorRef allocator, CFTypeRef object)
{
	if (CFGetTypeID(object) != CFStringGetTypeID())
		return (NULL);
	return (CFUUIDCreateFromString(allocator, (CFStringRef)object));
}

static CFTypeRef
url_to_path(CFAllocatorRef allocator __unused, CFTypeRef object)
{
	if (CFGetTypeID(object) != CFURLGetTypeID())
		return (NULL);
	return (CFURLCopyFileSystemPath((CFURLRef)object, kCFURLPOSIXPathStyle));
}

static CFTypeRef
path_to_url(CFAllocatorRef allocator, CFTypeRef object)
{
	if (CFGetTypeID(object) != CFStringGetTypeID())
		return (NULL);
	return (CFURLCreateWithFileSystemPath(allocator, (CFStringRef)object,
	    kCFURLPOSIXPathStyle, TRUE));
}

CFDataRef
_DASerializeDiskDescription(CFAllocatorRef allocator,
    CFDictionaryRef description)
{
	CFMutableDictionaryRef copy;
	CFDataRef data;

	if (description == NULL)
		return (NULL);
	copy = CFDictionaryCreateMutableCopy(allocator, 0, description);
	if (copy == NULL)
		return (NULL);
	convert(copy, DA_KEY_MEDIA_UUID, uuid_to_string, allocator);
	convert(copy, DA_KEY_VOLUME_PATH, url_to_path, allocator);
	convert(copy, DA_KEY_VOLUME_UUID, uuid_to_string, allocator);
	data = _DASerialize(allocator, copy);
	CFRelease(copy);
	return (data);
}

CFTypeRef
_DAUnserialize(CFAllocatorRef allocator, CFDataRef data)
{
	return (CFPropertyListCreateWithData(allocator, data,
	    kCFPropertyListImmutable, NULL, NULL));
}

CFMutableDictionaryRef
_DAUnserializeDiskDescription(CFAllocatorRef allocator, CFDataRef data)
{
	CFMutableDictionaryRef description;

	description = (CFMutableDictionaryRef)CFPropertyListCreateWithData(
	    allocator, data, kCFPropertyListMutableContainers, NULL, NULL);
	if (description == NULL)
		return (NULL);
	if (CFGetTypeID(description) != CFDictionaryGetTypeID()) {
		CFRelease(description);
		return (NULL);
	}
	convert(description, DA_KEY_MEDIA_UUID, string_to_uuid, allocator);
	convert(description, DA_KEY_VOLUME_PATH, path_to_url, allocator);
	convert(description, DA_KEY_VOLUME_UUID, string_to_uuid, allocator);
	return (description);
}

CFMutableDictionaryRef
_DAUnserializeDiskDescriptionWithBytes(CFAllocatorRef allocator,
    vm_address_t bytes, vm_size_t length)
{
	CFMutableDictionaryRef description = NULL;
	CFDataRef data;

	if (bytes == 0)
		return (NULL);
	data = CFDataCreateWithBytesNoCopy(allocator, (const UInt8 *)bytes,
	    (CFIndex)length, kCFAllocatorNull);
	if (data != NULL) {
		description = _DAUnserializeDiskDescription(allocator, data);
		CFRelease(data);
	}
	return (description);
}

CFTypeRef
_DAUnserializeWithBytes(CFAllocatorRef allocator, vm_address_t bytes,
    vm_size_t length)
{
	CFTypeRef object = NULL;
	CFDataRef data;

	if (bytes == 0)
		return (NULL);
	data = CFDataCreateWithBytesNoCopy(allocator, (const UInt8 *)bytes,
	    (CFIndex)length, kCFAllocatorNull);
	if (data != NULL) {
		object = _DAUnserialize(allocator, data);
		CFRelease(data);
	}
	return (object);
}
