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
 * Modification History
 *
 * July 9, 2001			Allan Nathanson <ajn@apple.com>
 * - added "-r" option for checking network reachability
 * - added "-w" option to check/wait for the presence of a
 *   dynamic store key.
 *
 * June 1, 2001			Allan Nathanson <ajn@apple.com>
 * - public API conversion
 *
 * November 9, 2000		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

/*
 * NextBSD: of Apple's tests.c only the "-w" key wait is carried. The
 * rest backs options whose APIs our libSystemConfiguration does not
 * have yet -- "-r" (SCNetworkReachabilityCreateWithName/WithAddress),
 * "--dns" (dnsinfo), "--proxy" (SCDynamicStoreCopyProxies), "--nwi"
 * (network_information), "--renew"
 * (SCNetworkInterfaceForceConfigurationRefresh), "--snapshot",
 * "--advisory" and "--rank".
 */

#include <signal.h>
#include <sys/time.h>

#include "scutil.h"
#include "tests.h"


static void __attribute__((noreturn))
waitKeyFound(SCDynamicStoreRef session, CFArrayRef changes,
	     void * info)
{
#pragma unused(session, changes, info)
	/*
	 * NextBSD: exit as soon as the watched key changes, as configd-1109
	 * and earlier did. configd-1405's bare "return" leaves CFRunLoopRun
	 * spinning until the -t timeout, so a key that appears late would
	 * report failure.
	 */
	exit(0);
}

static void __attribute__((noreturn))
waitTimeout(int sigraised)
{
#pragma unused(sigraised)
	exit(1);
}

__private_extern__
void
do_wait(char *waitKey, int timeout)
{
	struct itimerval	itv;
	CFStringRef		key;
	CFMutableArrayRef	keys;
	Boolean			ok;

	store = SCDynamicStoreCreate(NULL, CFSTR("scutil (wait)"), (SCDynamicStoreCallBack)waitKeyFound, NULL);
	if (store == NULL) {
		SCPrint(TRUE, stderr,
			CFSTR("SCDynamicStoreCreate() failed: %s\n"), SCErrorString(SCError()));
		exit(1);
	}

	key  = CFStringCreateWithCString(NULL, waitKey, kCFStringEncodingUTF8);

	keys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(keys, key);
	ok = SCDynamicStoreSetNotificationKeys(store, keys, NULL);
	CFRelease(keys);
	if (!ok) {
		SCPrint(TRUE, stderr,
			CFSTR("SCDynamicStoreSetNotificationKeys() failed: %s\n"), SCErrorString(SCError()));
		exit(1);
	}

	notifyRls = SCDynamicStoreCreateRunLoopSource(NULL, store, 0);
	if (!notifyRls) {
		SCPrint(TRUE, stderr,
			CFSTR("SCDynamicStoreCreateRunLoopSource() failed: %s\n"), SCErrorString(SCError()));
		exit(1);
	}

	CFRunLoopAddSource(CFRunLoopGetCurrent(), notifyRls, kCFRunLoopDefaultMode);

	value = SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	if (value) {
		/* if the key is already present */
		return;
	}

	if (timeout > 0) {
		signal(SIGALRM, waitTimeout);
		memset(&itv, 0, sizeof(itv));
		itv.it_value.tv_sec = timeout;
		if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {
			SCPrint(TRUE, stderr,
				CFSTR("setitimer() failed: %s\n"), strerror(errno));
			exit(1);
		}
	}

	CFRunLoopRun();
}
