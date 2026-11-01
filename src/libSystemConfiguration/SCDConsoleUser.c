/*
 * Copyright (c) 2000-2025 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * SCDConsoleUser.c — the console user in the dynamic store
 * (nextbsd-userland#199).
 *
 * NextBSD port of the read and write halves of Apple's
 * SystemConfiguration.fproj/SCDConsoleUser.c (configd-1405.120.5):
 * State:/Users/ConsoleUser is a dictionary of Name (CFString), UID and
 * GID (CFNumber) and SessionInfo (a CFArray of kSCConsoleSession*
 * dictionaries). Apple's loginwindow writes it with
 * SCDynamicStoreSetConsoleInformation; on NextBSD /usr/libexec/consoleuser
 * does, and configd takes the write from root only. diskarbitrationd
 * watches the key to defer removable and external mounts until a login.
 *
 * Apple's SCDynamicStoreSetConsoleUser (deprecated) and the CFRunLoop
 * console notification helpers are not ported.
 */

#include "SCInternal.h"

#include <SystemConfiguration/SCDynamicStoreCopySpecific.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecificPrivate.h>
#include <SystemConfiguration/SCDynamicStoreSetSpecificPrivate.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

/*
 * A NULL store means a temporary session, as Apple's
 * __SCDynamicStoreNullSession does for these calls.
 */
static SCDynamicStoreRef
console_store(SCDynamicStoreRef store, int *temporary)
{
	*temporary = 0;
	if (store != NULL)
		return (store);
	store = SCDynamicStoreCreate(NULL, CFSTR("SCDConsoleUser"), NULL,
	    NULL);
	if (store != NULL)
		*temporary = 1;
	return (store);
}

static CFDictionaryRef
console_copy(SCDynamicStoreRef store)
{
	CFDictionaryRef dict;
	CFStringRef key;
	int temporary;

	store = console_store(store, &temporary);
	if (store == NULL)
		return (NULL);
	key = SCDynamicStoreKeyCreateConsoleUser(NULL);
	dict = (key != NULL) ? SCDynamicStoreCopyValue(store, key) : NULL;
	if (key != NULL)
		CFRelease(key);
	if (temporary)
		CFRelease(store);
	if (dict != NULL && CFGetTypeID(dict) != CFDictionaryGetTypeID()) {
		CFRelease(dict);
		dict = NULL;
	}
	if (dict == NULL)
		_SCErrorSet(kSCStatusNoKey);
	return (dict);
}

CFStringRef
SCDynamicStoreCopyConsoleUser(SCDynamicStoreRef store, uid_t *uid,
    gid_t *gid)
{
	CFStringRef user = NULL;
	CFDictionaryRef dict;
	CFNumberRef num;
	SInt32 v;

	dict = console_copy(store);
	if (dict == NULL)
		return (NULL);
	user = CFDictionaryGetValue(dict, kSCPropUsersConsoleUserName);
	if (user == NULL || CFGetTypeID(user) != CFStringGetTypeID()) {
		_SCErrorSet(kSCStatusNoKey);
		CFRelease(dict);
		return (NULL);
	}
	CFRetain(user);
	if (uid != NULL) {
		num = CFDictionaryGetValue(dict, kSCPropUsersConsoleUserUID);
		if (num != NULL && CFGetTypeID(num) == CFNumberGetTypeID() &&
		    CFNumberGetValue(num, kCFNumberSInt32Type, &v))
			*uid = (uid_t)v;
	}
	if (gid != NULL) {
		num = CFDictionaryGetValue(dict, kSCPropUsersConsoleUserGID);
		if (num != NULL && CFGetTypeID(num) == CFNumberGetTypeID() &&
		    CFNumberGetValue(num, kCFNumberSInt32Type, &v))
			*gid = (gid_t)v;
	}
	CFRelease(dict);
	return (user);
}

CFArrayRef
SCDynamicStoreCopyConsoleInformation(SCDynamicStoreRef store)
{
	CFArrayRef sessions;
	CFDictionaryRef dict;

	dict = console_copy(store);
	if (dict == NULL)
		return (NULL);
	sessions = CFDictionaryGetValue(dict, kSCPropUsersConsoleSessionInfo);
	if (sessions != NULL && CFGetTypeID(sessions) == CFArrayGetTypeID())
		CFRetain(sessions);
	else {
		sessions = NULL;
		_SCErrorSet(kSCStatusNoKey);
	}
	CFRelease(dict);
	return (sessions);
}

static void
dict_set_int(CFMutableDictionaryRef dict, CFStringRef key, SInt32 value)
{
	CFNumberRef num;

	num = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
	if (num != NULL) {
		CFDictionarySetValue(dict, key, num);
		CFRelease(num);
	}
}

Boolean
SCDynamicStoreSetConsoleInformation(SCDynamicStoreRef store, const char *user,
    uid_t uid, gid_t gid, CFArrayRef sessions)
{
	CFMutableDictionaryRef dict = NULL;
	CFStringRef key, name;
	Boolean ok = FALSE;
	int temporary;

	store = console_store(store, &temporary);
	if (store == NULL)
		return (FALSE);
	key = SCDynamicStoreKeyCreateConsoleUser(NULL);
	if (key == NULL) {
		_SCErrorSet(kSCStatusFailed);
		goto done;
	}

	if (user == NULL && sessions == NULL) {
		ok = SCDynamicStoreRemoveValue(store, key);
		goto done;
	}

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL) {
		_SCErrorSet(kSCStatusFailed);
		goto done;
	}
	if (user != NULL) {
		name = CFStringCreateWithCString(NULL, user,
		    kCFStringEncodingUTF8);
		if (name == NULL) {
			_SCErrorSet(kSCStatusInvalidArgument);
			goto done;
		}
		CFDictionarySetValue(dict, kSCPropUsersConsoleUserName, name);
		CFRelease(name);
		dict_set_int(dict, kSCPropUsersConsoleUserUID, (SInt32)uid);
		dict_set_int(dict, kSCPropUsersConsoleUserGID, (SInt32)gid);
	}
	if (sessions != NULL)
		CFDictionarySetValue(dict, kSCPropUsersConsoleSessionInfo,
		    sessions);
	ok = SCDynamicStoreSetValue(store, key, dict);

done:
	if (dict != NULL)
		CFRelease(dict);
	if (key != NULL)
		CFRelease(key);
	if (temporary)
		CFRelease(store);
	return (ok);
}
