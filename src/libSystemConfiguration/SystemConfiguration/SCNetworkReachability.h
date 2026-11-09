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
 * SCNetworkReachability.h — async DNS / reverse-DNS / reachability
 * lookups, dispatched on a libdispatch queue.
 *
 * freebsd-launchd-mach port: the PTR-only subset of Apple's
 * SystemConfiguration.framework SCNetworkReachability.h. The full
 * API supports forward DNS, host-reachability flags, and interface-
 * change notifications; this port covers only what the vendored
 * IPMonitor set-hostname.c uses — async reverse PTR lookup keyed by a
 * sockaddr, with cancellation via CFRelease — backed by libdns_sd's
 * DNSServiceQueryRecord against mDNSResponder. Forward DNS /
 * reachability flags will land when a consumer needs them.
 */

#ifndef _SCNETWORKREACHABILITY_H
#define _SCNETWORKREACHABILITY_H

#include <sys/cdefs.h>
#include <sys/socket.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>

/*!
	@typedef SCNetworkReachabilityRef
	@discussion Opaque handle to a reachability target. A CoreFoundation
		type (see SCNetworkReachabilityGetTypeID()) — release with
		CFRelease(). Releasing cancels any in-flight query and
		fires no further callbacks.
 */
typedef const struct __SCNetworkReachability *	SCNetworkReachabilityRef;

/*!
	@typedef SCNetworkReachabilityContext
	@discussion Client-supplied data + lifecycle callbacks. The
		`info` pointer is passed verbatim to the reachability
		callback.
 */
typedef struct {
	CFIndex		version;
	void *		info;
	const void *	(*retain)(const void *info);
	void		(*release)(const void *info);
	CFStringRef	(*copyDescription)(const void *info);
} SCNetworkReachabilityContext;

/*!
	@typedef SCNetworkReachabilityFlags
	@discussion Bitmask describing the reachability state. NextBSD has
		no PPP, on-demand or VPN, so Reachable, IsLocalAddress,
		IsDirect and ConnectionRequired are the only flags set;
		TransientConnection, ConnectionOnTraffic, ConnectionOnDemand
		and InterventionRequired stay defined for source
		compatibility.
 */
typedef uint32_t	SCNetworkReachabilityFlags;

#define	kSCNetworkReachabilityFlagsTransientConnection		(1 << 0)
#define	kSCNetworkReachabilityFlagsReachable			(1 << 1)
#define	kSCNetworkReachabilityFlagsConnectionRequired		(1 << 2)
#define	kSCNetworkReachabilityFlagsConnectionOnTraffic		(1 << 3)
#define	kSCNetworkReachabilityFlagsInterventionRequired		(1 << 4)
#define	kSCNetworkReachabilityFlagsConnectionOnDemand		(1 << 5)
#define	kSCNetworkReachabilityFlagsIsLocalAddress		(1 << 16)
#define	kSCNetworkReachabilityFlagsIsDirect			(1 << 17)
#define	kSCNetworkReachabilityFlagsConnectionAutomatic		kSCNetworkReachabilityFlagsConnectionOnTraffic

/*!
	@typedef SCNetworkReachabilityCallBack
	@discussion Fired on the dispatch queue (or run loop) the
		target is scheduled on, whenever the resolved state
		changes. For a PTR query, fires once with
		kSCNetworkReachabilityFlagsReachable set when the answer
		arrives, or 0 on timeout / failure.
 */
typedef void (*SCNetworkReachabilityCallBack)(
	SCNetworkReachabilityRef		target,
	SCNetworkReachabilityFlags		flags,
	void *					info);

/*!
	@const kSCNetworkReachabilityOptionPTRAddress
	@discussion Options-dict key for CreateWithOptions. The value is
		a CFData wrapping a struct sockaddr (AF_INET or AF_INET6);
		the shim issues a reverse PTR for that address.
 */
extern const CFStringRef	kSCNetworkReachabilityOptionPTRAddress;

/*!
	@constant kSCNetworkReachabilityOptionNodeName
	@discussion Options-dictionary keys for
		SCNetworkReachabilityCreateWithOptions(), from configd-596.15
		SCPrivate.h (dnsinfo_shim.h's static _SC_* helpers cannot
		coexist with SCPrivate.h, so they live here). The value of
		NodeName and Interface is a CFString, of LocalAddress and
		RemoteAddress a CFData holding a struct sockaddr, and of
		ResolverBypass a CFBoolean. The reachability server and
		on-demand keys have nothing to act on here and are not
		defined.
*/
#define kSCNetworkReachabilityOptionNodeName		CFSTR("nodename")
#define kSCNetworkReachabilityOptionLocalAddress	CFSTR("local-address")
#define kSCNetworkReachabilityOptionRemoteAddress	CFSTR("remote-address")
#define kSCNetworkReachabilityOptionInterface		CFSTR("interface")
#define kSCNetworkReachabilityOptionResolverBypass	CFSTR("ResolverBypass")

__BEGIN_DECLS

/*!
	@function SCNetworkReachabilityGetTypeID
	@discussion CoreFoundation type ID for SCNetworkReachabilityRef.
 */
CFTypeID
SCNetworkReachabilityGetTypeID	(void);

/*!
	@function SCNetworkReachabilityCreateWithAddress
	@discussion Create a target for a host address. Caller releases.
 */
SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithAddress
				(CFAllocatorRef			allocator,
				 const struct sockaddr *	address);

/*!
	@function SCNetworkReachabilityCreateWithAddressPair
	@discussion Create a target for a local and/or remote address.
		Caller releases.
 */
SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithAddressPair
				(CFAllocatorRef			allocator,
				 const struct sockaddr *	localAddress,
				 const struct sockaddr *	remoteAddress);

/*!
	@function SCNetworkReachabilityCreateWithName
	@discussion Create a target for a node name. Caller releases.
 */
SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithName
				(CFAllocatorRef			allocator,
				 const char *			nodename);

/*!
	@function SCNetworkReachabilityCreateWithOptions
	@discussion Create a target from an options dictionary. The keys
		are the kSCNetworkReachabilityOption* below, plus
		kSCNetworkReachabilityOptionPTRAddress for a reverse
		lookup. Caller releases.
 */
SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithOptions
				(CFAllocatorRef			allocator,
				 CFDictionaryRef		options);

/*!
	@function SCNetworkReachabilitySetCallback
	@discussion Sets (or clears, with NULL) the callback the target
		fires when its reachability changes. Must be called before
		scheduling. Returns TRUE on success.
 */
Boolean
SCNetworkReachabilitySetCallback
				(SCNetworkReachabilityRef	target,
				 SCNetworkReachabilityCallBack	callout,
				 SCNetworkReachabilityContext *	context);

/*!
	@function SCNetworkReachabilitySetDispatchQueue
	@discussion Schedules callback delivery on a dispatch queue. Pass
		NULL to unschedule (this cancels any in-flight query and
		fires no further callbacks). Returns TRUE on success.
 */
Boolean
SCNetworkReachabilitySetDispatchQueue
				(SCNetworkReachabilityRef	target,
				 dispatch_queue_t		queue);

/*!
	@function SCNetworkReachabilityCopyResolvedAddress
	@discussion After the callback fires with
		kSCNetworkReachabilityFlagsReachable, returns the resolved
		results. For a PTR query, the array contains
		CFStringRef hostnames (length 1 in this port). Returns
		NULL if the query has not completed, timed out, or failed;
		writes the libdns_sd error code into *error_num if non-NULL.
		Caller releases.
 */
CFArrayRef
SCNetworkReachabilityCopyResolvedAddress
				(SCNetworkReachabilityRef	target,
				 int *				error_num);

/*!
	@function SCNetworkReachabilityGetFlags
	@discussion Determines the reachability of the target. Returns
		FALSE if it could not be determined.
 */
Boolean
SCNetworkReachabilityGetFlags	(SCNetworkReachabilityRef	target,
				 SCNetworkReachabilityFlags *	flags);

/*!
	@function SCNetworkReachabilityGetInterfaceIndex
	@discussion Returns the index of the interface the target is
		reached through, or -1 if it is not reachable.
 */
int
SCNetworkReachabilityGetInterfaceIndex
				(SCNetworkReachabilityRef	target);

/*!
	@function SCNetworkReachabilityScheduleWithRunLoop
	@discussion Schedules callback delivery on a run loop.
 */
Boolean
SCNetworkReachabilityScheduleWithRunLoop
				(SCNetworkReachabilityRef	target,
				 CFRunLoopRef			runLoop,
				 CFStringRef			runLoopMode);

/*!
	@function SCNetworkReachabilityUnscheduleFromRunLoop
	@discussion Removes callback delivery from a run loop.
 */
Boolean
SCNetworkReachabilityUnscheduleFromRunLoop
				(SCNetworkReachabilityRef	target,
				 CFRunLoopRef			runLoop,
				 CFStringRef			runLoopMode);

/*!
	@function __SC_checkResolverReachabilityInternal
	@discussion SPI: which resolver would answer a query for nodename,
		and whether it is reachable. scutil --dns <hostname>.
 */
Boolean
__SC_checkResolverReachabilityInternal
				(SCDynamicStoreRef *		storeP,
				 SCNetworkReachabilityFlags *	flags,
				 Boolean *			haveDNS,
				 const char *			nodename,
				 uint32_t *			resolver_if_index,
				 int *				dns_config_index);

/*!
	@function __SCNetworkReachabilityGetTargetName
	@discussion SPI: the node name of a by-name target and its scope, or
		NULL for an address target. Apple's scutil reads the private
		struct for these.
 */
const char *
__SCNetworkReachabilityGetTargetName
				(SCNetworkReachabilityRef	target,
				 unsigned int *			if_index);

__END_DECLS

#endif	/* _SCNETWORKREACHABILITY_H */
