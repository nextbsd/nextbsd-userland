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
 * SCDynamicStoreCopySpecific.h — the console user (nextbsd-userland#199).
 *
 * NextBSD port of the console-user part of Apple's SystemConfiguration
 * .framework SCDynamicStoreCopySpecific.h (configd-1405.120.5); the
 * computer and host name readers live in SCDynamicStore.h here. The
 * availability and nullability annotations are dropped. Implemented in
 * SCDConsoleUser.c.
 */

#ifndef _SCDYNAMICSTORECOPYSPECIFIC_H
#define _SCDYNAMICSTORECOPYSPECIFIC_H

#include <sys/cdefs.h>
#include <sys/types.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>

__BEGIN_DECLS

/*!
	@function SCDynamicStoreCopyConsoleUser
	@discussion Gets the name, user ID and group ID of the user logged
		in at the console (State:/Users/ConsoleUser).
	@param store An SCDynamicStoreRef, or NULL for a temporary session.
	@param uid Where to return the user ID, or NULL.
	@param gid Where to return the group ID, or NULL.
	@result The user name (caller releases), or NULL if nobody is
		logged in or on error.
 */
CFStringRef
SCDynamicStoreCopyConsoleUser	(SCDynamicStoreRef		store,
				 uid_t				*uid,
				 gid_t				*gid);

__END_DECLS

#endif	/* _SCDYNAMICSTORECOPYSPECIFIC_H */
