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
 * SCPreferencesSetSpecificPrivate.h — set the host name preference.
 *
 * NextBSD port of Apple's SystemConfiguration.framework
 * SCPreferencesSetSpecificPrivate.h (configd-1405.120.5). Implemented in
 * SCDHostName.c.
 */

#ifndef _SCPREFERENCESSETSPECIFICPRIVATE_H
#define _SCPREFERENCESSETSPECIFICPRIVATE_H

#include <sys/cdefs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCPreferences.h>

__BEGIN_DECLS

/*!
	@function SCPreferencesSetHostName
	@discussion Updates the host name preference.

		Note: To commit these changes to permanent storage you must
		call the SCPreferencesCommitChanges function.
	@param prefs The preferences session.
	@param name The host name to be set; NULL or "" removes it.
	@result Returns TRUE if successful; FALSE otherwise.
 */
Boolean
SCPreferencesSetHostName		(SCPreferencesRef	prefs,
					 CFStringRef		name);

__END_DECLS

#endif	/* _SCPREFERENCESSETSPECIFICPRIVATE_H */
