/*
 * Copyright (c) 2003-2013 Apple Inc. All rights reserved.
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
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * From apple-oss-distributions/configd @ configd-596.15
 * SystemConfiguration.fproj/SCNetworkReachabilityInternal.h: only the flag
 * printer scutil uses. The struct behind SCNetworkReachabilityRef stays in
 * libSystemConfiguration; scutil gets the name of a by-name target through
 * __SCNetworkReachabilityGetTargetName().
 */

#ifndef _SCNETWORKREACHABILITYINTERNAL_H
#define _SCNETWORKREACHABILITYINTERNAL_H

#include <stdio.h>
#include <SystemConfiguration/SCNetworkReachability.h>

#include "scutil_shim.h"

static __inline__ void
__SCNetworkReachabilityPrintFlags(SCNetworkReachabilityFlags flags)
{
	if (flags != 0) {
		if (flags & kSCNetworkReachabilityFlagsReachable) {
			SCPrint(TRUE, stdout, CFSTR("Reachable"));
			flags &= ~kSCNetworkReachabilityFlagsReachable;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags & kSCNetworkReachabilityFlagsTransientConnection) {
			SCPrint(TRUE, stdout, CFSTR("Transient Connection"));
			flags &= ~kSCNetworkReachabilityFlagsTransientConnection;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags & kSCNetworkReachabilityFlagsConnectionRequired) {
			SCPrint(TRUE, stdout, CFSTR("Connection Required"));
			flags &= ~kSCNetworkReachabilityFlagsConnectionRequired;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags & kSCNetworkReachabilityFlagsConnectionOnTraffic) {
			SCPrint(TRUE, stdout, CFSTR("Automatic Connection On Traffic"));
			flags &= ~kSCNetworkReachabilityFlagsConnectionOnTraffic;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags & kSCNetworkReachabilityFlagsConnectionOnDemand) {
			SCPrint(TRUE, stdout, CFSTR("Automatic Connection On Demand"));
			flags &= ~kSCNetworkReachabilityFlagsConnectionOnDemand;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags & kSCNetworkReachabilityFlagsInterventionRequired) {
			SCPrint(TRUE, stdout, CFSTR("Intervention Required"));
			flags &= ~kSCNetworkReachabilityFlagsInterventionRequired;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags & kSCNetworkReachabilityFlagsIsLocalAddress) {
			SCPrint(TRUE, stdout, CFSTR("Local Address"));
			flags &= ~kSCNetworkReachabilityFlagsIsLocalAddress;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags & kSCNetworkReachabilityFlagsIsDirect) {
			SCPrint(TRUE, stdout, CFSTR("Directly Reachable Address"));
			flags &= ~kSCNetworkReachabilityFlagsIsDirect;
			SCPrint(flags != 0, stdout, CFSTR(","));
		}
		if (flags != 0) {
			SCPrint(TRUE, stdout, CFSTR("0x%08x"), flags);
		}
	} else {
		SCPrint(TRUE, stdout, CFSTR("Not Reachable"));
	}

	return;
}

#endif // _SCNETWORKREACHABILITYINTERNAL_H
