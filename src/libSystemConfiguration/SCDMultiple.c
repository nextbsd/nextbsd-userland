/*
 * SCDMultiple.c — the SCDynamicStore batch store calls
 * (freebsd-launchd-mach port of Apple's SystemConfiguration.fproj
 * SCDGet.c SCDynamicStoreCopyMultiple + SCDSet.c
 * SCDynamicStoreSetMultiple).
 *
 * One MIG round trip moves several keys at once: configget_m fetches
 * the values for a set of keys (and pattern-matched keys); configset_m
 * sets, removes and notifies several keys together.
 *
 * Apple serializes each batch argument as a property list; this port
 * carries them in configd's length-prefixed wire encodings instead
 * (config_wire.h) — a key list for the get keys / patterns / removes /
 * notifies, and a key/value map for the values to fetch or set. Values
 * inside the map are still XML-plist blobs (the SCD.c form).
 */

#include "SCInternal.h"
#include "config_wire.h"


#pragma mark -
#pragma mark Key/value-map encoding

/* CFDictionaryApplyFunction context for encode_kvmap() */
struct kvmap_encode_ctx {
	CFMutableDataRef	buf;
	int			err;
};

/* encode one key (CFString) / value (CFPropertyList) pair into the map */
static void
kvmap_encode_one(const void *key, const void *value, void *context)
{
	struct kvmap_encode_ctx	*ctx	= context;
	CFDataRef		keyData;
	CFDataRef		valueData;

	if (ctx->err) {
		return;
	}
	if ((key == NULL) || (CFGetTypeID(key) != CFStringGetTypeID())) {
		ctx->err = 1;
		return;
	}

	keyData = CFStringCreateExternalRepresentation(NULL, (CFStringRef)key,
						       kCFStringEncodingUTF8, 0);
	if (keyData == NULL) {
		ctx->err = 1;
		return;
	}
	valueData = _SCSerialize((CFPropertyListRef)value);
	if (valueData == NULL) {
		CFRelease(keyData);
		ctx->err = 1;
		return;
	}

	if (_SCAppendKeyValue(ctx->buf,
			      CFDataGetBytePtr(keyData),
			      (size_t)CFDataGetLength(keyData),
			      CFDataGetBytePtr(valueData),
			      (size_t)CFDataGetLength(valueData)) != 0) {
		ctx->err = 1;
	}

	CFRelease(keyData);
	CFRelease(valueData);
}

/*
 * Encode a CFDictionary of CFString -> CFPropertyList into configd's
 * wire key/value-map form, appending to `buf`. Returns 0, or -1 on a
 * bad element.
 */
static int
encode_kvmap(CFDictionaryRef dict, CFMutableDataRef buf)
{
	struct kvmap_encode_ctx	ctx;

	if (dict == NULL) {
		return 0;
	}
	if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) {
		return -1;
	}

	ctx.buf = buf;
	ctx.err = 0;
	CFDictionaryApplyFunction(dict, kvmap_encode_one, &ctx);
	return ctx.err ? -1 : 0;
}


#pragma mark -
#pragma mark Batch store access

CFDictionaryRef
SCDynamicStoreCopyMultiple(SCDynamicStoreRef	store,
			   CFArrayRef		keys,
			   CFArrayRef		patterns)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFMutableDataRef		keyBuf;
	CFMutableDataRef		patternBuf;
	xmlDataOut_t			reply		= NULL;
	mach_msg_type_number_t		replyLen	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;
	CFMutableDictionaryRef		result;
	const uint8_t			*cur;
	const uint8_t			*end;
	const void			*keyBytes;
	const void			*valueBytes;
	size_t				kl;
	size_t				vl;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return NULL;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return NULL;
	}

	keyBuf     = CFDataCreateMutable(NULL, 0);
	patternBuf = CFDataCreateMutable(NULL, 0);
	if ((keyBuf == NULL) || (patternBuf == NULL) ||
	    (_SCEncodeKeyList(keys, keyBuf) != 0) ||
	    (_SCEncodeKeyList(patterns, patternBuf) != 0)) {
		if (keyBuf != NULL)	CFRelease(keyBuf);
		if (patternBuf != NULL)	CFRelease(patternBuf);
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	kr = configget_m(storePrivate->server,
			 (xmlData_t)CFDataGetBytePtr(keyBuf),
			 (mach_msg_type_number_t)CFDataGetLength(keyBuf),
			 (xmlData_t)CFDataGetBytePtr(patternBuf),
			 (mach_msg_type_number_t)CFDataGetLength(patternBuf),
			 &reply, &replyLen,
			 &sc_status);
	CFRelease(keyBuf);
	CFRelease(patternBuf);
	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return NULL;
	}

	result = CFDictionaryCreateMutable(NULL, 0,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	if (result == NULL) {
		if (reply != NULL) {
			(void) vm_deallocate(mach_task_self(), (vm_address_t)reply, replyLen);
		}
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	/* the reply is a key/value map; each value is an XML-plist blob */
	cur = (const uint8_t *)reply;
	end = cur + replyLen;
	while (wire_kvmap_next(&cur, end, &keyBytes, &kl, &valueBytes, &vl)) {
		CFStringRef		key;
		CFPropertyListRef	value;

		key   = CFStringCreateWithBytes(NULL, keyBytes, (CFIndex)kl,
						kCFStringEncodingUTF8, FALSE);
		value = _SCUnserialize(valueBytes, (CFIndex)vl);
		if ((key != NULL) && (value != NULL)) {
			CFDictionarySetValue(result, key, value);
		}
		if (key != NULL)   CFRelease(key);
		if (value != NULL) CFRelease(value);
	}
	if (reply != NULL) {
		(void) vm_deallocate(mach_task_self(), (vm_address_t)reply, replyLen);
	}

	_SCErrorSet(kSCStatusOK);
	return result;
}

Boolean
SCDynamicStoreSetMultiple(SCDynamicStoreRef	store,
			  CFDictionaryRef	keysToSet,
			  CFArrayRef		keysToRemove,
			  CFArrayRef		keysToNotify)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFMutableDataRef		setBuf;
	CFMutableDataRef		removeBuf;
	CFMutableDataRef		notifyBuf;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return FALSE;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return FALSE;
	}

	setBuf    = CFDataCreateMutable(NULL, 0);
	removeBuf = CFDataCreateMutable(NULL, 0);
	notifyBuf = CFDataCreateMutable(NULL, 0);
	if ((setBuf == NULL) || (removeBuf == NULL) || (notifyBuf == NULL) ||
	    (encode_kvmap(keysToSet, setBuf) != 0) ||
	    (_SCEncodeKeyList(keysToRemove, removeBuf) != 0) ||
	    (_SCEncodeKeyList(keysToNotify, notifyBuf) != 0)) {
		if (setBuf != NULL)	CFRelease(setBuf);
		if (removeBuf != NULL)	CFRelease(removeBuf);
		if (notifyBuf != NULL)	CFRelease(notifyBuf);
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	kr = configset_m(storePrivate->server,
			 (xmlData_t)CFDataGetBytePtr(setBuf),
			 (mach_msg_type_number_t)CFDataGetLength(setBuf),
			 (xmlData_t)CFDataGetBytePtr(removeBuf),
			 (mach_msg_type_number_t)CFDataGetLength(removeBuf),
			 (xmlData_t)CFDataGetBytePtr(notifyBuf),
			 (mach_msg_type_number_t)CFDataGetLength(notifyBuf),
			 &sc_status);
	CFRelease(setBuf);
	CFRelease(removeBuf);
	CFRelease(notifyBuf);
	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}
	return TRUE;
}
