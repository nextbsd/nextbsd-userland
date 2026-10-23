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
 * scutil_shim.h — the SystemConfiguration SPI scutil expects from
 * <SystemConfiguration/SCPrivate.h> and <SystemConfiguration/SCValidation.h>.
 *
 * NextBSD's libSystemConfiguration does not install those private
 * headers, so the small subset scutil uses lives here, local to the
 * tool: the isA_CF* type checks (static inlines, as in Apple's
 * SCValidation.h) and SCPrint with Apple's SC-formatted descriptions
 * (scutil_shim.c, from SCD.c). Same arrangement as hostnamed's
 * freebsd-shim.
 */

#ifndef _SCUTIL_SHIM_H
#define _SCUTIL_SHIM_H

#include <sys/cdefs.h>
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>

#ifndef __private_extern__
#define	__private_extern__	__attribute__((__visibility__("hidden")))
#endif

#define	_SCUTIL_ISA(BASE, TYPE, TYPEID_FN)				\
	static inline TYPE						\
	isA_ ## BASE(CFTypeRef cf)					\
	{								\
		return ((cf != NULL && CFGetTypeID(cf) == TYPEID_FN()) ? \
		    (TYPE)cf : NULL);					\
	}

_SCUTIL_ISA(CFArray, CFArrayRef, CFArrayGetTypeID)
_SCUTIL_ISA(CFBoolean, CFBooleanRef, CFBooleanGetTypeID)
_SCUTIL_ISA(CFData, CFDataRef, CFDataGetTypeID)
_SCUTIL_ISA(CFDate, CFDateRef, CFDateGetTypeID)
_SCUTIL_ISA(CFDictionary, CFDictionaryRef, CFDictionaryGetTypeID)
_SCUTIL_ISA(CFNumber, CFNumberRef, CFNumberGetTypeID)
_SCUTIL_ISA(CFString, CFStringRef, CFStringGetTypeID)

#undef	_SCUTIL_ISA

__BEGIN_DECLS

/*
 * SCPrint — print a CFString format to `stream` when `condition` is
 * TRUE. CF objects passed for %@ are described in SC's indented
 * "<dictionary> { key : value }" form, matching scutil on Darwin.
 */
void	SCPrint		(Boolean	condition,
			 FILE		*stream,
			 CFStringRef	formatString,
			 ...)		CF_FORMAT_FUNCTION(3, 4);

__END_DECLS

#endif	/* !_SCUTIL_SHIM_H */
