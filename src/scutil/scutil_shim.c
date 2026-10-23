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
 * scutil_shim.c — SCPrint and the SC description formatter behind it.
 *
 * Ported from apple-oss-distributions/configd @ configd-1405.120.5
 * (SystemConfiguration.fproj/SCD.c: SCPrint, __SCPrint,
 * _SCCopyDescription and its compare_* helpers), built as if
 * ENABLE_SC_FORMATTING were defined. On Darwin these live in the
 * framework; NextBSD's libSystemConfiguration does not carry them, so
 * scutil keeps its own copy (see scutil_shim.h).
 */

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "scutil_shim.h"

/* CoreFoundation SPI (ForFoundationOnly.h): CFStringCreateWithFormat
 * with a caller-supplied %@ description callout. */
extern CFStringRef	_CFStringCreateWithFormatAndArgumentsAux(CFAllocatorRef alloc,
								 CFStringRef (*copyDescFunc)(void *, const void *),
								 CFDictionaryRef formatOptions,
								 CFStringRef format,
								 va_list arguments);

static pthread_mutex_t	lock	= PTHREAD_MUTEX_INITIALIZER;

static CFStringRef	_SCCopyDescription(CFTypeRef cf, CFDictionaryRef formatOptions);


static CFComparisonResult
compare_CFDate(const void *val1, const void *val2, void *context)
{
#pragma unused(context)
	CFDateRef		str1	= (CFDateRef)val1;
	CFDateRef		str2	= (CFDateRef)val2;

	return CFDateCompare(str1, str2, 0);
}


static CFComparisonResult
compare_CFNumber(const void *val1, const void *val2, void *context)
{
#pragma unused(context)
	CFNumberRef		str1	= (CFNumberRef)val1;
	CFNumberRef		str2	= (CFNumberRef)val2;

	return CFNumberCompare(str1, str2, 0);
}


static CFComparisonResult
compare_CFString(const void *val1, const void *val2, void *context)
{
#pragma unused(context)
	CFStringRef		str1	= (CFStringRef)val1;
	CFStringRef		str2	= (CFStringRef)val2;

	return CFStringCompare(str1, str2, 0);
}


static CFStringRef
_SCCopyDescription(CFTypeRef cf, CFDictionaryRef formatOptions)
{
	CFMutableDictionaryRef	nFormatOptions;
	CFStringRef		prefix1;
	CFStringRef		prefix2;
	CFTypeID		type	= CFGetTypeID(cf);

	if (!formatOptions ||
	    !CFDictionaryGetValueIfPresent(formatOptions, CFSTR("PREFIX1"), (const void **)&prefix1)) {
		prefix1 = CFSTR("");
	}

	if (type == CFStringGetTypeID()) {
		return CFStringCreateWithFormat(NULL,
						formatOptions,
						CFSTR("%@%@"),
						prefix1,
						cf);
	}

	if (type == CFBooleanGetTypeID()) {
		return CFStringCreateWithFormat(NULL,
						formatOptions,
						CFSTR("%@%s"),
						prefix1,
						CFBooleanGetValue(cf) ? "TRUE" : "FALSE");
	}

	if (type == CFDataGetTypeID()) {
		const uint8_t		*data;
		CFIndex			dataLen;
		CFIndex			i;
		CFMutableStringRef	str;

		str = CFStringCreateMutable(NULL, 0);
		CFStringAppendFormat(str, formatOptions, CFSTR("%@<data> 0x"), prefix1);

		data    = CFDataGetBytePtr(cf);
		dataLen = CFDataGetLength(cf);
		for (i = 0; i < dataLen; i++) {
			CFStringAppendFormat(str, NULL, CFSTR("%02x"), data[i]);
		}

		return str;
	}

	if (type == CFNumberGetTypeID()) {
		return CFStringCreateWithFormat(NULL,
						formatOptions,
						CFSTR("%@%@"),
						prefix1,
						cf);
	}

	if (type == CFDateGetTypeID()) {
		CFCalendarRef	calendar;
		CFStringRef	str;
		CFTimeZoneRef	tz;
		int		MM, DD, YYYY, hh, mm, ss;

		calendar = CFCalendarCreateWithIdentifier(NULL, kCFGregorianCalendar);
		tz = CFTimeZoneCopySystem();
		CFCalendarSetTimeZone(calendar, tz);
		CFRelease(tz);
		CFCalendarDecomposeAbsoluteTime(calendar,
						CFDateGetAbsoluteTime(cf),
						"MdyHms",
						&MM, &DD, &YYYY, &hh, &mm, &ss);
		CFRelease(calendar);

		str = CFStringCreateWithFormat(NULL,
					       formatOptions,
					       CFSTR("%@%02d/%02d/%04d %02d:%02d:%02d"),
					       prefix1,
					       MM, DD, YYYY, hh, mm, ss);
		return str;
	}

	if ((formatOptions == NULL) ||
	    !CFDictionaryGetValueIfPresent(formatOptions, CFSTR("PREFIX2"), (const void **)&prefix2)) {
		prefix2 = prefix1;
	}

	if (formatOptions != NULL) {
		nFormatOptions = CFDictionaryCreateMutableCopy(NULL, 0, formatOptions);
	} else {
		nFormatOptions = CFDictionaryCreateMutable(NULL,
							   0,
							   &kCFTypeDictionaryKeyCallBacks,
							   &kCFTypeDictionaryValueCallBacks);
	}
	assert(nFormatOptions != NULL);

#define	N_QUICK	32

	if (type == CFArrayGetTypeID()) {
		const void *		elements_q[N_QUICK];
		const void **		elements	= elements_q;
		CFIndex			i;
		CFIndex			nElements;
		CFMutableStringRef	str;

		str = CFStringCreateMutable(NULL, 0);
		CFStringAppendFormat(str, formatOptions, CFSTR("%@<array> {"), prefix1);

		nElements = CFArrayGetCount(cf);
		if (nElements > 0) {
			if (nElements > (CFIndex)(sizeof(elements_q)/sizeof(CFTypeRef)))
				elements  = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
			CFArrayGetValues(cf, CFRangeMake(0, nElements), elements);
			for (i = 0; i < nElements; i++) {
				CFMutableStringRef	nPrefix1;
				CFMutableStringRef	nPrefix2;
				CFStringRef		nStr;
				CFStringRef		vStr;

				nStr = CFStringCreateWithFormat(NULL, NULL, CFSTR("%ld"), i);

				nPrefix1 = CFStringCreateMutable(NULL, 0);
				CFStringAppendFormat(nPrefix1,
						     formatOptions,
						     CFSTR("%@  %@ : "),
						     prefix2,
						     nStr);
				nPrefix2 = CFStringCreateMutable(NULL, 0);
				CFStringAppendFormat(nPrefix2,
						     formatOptions,
						     CFSTR("%@  "),
						     prefix2);

				CFDictionarySetValue(nFormatOptions, CFSTR("PREFIX1"), nPrefix1);
				CFDictionarySetValue(nFormatOptions, CFSTR("PREFIX2"), nPrefix2);
				CFRelease(nPrefix1);
				CFRelease(nPrefix2);
				CFRelease(nStr);

				vStr = _SCCopyDescription((CFTypeRef)elements[i], nFormatOptions);
				CFStringAppendFormat(str,
						     formatOptions,
						     CFSTR("\n%@"),
						     vStr);
				CFRelease(vStr);
			}
			if (elements != elements_q) CFAllocatorDeallocate(NULL, elements);
		}
		CFStringAppendFormat(str, formatOptions, CFSTR("\n%@}"), prefix2);

		CFRelease(nFormatOptions);
		return str;
	}

	if (type == CFDictionaryGetTypeID()) {
		const void *		keys_q[N_QUICK];
		const void **		keys	= keys_q;
		CFIndex			i;
		CFIndex			nElements;
		CFMutableStringRef	nPrefix1;
		CFMutableStringRef	nPrefix2;
		CFMutableStringRef	str;

		str = CFStringCreateMutable(NULL, 0);
		CFStringAppendFormat(str, formatOptions, CFSTR("%@<dictionary> {"), prefix1);

		nElements = CFDictionaryGetCount(cf);
		if (nElements > 0) {
			CFComparatorFunction	compFunc	= NULL;
			CFMutableArrayRef	sortedKeys;

			if (nElements > (CFIndex)(sizeof(keys_q) / sizeof(CFTypeRef))) {
				keys = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
			}
			CFDictionaryGetKeysAndValues(cf, keys, NULL);

			sortedKeys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
			for (i = 0; i < nElements; i++) {
				CFArrayAppendValue(sortedKeys, (CFStringRef)keys[i]);
			}

			if (isA_CFString(keys[0])) {
				compFunc = compare_CFString;
			}
			else if (isA_CFNumber(keys[0])) {
				compFunc = compare_CFNumber;
			}
			else if (isA_CFDate(keys[0])) {
				compFunc = compare_CFDate;
			}

			if (compFunc != NULL) {
				CFArraySortValues(sortedKeys,
						  CFRangeMake(0, nElements),
						  compFunc,
						  NULL);
			}

			for (i = 0; i < nElements; i++) {
				CFStringRef		key;
				CFStringRef		kStr;
				CFTypeRef		val;
				CFStringRef		vStr;

				key  = CFArrayGetValueAtIndex(sortedKeys, i);
				kStr = _SCCopyDescription((CFTypeRef)key, NULL);

				nPrefix1 = CFStringCreateMutable(NULL, 0);
				CFStringAppendFormat(nPrefix1,
						     formatOptions,
						     CFSTR("%@  %@ : "),
						     prefix2,
						     kStr);
				nPrefix2 = CFStringCreateMutable(NULL, 0);
				CFStringAppendFormat(nPrefix2,
						     formatOptions,
						     CFSTR("%@  "),
						     prefix2);

				CFDictionarySetValue(nFormatOptions, CFSTR("PREFIX1"), nPrefix1);
				CFDictionarySetValue(nFormatOptions, CFSTR("PREFIX2"), nPrefix2);
				CFRelease(nPrefix1);
				CFRelease(nPrefix2);
				CFRelease(kStr);

				val  = CFDictionaryGetValue(cf, key);
				vStr = _SCCopyDescription((CFTypeRef)val, nFormatOptions);
				CFStringAppendFormat(str,
						     formatOptions,
						     CFSTR("\n%@"),
						     vStr);
				CFRelease(vStr);
			}

			CFRelease(sortedKeys);

			if (keys != keys_q) {
				CFAllocatorDeallocate(NULL, keys);
			}
		}
		CFStringAppendFormat(str, formatOptions, CFSTR("\n%@}"), prefix2);

		CFRelease(nFormatOptions);
		return str;
	}

	CFRelease(nFormatOptions);

	return CFStringCreateWithFormat(NULL,
					formatOptions,
					CFSTR("%@%@"),
					prefix1,
					cf);
}


static void
__SCPrint(FILE *stream, CFStringRef formatString, va_list formatArguments, Boolean addTime, Boolean addNL)
{
	UInt8		*line;
	CFStringRef	str;
	CFIndex		usedBufLen	= 0;
	CFRange		range;

	str = _CFStringCreateWithFormatAndArgumentsAux(NULL,
						       (CFStringRef (*)(void *, const void *))_SCCopyDescription,
						       NULL,
						       formatString,
						       formatArguments);
	if (str == NULL) {
		return;
	}

	/* _SC_cfstring_to_cstring_ext(str, NULL, 0, kCFStringEncodingUTF8, '?', &usedBufLen) */
	range = CFRangeMake(0, CFStringGetLength(str));
	(void)CFStringGetBytes(str, range, kCFStringEncodingUTF8, (UInt8)'?',
			       FALSE, NULL, 0, &usedBufLen);
	line = malloc(usedBufLen + 1);
	if (line == NULL) {
		CFRelease(str);
		return;
	}
	(void)CFStringGetBytes(str, range, kCFStringEncodingUTF8, (UInt8)'?',
			       FALSE, line, usedBufLen, &usedBufLen);
	line[usedBufLen] = '\0';
	CFRelease(str);

	pthread_mutex_lock(&lock);
	if (addTime) {
		struct tm	tm_now;
		struct timeval	tv_now;

		(void)gettimeofday(&tv_now, NULL);
		(void)localtime_r(&tv_now.tv_sec, &tm_now);
		(void)fprintf(stream, "%2d:%02d:%02d.%03d ",
			      tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, (int)(tv_now.tv_usec / 1000));
	}
	(void)fwrite((const void *)line, usedBufLen, 1, stream);
	if (addNL) {
		(void)fputc('\n', stream);
	}
	fflush (stream);
	pthread_mutex_unlock(&lock);
	free(line);

	return;
}


void
SCPrint(Boolean condition, FILE *stream, CFStringRef formatString, ...)
{
	va_list		formatArguments;

	if (!condition) {
		return;
	}

	va_start(formatArguments, formatString);
	__SCPrint(stream, formatString, formatArguments, FALSE, FALSE);
	va_end(formatArguments);

	return;
}
