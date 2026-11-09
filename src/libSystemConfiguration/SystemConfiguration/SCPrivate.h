/*
 * Copyright (c) 2000-2023 Apple Inc. All rights reserved.
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
 * SCPrivate.h — SystemConfiguration SPI. NextBSD port of the private
 * header from apple-oss-distributions/configd/SystemConfiguration.fproj/
 * SCPrivate.h, populated as in-tree consumers need it.
 *
 * The notify(3) names: configd's KernelEventMonitor posts
 * _SC_NOTIFY_NETWORK_CHANGE on network state changes and set-hostname.c
 * (hostnamed) registers for it. The values are Apple's stable
 * notification names; do not edit them.
 *
 * The string and address helpers are from Apple's SCDPrivate.c.
 *
 * Unlike Apple's header this one does not pull in
 * <SystemConfiguration/SCNetworkConfigurationPrivate.h>; include that
 * directly for the SCNetworkConfiguration SPI.
 */

#ifndef _SCPRIVATE_H
#define _SCPRIVATE_H

/*
 * configd's config_link_monitor.c and ipconfigd's apply_lease.c read
 * this header for the notify names alone and do not include
 * CoreFoundation, so the helpers are declared only for a file that
 * already has.
 */
#ifdef __COREFOUNDATION_CFBASE__

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <SystemConfiguration/SCDynamicStore.h>

__BEGIN_DECLS

/*!
	@typedef SCDynamicStoreServerDeathCallBack
	@discussion Called once when configd has gone away (see
		_SCDynamicStoreSetServerDeathCallBack).
 */
typedef void (*SCDynamicStoreServerDeathCallBack)(SCDynamicStoreRef store,
						  void *info);

/*!
	@function _SCDynamicStoreSetServerDeathCallBack
	@discussion Asks to be told when this session's configd has gone
		away. The session must deliver notifications with
		SCDynamicStoreSetDispatchQueue or a run-loop source: the
		detection is the no-senders notification on its notification
		port (Darwin's SCDNotifierInformViaCallback.c), and the
		callback runs on the queue that delivers its changes.

		This is not Darwin's SCDynamicStoreSetDisconnectCallBack,
		whose callback runs after the library has reconnected. This
		library does not reconnect, so the session is dead when the
		callback runs, and the store it was publishing to has been
		emptied. The caller's job is to get itself restarted or to
		make new sessions.
	@param store the session
	@param callback the function to call, or NULL for none
	@result TRUE, or FALSE if store is not a session.
 */
Boolean
_SCDynamicStoreSetServerDeathCallBack	(SCDynamicStoreRef		store,
					 SCDynamicStoreServerDeathCallBack callback);

/*!
	@function _SC_cfstring_to_cstring
	@discussion Converts a CFString to a NUL-terminated C string. With
		`buf` NULL the result is allocated and the caller frees it
		with CFAllocatorDeallocate.
	@result the C string, or NULL if it cannot be converted or does
		not fit.
 */
char *
_SC_cfstring_to_cstring			(CFStringRef		cfstr,
					 char			*buf,
					 CFIndex		bufLen,
					 CFStringEncoding	encoding);

/*!
	@function _SC_sockaddr_to_string
	@discussion Formats an AF_INET or AF_INET6 address into `buf`.
 */
void
_SC_sockaddr_to_string			(const struct sockaddr	*address,
					 char			*buf,
					 size_t			bufLen);

/*!
	@function _SC_string_to_sockaddr
	@discussion Parses a numeric address; AF_UNSPEC accepts IPv4 or
		IPv6. With `buf` NULL the result is allocated and the
		caller frees it with CFAllocatorDeallocate.
	@result the address, or NULL if `str` does not parse.
 */
struct sockaddr *
_SC_string_to_sockaddr			(const char		*str,
					 sa_family_t		af,
					 void			*buf,
					 size_t			bufLen);

__END_DECLS

#endif	/* __COREFOUNDATION_CFBASE__ */

/* notify(3) keys */

#define _SC_NOTIFY_PREFIX			"com.apple.system.config"

#define _SC_NOTIFY_NETWORK_CHANGE		_SC_NOTIFY_PREFIX ".network_change"
#define _SC_NOTIFY_NETWORK_CHANGE_DNS		_SC_NOTIFY_NETWORK_CHANGE ".dns"
#define _SC_NOTIFY_NETWORK_CHANGE_NWI		_SC_NOTIFY_NETWORK_CHANGE ".nwi"
#define _SC_NOTIFY_NETWORK_CHANGE_PROXY		_SC_NOTIFY_NETWORK_CHANGE ".proxy"

#endif	/* _SCPRIVATE_H */
