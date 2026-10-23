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
 * SCPreferencesSetSpecific.h — set the computer and local host names.
 *
 * NextBSD port of Apple's SystemConfiguration.framework
 * SCPreferencesSetSpecific.h (configd-1405.120.5); the availability
 * and nullability annotations are dropped. Implemented in SCDHostName.c.
 */

#ifndef _SCPREFERENCESSETSPECIFIC_H
#define _SCPREFERENCESSETSPECIFIC_H

#include <sys/cdefs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCPreferences.h>

__BEGIN_DECLS

/*!
	@function SCPreferencesSetComputerName
	@discussion Updates the computer name preference.

		Note: To commit these changes to permanent storage you must
		call the SCPreferencesCommitChanges function.
	@param prefs The preferences session.
	@param name The computer name to be set; NULL or "" removes it.
	@param nameEncoding The encoding associated with the computer name.
	@result Returns TRUE if successful; FALSE otherwise.
 */
Boolean
SCPreferencesSetComputerName		(SCPreferencesRef	prefs,
					 CFStringRef		name,
					 CFStringEncoding	nameEncoding);

/*!
	@function SCPreferencesSetLocalHostName
	@discussion Updates the local host name.

		Note: To commit these changes to permanent storage you must
		call the SCPreferencesCommitChanges function.
	@param prefs The preferences session.
	@param name The local host name to be set; NULL or "" removes it.

	Note: this string must conform to the naming conventions of a DNS host
		name as specified in RFC 1034 (section 3.5), and must be a
		single label (no ".").
	@result Returns TRUE if successful; FALSE otherwise.
 */
Boolean
SCPreferencesSetLocalHostName		(SCPreferencesRef	prefs,
					 CFStringRef		name);

__END_DECLS

#endif	/* _SCPREFERENCESSETSPECIFIC_H */
