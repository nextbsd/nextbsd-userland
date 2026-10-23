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
 * SCPreferencesGetSpecificPrivate.h — read the host name preferences.
 *
 * NextBSD port of Apple's SystemConfiguration.framework
 * SCPreferencesGetSpecificPrivate.h (configd-1405.120.5).
 * _SCPreferencesCopyComputerName is implemented in SCPName.c (and also
 * declared in SCPreferences.h, where existing callers find it); the
 * other two are in SCDHostName.c.
 */

#ifndef _SCPREFERENCESGETSPECIFICPRIVATE_H
#define _SCPREFERENCESGETSPECIFICPRIVATE_H

#include <sys/cdefs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCPreferences.h>

__BEGIN_DECLS

/*!
	@function SCPreferencesGetHostName
	@discussion Gets the host name preference.
	@param prefs The preferences session.
	@result The host name (not retained);
		NULL if the name has not been set or if an error was encountered.
 */
CFStringRef
SCPreferencesGetHostName		(SCPreferencesRef	prefs);

/*!
	@function _SCPreferencesCopyLocalHostName
	@discussion Reads LocalHostName from /System/Network/HostNames.
	@param prefs The preferences session, or NULL for the default
		preferences file.
	@result The local host name (caller releases), or NULL.
 */
CFStringRef
_SCPreferencesCopyLocalHostName		(SCPreferencesRef	prefs);

CFStringRef
_SCPreferencesCopyComputerName		(SCPreferencesRef	prefs,
					 CFStringEncoding	*nameEncoding);

__END_DECLS

#endif	/* _SCPREFERENCESGETSPECIFICPRIVATE_H */
