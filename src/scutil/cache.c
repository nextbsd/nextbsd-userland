/*
 * Copyright (c) 2000-2005, 2011, 2012, 2017, 2018, 2020 Apple Inc. All rights reserved.
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
 * June 1, 2001			Allan Nathanson <ajn@apple.com>
 * - public API conversion
 *
 * November 9, 2000		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

#include <sys/types.h>

#include "scutil.h"
#include "cache.h"

/*
 * NextBSD: Apple's "block" command batches store operations in the
 * private SCDynamicStore transaction cache (_SCDynamicStoreCache*,
 * SCDynamicStoreInternal.h). Our libSystemConfiguration has no such
 * cache, so "block" and the cache-aware paths in list / add / show are
 * dropped and every operation goes straight to configd.
 */

#define	__SC_CFRELEASE(cf)	do { if ((cf) != NULL) { CFRelease(cf); (cf) = NULL; } } while (0)


#pragma mark -
#pragma mark SCDynamicStore operations


static CFComparisonResult
sort_keys(const void *p1, const void *p2, void *context)
{
#pragma unused(context)
	CFStringRef key1 = (CFStringRef)p1;
	CFStringRef key2 = (CFStringRef)p2;
	return CFStringCompare(key1, key2, 0);
}


__private_extern__
void
do_list(int argc, char * const argv[])
{
	int				i;
	CFStringRef			pattern;
	CFArrayRef			list;
	CFIndex				listCnt;
	CFMutableArrayRef		sortedList;

	pattern = CFStringCreateWithCString(NULL,
					    (argc >= 1) ? argv[0] : ".*",
					    kCFStringEncodingUTF8);
	if (pattern == NULL) {
		SCPrint(TRUE, stdout, CFSTR("Failed to allocate pattern\n"));
		return;
	}
	list = SCDynamicStoreCopyKeyList(store, pattern);
	CFRelease(pattern);
	if (list == NULL) {
		if (SCError() != kSCStatusOK) {
			SCPrint(TRUE, stdout, CFSTR("  %s\n"), SCErrorString(SCError()));
		} else {
			SCPrint(TRUE, stdout, CFSTR("  no keys.\n"));
		}
		return;
	}

	listCnt = CFArrayGetCount(list);
	sortedList = CFArrayCreateMutableCopy(NULL, listCnt, list);
	__SC_CFRELEASE(list);
	CFArraySortValues(sortedList,
			  CFRangeMake(0, listCnt),
			  sort_keys,
			  NULL);

	if (listCnt > 0) {
		for (i = 0; i < listCnt; i++) {
			SCPrint(TRUE,
				stdout,
				CFSTR("  subKey [%d] = %@\n"),
				i,
				CFArrayGetValueAtIndex(sortedList, i));
		}
	} else {
		SCPrint(TRUE, stdout, CFSTR("  no keys.\n"));
	}
	__SC_CFRELEASE(sortedList);

	return;
}


__private_extern__
void
do_add(int argc, char * const argv[])
{
#pragma unused(argc)
	CFStringRef	key;

	key    = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);

	if (!SCDynamicStoreAddValue(store, key, value)) {
		SCPrint(TRUE, stdout, CFSTR("  %s\n"), SCErrorString(SCError()));
	}

	__SC_CFRELEASE(key);
	return;
}


__private_extern__
void
do_get(int argc, char * const argv[])
{
#pragma unused(argc)
	CFStringRef		key;
	CFPropertyListRef	newValue;

	key      = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);
	newValue = SCDynamicStoreCopyValue(store, key);
	__SC_CFRELEASE(key);
	if (newValue == NULL) {
		SCPrint(TRUE, stdout, CFSTR("  %s\n"), SCErrorString(SCError()));
		return;
	}
	__SC_CFRELEASE(value);		/* we have new information, release the old */
	value = newValue;

	return;
}


__private_extern__
void
do_set(int argc, char * const argv[])
{
#pragma unused(argc)
	CFStringRef	key;

	key = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);
	if (!SCDynamicStoreSetValue(store, key, value)) {
		SCPrint(TRUE, stdout, CFSTR("  %s\n"), SCErrorString(SCError()));
	}
	__SC_CFRELEASE(key);
	return;
}


__private_extern__
void
do_show(int argc, char * const argv[])
{
	CFStringRef		key;
	CFPropertyListRef	newValue;

	key = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);

	if (argc == 1) {
		newValue = SCDynamicStoreCopyValue(store, key);
	} else {
		CFArrayRef	patterns;

		patterns = CFArrayCreate(NULL, (const void **)&key, 1, &kCFTypeArrayCallBacks);
		newValue = SCDynamicStoreCopyMultiple(store, NULL, patterns);
		__SC_CFRELEASE(patterns);
	}

	__SC_CFRELEASE(key);
	if (newValue == NULL) {
		SCPrint(TRUE, stdout, CFSTR("  %s\n"), SCErrorString(SCError()));
		return;
	}

	SCPrint(TRUE, stdout, CFSTR("%@\n"), newValue);
	__SC_CFRELEASE(newValue);
	return;
}


__private_extern__
void
do_remove(int argc, char * const argv[])
{
#pragma unused(argc)
	CFStringRef	key;

	key = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);
	if (!SCDynamicStoreRemoveValue(store, key)) {
		SCPrint(TRUE, stdout, CFSTR("  %s\n"), SCErrorString(SCError()));
	}
	__SC_CFRELEASE(key);
	return;
}


__private_extern__
void
do_notify(int argc, char * const argv[])
{
#pragma unused(argc)
	CFStringRef	key;

	key = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);
	if (!SCDynamicStoreNotifyValue(store, key)) {
		SCPrint(TRUE, stdout, CFSTR("  %s\n"), SCErrorString(SCError()));
	}
	__SC_CFRELEASE(key);
	return;
}
