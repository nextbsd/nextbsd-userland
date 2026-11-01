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
 * SCDynamicStoreCopySpecificPrivate.h — the console session list
 * (nextbsd-userland#199).
 *
 * NextBSD port of the console-session part of Apple's
 * SCDynamicStoreCopySpecificPrivate.h (configd-1405.120.5). The session
 * dictionary keys keep Apple's values (CoreGraphics' kCGSSession* names),
 * so a publisher written for macOS fills the same dictionary. Implemented
 * in SCDConsoleUser.c.
 */

#ifndef _SCDYNAMICSTORECOPYSPECIFICPRIVATE_H
#define _SCDYNAMICSTORECOPYSPECIFICPRIVATE_H

#include <sys/cdefs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>

/* Keys of one console session dictionary (Apple's values). */
#define kSCConsoleSessionID		CFSTR("kCGSSessionIDKey")		/* SInt32 */
#define kSCConsoleSessionUserName	CFSTR("kCGSSessionUserNameKey")		/* CFString */
#define kSCConsoleSessionUID		CFSTR("kCGSSessionUserIDKey")		/* SInt32 (uid_t) */
#define kSCConsoleSessionConsoleSet	CFSTR("kCGSSessionConsoleSetKey")	/* SInt32 */
#define kSCConsoleSessionOnConsole	CFSTR("kCGSSessionOnConsoleKey")	/* CFBoolean */
#define kSCConsoleSessionLoginDone	CFSTR("kCGSessionLoginDoneKey")		/* CFBoolean */
#define kSCConsoleSessionSystemSafeBoot	CFSTR("kCGSSessionSystemSafeBoot")	/* CFBoolean */
#define kSCConsoleSessionLoginwindowSafeLogin \
					CFSTR("kCGSSessionLoginwindowSafeLogin") /* CFBoolean */

__BEGIN_DECLS

/*!
	@function SCDynamicStoreCopyConsoleInformation
	@discussion Gets the console sessions (the SessionInfo array of
		State:/Users/ConsoleUser).
	@param store An SCDynamicStoreRef, or NULL for a temporary session.
	@result A CFArray of session dictionaries (caller releases), or NULL
		if there are none.
 */
CFArrayRef
SCDynamicStoreCopyConsoleInformation	(SCDynamicStoreRef	store);

__END_DECLS

#endif	/* _SCDYNAMICSTORECOPYSPECIFICPRIVATE_H */
