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
 * SCDynamicStoreSetSpecificPrivate.h — publish the console user
 * (nextbsd-userland#199).
 *
 * NextBSD port of the console-user setter of Apple's
 * SCDynamicStoreSetSpecificPrivate.h (configd-1405.120.5); Apple's
 * loginwindow calls it, here /usr/libexec/consoleuser does. configd
 * accepts the write from root only. Implemented in SCDConsoleUser.c.
 */

#ifndef _SCDYNAMICSTORESETSPECIFICPRIVATE_H
#define _SCDYNAMICSTORESETSPECIFICPRIVATE_H

#include <sys/cdefs.h>
#include <sys/types.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>

__BEGIN_DECLS

/*!
	@function SCDynamicStoreSetConsoleInformation
	@discussion Sets (or, with user and sessions both NULL, removes)
		State:/Users/ConsoleUser.
	@param store An SCDynamicStoreRef, or NULL for a temporary session.
	@param user The console user's name, or NULL.
	@param uid The console user's user ID (ignored without user).
	@param gid The console user's group ID (ignored without user).
	@param sessions A CFArray of session dictionaries
		(kSCConsoleSession* keys), or NULL.
	@result TRUE on success; on failure SCError() says why
		(kSCStatusAccessError for a non-root caller).
 */
Boolean
SCDynamicStoreSetConsoleInformation	(SCDynamicStoreRef	store,
					 const char		*user,
					 uid_t			uid,
					 gid_t			gid,
					 CFArrayRef		sessions);

__END_DECLS

#endif	/* _SCDYNAMICSTORESETSPECIFICPRIVATE_H */
