/*
 * SCD.c — SCError() state and property-list serialization for the
 * SCDynamicStore client (freebsd-launchd-mach port of Apple's
 * SystemConfiguration.fproj SCD.c / SCDPrivate.c).
 *
 * Two concerns live here:
 *
 *  - SCError() / _SCErrorSet(): a per-thread last-error slot. Apple
 *    keeps it in __SCThreadSpecificData; this port keeps just the int,
 *    in a pthread-specific key.
 *
 *  - _SCSerialize* / _SCUnserialize: the byte<->CF conversions every
 *    config.defs RPC needs. A configd key is raw UTF-8; a configd
 *    value is an opaque serialized property list (configd never looks
 *    inside it). Apple serializes values as a binary plist; this port
 *    uses the XML plist format, which the repo's libCoreFoundation
 *    round-trips in its own test — the choice is purely internal to
 *    libSystemConfiguration, since the same library serializes and
 *    un-serializes and configd treats the blob as opaque.
 */

#include "SCInternal.h"
#include "config_wire.h"		/* wire_*_put — _SCAppendKey / _SCAppendKeyValue */

#include <pthread.h>
#include <stdlib.h>


#pragma mark -
#pragma mark SCError

static pthread_key_t	sc_error_key;
static pthread_once_t	sc_error_once = PTHREAD_ONCE_INIT;

static void
sc_error_key_init(void)
{
	(void) pthread_key_create(&sc_error_key, free);
}

void
_SCErrorSet(int error)
{
	int	*slot;

	(void) pthread_once(&sc_error_once, sc_error_key_init);
	slot = pthread_getspecific(sc_error_key);
	if (slot == NULL) {
		slot = malloc(sizeof(*slot));
		if (slot == NULL) {
			return;
		}
		(void) pthread_setspecific(sc_error_key, slot);
	}
	*slot = error;
}

int
SCError(void)
{
	int	*slot;

	(void) pthread_once(&sc_error_once, sc_error_key_init);
	slot = pthread_getspecific(sc_error_key);
	return (slot != NULL) ? *slot : kSCStatusOK;
}

const char *
SCErrorString(int status)
{
	switch (status) {
	case kSCStatusOK :
		return "Success!";
	case kSCStatusFailed :
		return "Failed!";
	case kSCStatusInvalidArgument :
		return "Invalid argument";
	case kSCStatusNoKey :
		return "No such key";
	case kSCStatusKeyExists :
		return "Key already defined";
	case kSCStatusNoStoreSession :
		return "Configuration daemon session not active";
	case kSCStatusNoStoreServer :
		return "Configuration daemon not (no longer) available";
	case kSCStatusNotifierActive :
		return "Notifier is currently active";
	case kSCStatusMaxLink :
		return "Maximum link count exceeded";
	case kSCStatusNoPrefsSession :
		return "Preferences session not active";
	case kSCStatusPrefsBusy :
		return "Preferences update currently in progress";
	case kSCStatusNoConfigFile :
		return "Configuration file not found";
	case kSCStatusStale :
		return "Write attempted on stale version of object";
	case kSCStatusAccessError :
		return "Permission denied";
	case kSCStatusLocked :
		return "Preferences update currently in progress";
	case kSCStatusNeedLock :
		return "Lock not held";
	default :
		return "Unknown error";
	}
}


#pragma mark -
#pragma mark Serialization

CFDataRef
_SCSerialize(CFPropertyListRef obj)
{
	CFDataRef	data;
	CFErrorRef	error	= NULL;

	if (obj == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	data = CFPropertyListCreateData(NULL,
					obj,
					kCFPropertyListXMLFormat_v1_0,
					0,
					&error);
	if (data == NULL) {
		if (error != NULL) {
			CFRelease(error);
		}
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	return data;
}

CFDataRef
_SCSerializeString(CFStringRef str)
{
	CFDataRef	data;

	if ((str == NULL) || (CFGetTypeID(str) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	data = CFStringCreateExternalRepresentation(NULL,
						    str,
						    kCFStringEncodingUTF8,
						    0);
	if (data == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	return data;
}

CFPropertyListRef
_SCUnserialize(const void *bytes, CFIndex len)
{
	CFErrorRef		error	= NULL;
	CFPropertyListRef	obj;
	CFDataRef		xml;

	/*
	 * The bytes came back out-of-line in a config.defs reply — wrap
	 * them without copying; CFPropertyList deep-copies whatever it
	 * parses, so the caller can vm_deallocate() the reply the moment
	 * this returns.
	 */
	xml = CFDataCreateWithBytesNoCopy(NULL, bytes, len, kCFAllocatorNull);
	if (xml == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	obj = CFPropertyListCreateWithData(NULL,
					   xml,
					   kCFPropertyListImmutable,
					   NULL,
					   &error);
	CFRelease(xml);

	if (obj == NULL) {
		if (error != NULL) {
			CFRelease(error);
		}
		_SCErrorSet(kSCStatusFailed);
	}

	return obj;
}

/*
 * Grow `buf` by `len` bytes and return the offset the new bytes start
 * at, or -1 if the payload would outgrow a Mach message count.
 */
static CFIndex
__SCGrow(CFMutableDataRef buf, size_t len)
{
	CFIndex	off	= CFDataGetLength(buf);

	if (len > (size_t)UINT32_MAX - (size_t)off) {
		return -1;
	}
	CFDataIncreaseLength(buf, (CFIndex)len);
	return off;
}

int
_SCAppendKey(CFMutableDataRef buf, const void *key, size_t klen)
{
	CFIndex	start	= __SCGrow(buf, sizeof(uint32_t) + klen);
	size_t	off;

	if (start < 0) {
		return -1;
	}
	off = (size_t)start;
	return wire_keylist_put(CFDataGetMutableBytePtr(buf),
				(size_t)CFDataGetLength(buf), &off, key, klen);
}

int
_SCAppendKeyValue(CFMutableDataRef buf, const void *key, size_t klen,
		  const void *val, size_t vlen)
{
	CFIndex	start;
	size_t	off;

	if (vlen > SIZE_MAX - 2 * sizeof(uint32_t) - klen) {
		return -1;
	}
	start = __SCGrow(buf, 2 * sizeof(uint32_t) + klen + vlen);
	if (start < 0) {
		return -1;
	}
	off = (size_t)start;
	return wire_kvmap_put(CFDataGetMutableBytePtr(buf),
			      (size_t)CFDataGetLength(buf), &off,
			      key, klen, val, vlen);
}

int
_SCEncodeKeyList(CFArrayRef array, CFMutableDataRef buf)
{
	CFIndex	i;
	CFIndex	n;

	if (array == NULL) {
		return 0;
	}
	if (CFGetTypeID(array) != CFArrayGetTypeID()) {
		return -1;
	}

	n = CFArrayGetCount(array);
	for (i = 0; i < n; i++) {
		CFStringRef	key	= CFArrayGetValueAtIndex(array, i);
		CFDataRef	keyData;
		int		rc;

		if ((key == NULL) || (CFGetTypeID(key) != CFStringGetTypeID())) {
			return -1;
		}
		keyData = CFStringCreateExternalRepresentation(NULL, key,
							       kCFStringEncodingUTF8,
							       0);
		if (keyData == NULL) {
			return -1;
		}
		rc = _SCAppendKey(buf, CFDataGetBytePtr(keyData),
				  (size_t)CFDataGetLength(keyData));
		CFRelease(keyData);
		if (rc != 0) {
			return -1;
		}
	}

	return 0;
}
