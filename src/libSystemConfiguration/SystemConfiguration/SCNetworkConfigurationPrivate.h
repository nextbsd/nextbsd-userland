/*
 * Copyright (c) 2005-2013 Apple Inc. All rights reserved.
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
 * SCNetworkConfigurationPrivate.h — SCNetworkConfiguration SPI. NextBSD
 * port of the private header from configd-596.15
 * SystemConfiguration.fproj/SCNetworkConfigurationPrivate.h, populated
 * as in-tree consumers need it: the default-configuration calls, primary
 * service ranks, and the live ("active") service and interface handles.
 *
 * Apple's SCPrivate.h includes this file. Here SCPrivate.h is also read
 * by daemons that have no use for the SCNetworkConfiguration types, so
 * include this header directly.
 */

#ifndef _SCNETWORKCONFIGURATIONPRIVATE_H
#define _SCNETWORKCONFIGURATIONPRIVATE_H

#include <sys/cdefs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCNetworkConfiguration.h>

/*!
	@enum SCNetworkServicePrimaryRank
	@discussion Used to influence how the primary service is chosen.
	@constant kSCNetworkServicePrimaryRankDefault No rank.
	@constant kSCNetworkServicePrimaryRankFirst Prefer this service.
	@constant kSCNetworkServicePrimaryRankLast Avoid this service.
	@constant kSCNetworkServicePrimaryRankNever Never primary.
 */
enum {
	kSCNetworkServicePrimaryRankDefault	= 0,
	kSCNetworkServicePrimaryRankFirst	= 1,
	kSCNetworkServicePrimaryRankLast	= 2,
	kSCNetworkServicePrimaryRankNever	= 3
};
typedef uint32_t	SCNetworkServicePrimaryRank;

__BEGIN_DECLS

/* --------------------------------------------------------------------
 * SCNetworkInterface
 * ------------------------------------------------------------------ */

/*!
	@function _SCNetworkInterfaceCopyActive
	@discussion Returns a handle to the live interface `bsdName`, or
		NULL if there is no such interface. `store` is the session
		used by SCNetworkInterfaceSetPrimaryRank. Release the result.
 */
SCNetworkInterfaceRef
_SCNetworkInterfaceCopyActive		(SCDynamicStoreRef		store,
					 CFStringRef			bsdName);

/*!
	@function _SCNetworkInterfaceCopyAllWithPreferences
	@discussion Returns the hardware interfaces plus the Bond, Bridge
		and VLAN interfaces stored in `prefs` (NULL opens the
		default preferences), sorted.
	@result a CFArray of SCNetworkInterfaceRef (caller releases).
 */
CFArrayRef
_SCNetworkInterfaceCopyAllWithPreferences
					(SCPreferencesRef		prefs);

/*!
	@function _SCNetworkInterfaceIsPhysicalEthernet
	@discussion Returns whether the interface is real Ethernet, judged
		from the media it offers.
 */
Boolean
_SCNetworkInterfaceIsPhysicalEthernet	(SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkInterfaceCopyCapability
	@discussion Returns the current value (a CFNumber, 0 or 1) of one
		offload capability, or a dictionary of every available
		capability when `capability` is NULL. Release the result.
 */
CFTypeRef
SCNetworkInterfaceCopyCapability	(SCNetworkInterfaceRef		interface,
					 CFStringRef			capability);

/*!
	@function SCNetworkInterfaceSetCapability
	@discussion Stores the desired value of one offload capability in
		the interface's configuration; NULL removes the setting.
 */
Boolean
SCNetworkInterfaceSetCapability		(SCNetworkInterfaceRef		interface,
					 CFStringRef			capability,
					 CFTypeRef			newValue);

/*!
	@function SCNetworkInterfaceSetPrimaryRank
	@discussion Sets the rank of the interface in the dynamic store.
		The interface must come from _SCNetworkInterfaceCopyActive.
 */
Boolean
SCNetworkInterfaceSetPrimaryRank	(SCNetworkInterfaceRef		interface,
					 SCNetworkServicePrimaryRank	newRank);

/* --------------------------------------------------------------------
 * SCNetworkService
 * ------------------------------------------------------------------ */

/*!
	@function _SCNetworkServiceCopyActive
	@discussion Returns a handle to the live service `serviceID`, whose
		rank is read and written through `store`. Release the result.
 */
SCNetworkServiceRef
_SCNetworkServiceCopyActive		(SCDynamicStoreRef		store,
					 CFStringRef			serviceID);

/*!
	@function SCNetworkServiceGetPrimaryRank
	@discussion Returns the service's rank, or
		kSCNetworkServicePrimaryRankDefault if it has none.
 */
SCNetworkServicePrimaryRank
SCNetworkServiceGetPrimaryRank		(SCNetworkServiceRef		service);

/*!
	@function SCNetworkServiceSetPrimaryRank
	@discussion Sets the service's rank. A service from preferences
		accepts only Default and Never; First and Last are for a
		service from _SCNetworkServiceCopyActive.
 */
Boolean
SCNetworkServiceSetPrimaryRank		(SCNetworkServiceRef		service,
					 SCNetworkServicePrimaryRank	newRank);

/* --------------------------------------------------------------------
 * SCNetworkSet
 * ------------------------------------------------------------------ */

/*!
	@function SCNetworkSetCopyAvailableInterfaces
	@discussion Returns the interfaces a service in this set could use,
		which leaves out Bond and Bridge members.
	@result a CFArray of SCNetworkInterfaceRef (caller releases).
 */
CFArrayRef
SCNetworkSetCopyAvailableInterfaces	(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetEstablishDefaultConfiguration
	@discussion Adds a service with the default configuration for each
		interface that does not have one.
	@result TRUE if the set was changed. On FALSE, SCError() is
		kSCStatusOK when there was nothing to add.
 */
Boolean
SCNetworkSetEstablishDefaultConfiguration
					(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetEstablishDefaultInterfaceConfiguration
	@discussion Adds a service with the default configuration for
		`interface` if it does not have one.
	@result TRUE if the set was changed.
 */
Boolean
SCNetworkSetEstablishDefaultInterfaceConfiguration
					(SCNetworkSetRef		set,
					 SCNetworkInterfaceRef		interface);

__END_DECLS

#endif	/* _SCNETWORKCONFIGURATIONPRIVATE_H */
