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
 * SCDHostName.c — the Set side of the host-name API, plus the
 * SCPreferences readers that pair with it.
 *
 *   SCPreferencesSetComputerName   /System/System  ComputerName(+Encoding)
 *   SCPreferencesSetHostName       /System/System  HostName
 *   SCPreferencesGetHostName       /System/System  HostName
 *   SCPreferencesSetLocalHostName  /System/Network/HostNames LocalHostName
 *   _SCPreferencesCopyLocalHostName                   (reader for the above)
 *
 * Ported from apple-oss-distributions/configd @ configd-1405.120.5
 * (SystemConfiguration.fproj/SCDHostName.c). The read-only halves that
 * already existed stay where they were: _SCPreferencesCopyComputerName in
 * SCPName.c, SCDynamicStoreCopy{Computer,LocalHost}Name in SCDName.c.
 *
 * NextBSD differences from upstream:
 *
 *   - Apple writes through __SCNetworkConfigurationSetValue, which adds
 *     the network-configuration change tracking we do not have. The
 *     setters here use __SCPreferencesSetNameDict below, which keeps its
 *     user-visible contract (skip an unchanged value, remove the path
 *     once its dictionary is empty) on top of SCPreferencesPath*.
 *   - No ComputerNameRegion: it only exists for MacRoman names, and the
 *     user-default-encoding lookup it needs is a CoreFoundation SPI that
 *     our CF does not carry.
 *   - No SC_log notices on a successful set.
 *
 * Callers commit with SCPreferencesCommitChanges; that commit fires the
 * SCPreferences callback hostnamed's prefs_monitor watches, which
 * republishes Setup:/System and Setup:/Network/HostNames.
 */

#include "SCInternal.h"
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCPreferencesSetSpecific.h>
#include <SystemConfiguration/SCPreferencesSetSpecificPrivate.h>
#include <SystemConfiguration/SCPreferencesGetSpecificPrivate.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

#include <ctype.h>
#include <string.h>

/* Apple's SCValidation.h DNS-name limits. */
#define	_DNS_MAX_LEN		253
#define	_DNS_MAX_LABEL_LEN	63
#define	_DNS_MAX_LABELS		127

static Boolean
isA_CFStringValue(CFTypeRef cf)
{
	return ((cf != NULL) && (CFGetTypeID(cf) == CFStringGetTypeID()));
}

static Boolean
isA_CFDictionaryValue(CFTypeRef cf)
{
	return ((cf != NULL) && (CFGetTypeID(cf) == CFDictionaryGetTypeID()));
}

/*
 * Apple's _SC_stringIsValidDNSName (SCValidation.c), verbatim. Kept
 * static: hostnamed's freebsd-shim exports its own
 * _SC_CFStringIsValidDNSName and the two must not interpose.
 */
static Boolean
__SCStringIsValidDNSName(const char *name)
{
	size_t		i;
	size_t		label_len = 0;
	size_t		len;
	size_t		ndots = 0;
	char		prev = '\0';
	const char	*scan;
	Boolean		valid = FALSE;

	len = (name == NULL) ? 0 : strlen(name);
	if (len == 0 || len > _DNS_MAX_LEN) {
		goto done;
	}

	for (scan = name, i = 0; i < len; i++, scan++) {
		char	ch	= *scan;
		char 	next	= *(scan + 1);

		if (ch == '.') {
			ndots++;
			if (ndots > _DNS_MAX_LABELS) {
				/* too many labels */
				goto done;
			}
		} else {
			label_len++;
		}
		if ((prev == '.' || prev == '\0') && isalnum(ch) == 0) {
			/* a label must begin with a letter or digit */
			goto done;
		}
		if (next == '\0' || next == '.') {
			if (label_len > _DNS_MAX_LABEL_LEN) {
				goto done;
			}
			label_len = 0;
			if (ch == '.' && next == '\0' && ndots > 1) {
				/* trailing dot is OK e.g. "apple.com." */
				break;
			}
			if (isalnum(ch) == 0) {
				/* a label must end with a letter or digit */
				goto done;
			}
			if (next == '\0' && ndots == _DNS_MAX_LABELS) {
				/* too many labels */
				goto done;
			}
		} else if (isalnum(ch) == 0) {
			switch (ch) {
				case '.':
					/* a label separator */
					break;
				case '-':
					/* hyphens are OK within a label */
					break;
				default:
					/* an invalid character */
					goto done;
			}
		}
		prev = ch;
	}
	valid = TRUE;

 done:
	return valid;
}

static Boolean
__SCCFStringIsValidDNSName(CFStringRef name)
{
	char	buf[_DNS_MAX_LEN + 2];

	if (!isA_CFStringValue(name)) {
		return FALSE;
	}
	if (!CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingASCII)) {
		return FALSE;
	}
	return __SCStringIsValidDNSName(buf);
}

/*
 * Write `newDict` at `path` the way __SCNetworkConfigurationSetValue
 * does for these callers: an unchanged value is not rewritten, and an
 * empty dictionary removes the path rather than leaving {} behind.
 */
static Boolean
__SCPreferencesSetNameDict(SCPreferencesRef prefs, CFStringRef path,
			   CFDictionaryRef newDict)
{
	CFDictionaryRef	curDict;

	curDict = SCPreferencesPathGetValue(prefs, path);
	if (CFDictionaryGetCount(newDict) == 0) {
		if (curDict == NULL) {
			_SCErrorSet(kSCStatusOK);
			return TRUE;
		}
		return SCPreferencesPathRemoveValue(prefs, path);
	}
	if ((curDict != NULL) && CFEqual(curDict, newDict)) {
		_SCErrorSet(kSCStatusOK);
		return TRUE;
	}
	return SCPreferencesPathSetValue(prefs, path, newDict);
}

/* A mutable copy of the dictionary at `path`, or a new empty one. */
static CFMutableDictionaryRef
__SCPreferencesCopyNameDict(SCPreferencesRef prefs, CFStringRef path)
{
	CFDictionaryRef	dict;

	dict = SCPreferencesPathGetValue(prefs, path);
	if (isA_CFDictionaryValue(dict)) {
		return CFDictionaryCreateMutableCopy(NULL, 0, dict);
	}
	return CFDictionaryCreateMutable(NULL,
					 0,
					 &kCFTypeDictionaryKeyCallBacks,
					 &kCFTypeDictionaryValueCallBacks);
}


#pragma mark ComputerName


Boolean
SCPreferencesSetComputerName(SCPreferencesRef	prefs,
			     CFStringRef	name,
			     CFStringEncoding	encoding)
{
	CFMutableDictionaryRef	newDict;
	CFNumberRef		num;
	Boolean			ok;
	CFStringRef		path;

	if (name != NULL) {
		CFIndex	len;

		if (!isA_CFStringValue(name)) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}

		len = CFStringGetLength(name);
		if (len == 0) {
			name = NULL;
		}
	}

	path = CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("/%@/%@"),
					kSCCompSystem,
					kSCCompSystem);

	newDict = __SCPreferencesCopyNameDict(prefs, path);

	if (name != NULL) {
		SInt32	enc32	= (SInt32)encoding;

		CFDictionarySetValue(newDict, kSCPropSystemComputerName, name);

		num = CFNumberCreate(NULL, kCFNumberSInt32Type, &enc32);
		CFDictionarySetValue(newDict, kSCPropSystemComputerNameEncoding, num);
		CFRelease(num);
	} else {
		CFDictionaryRemoveValue(newDict, kSCPropSystemComputerName);
		CFDictionaryRemoveValue(newDict, kSCPropSystemComputerNameEncoding);
	}

	ok = __SCPreferencesSetNameDict(prefs, path, newDict);

	CFRelease(path);
	CFRelease(newDict);

	return ok;
}


#pragma mark -
#pragma mark HostName


CFStringRef
SCPreferencesGetHostName(SCPreferencesRef	prefs)
{
	CFDictionaryRef	dict;
	CFStringRef	name;
	CFStringRef	path;

	path = CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("/%@/%@"),
					kSCCompSystem,
					kSCCompSystem);
	dict = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);

	if (!isA_CFDictionaryValue(dict)) {
		_SCErrorSet(kSCStatusNoKey);
		return NULL;
	}

	name = CFDictionaryGetValue(dict, kSCPropSystemHostName);
	if (!isA_CFStringValue(name)) {
		_SCErrorSet(kSCStatusNoKey);
		return NULL;
	}

	return name;
}


Boolean
SCPreferencesSetHostName(SCPreferencesRef	prefs,
			 CFStringRef		name)
{
	CFMutableDictionaryRef	newDict;
	Boolean			ok;
	CFStringRef		path;

	if (name != NULL) {
		CFIndex	len;

		if (!isA_CFStringValue(name)) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}

		len = CFStringGetLength(name);
		if (len == 0) {
			name = NULL;
		}
	}

	path = CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("/%@/%@"),
					kSCCompSystem,
					kSCCompSystem);

	newDict = __SCPreferencesCopyNameDict(prefs, path);

	if (name != NULL) {
		CFDictionarySetValue(newDict, kSCPropSystemHostName, name);
	} else {
		CFDictionaryRemoveValue(newDict, kSCPropSystemHostName);
	}

	ok = __SCPreferencesSetNameDict(prefs, path, newDict);

	CFRelease(path);
	CFRelease(newDict);

	return ok;
}


#pragma mark -
#pragma mark LocalHostName


CFStringRef
_SCPreferencesCopyLocalHostName(SCPreferencesRef	prefs)
{
	CFDictionaryRef	dict;
	CFStringRef	name		= NULL;
	CFStringRef	path;
	Boolean		tempPrefs	= FALSE;

	if (prefs == NULL) {
		prefs = SCPreferencesCreate(NULL, CFSTR("_SCPreferencesCopyLocalHostName"), NULL);
		if (prefs == NULL) {
			return NULL;
		}
		tempPrefs = TRUE;
	}

	path = CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("/%@/%@/%@"),
					kSCCompSystem,
					kSCCompNetwork,
					kSCCompHostNames);
	dict = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);

	if (isA_CFDictionaryValue(dict)) {
		name = CFDictionaryGetValue(dict, kSCPropNetLocalHostName);
		if (isA_CFStringValue(name)) {
			CFRetain(name);
		} else {
			name = NULL;
		}
	}

	if (tempPrefs)	CFRelease(prefs);
	_SCErrorSet(name != NULL ? kSCStatusOK : kSCStatusNoKey);
	return name;
}


Boolean
SCPreferencesSetLocalHostName(SCPreferencesRef	prefs,
			      CFStringRef	name)
{
	CFMutableDictionaryRef	newDict;
	Boolean			ok;
	CFStringRef		path;

	if (name != NULL) {
		CFIndex	len;

		if (!isA_CFStringValue(name)) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}

		len = CFStringGetLength(name);
		if (len > 0) {
			if (!__SCCFStringIsValidDNSName(name)) {
				_SCErrorSet(kSCStatusInvalidArgument);
				return FALSE;
			}

			if (CFStringFindWithOptions(name, CFSTR("."), CFRangeMake(0, len), 0, NULL)) {
				_SCErrorSet(kSCStatusInvalidArgument);
				return FALSE;
			}
		} else {
			name = NULL;
		}
	}

	path = CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("/%@/%@/%@"),
					kSCCompSystem,
					kSCCompNetwork,
					kSCCompHostNames);

	newDict = __SCPreferencesCopyNameDict(prefs, path);

	if (name != NULL) {
		CFDictionarySetValue(newDict, kSCPropNetLocalHostName, name);
	} else {
		CFDictionaryRemoveValue(newDict, kSCPropNetLocalHostName);
	}

	ok = __SCPreferencesSetNameDict(prefs, path, newDict);

	CFRelease(path);
	CFRelease(newDict);

	return ok;
}
