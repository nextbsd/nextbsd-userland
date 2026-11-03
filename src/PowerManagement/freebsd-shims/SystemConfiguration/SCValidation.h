/*
 * SystemConfiguration/SCValidation.h — NextBSD shim for pmconfigd.
 *
 * Apple's SCValidation.h is a private SystemConfiguration header of
 * type-check helpers: isA_CFString(), isA_CFDictionary() and so on, each
 * "non-NULL and of that CF type, else NULL". NextBSD's libSystemConfiguration
 * does not install it. Apple's PMStore.c includes it by name; the helpers are
 * the same #ifndef-guarded set PrivateLib_freebsd.h carries, so whichever
 * header comes first defines them.
 */
#ifndef _PM_SHIM_SCVALIDATION_H_
#define _PM_SHIM_SCVALIDATION_H_

#include <CoreFoundation/CoreFoundation.h>

/* isA_CFType is a function, so a separate macro guards the set. */
#ifndef _PM_SHIM_ISA_CFTYPE
#define _PM_SHIM_ISA_CFTYPE
static __inline CFTypeRef
isA_CFType(CFTypeRef obj, CFTypeID type)
{
	if (obj == NULL || CFGetTypeID(obj) != type)
		return NULL;
	return obj;
}
#define isA_CFArray(obj)	((CFArrayRef)isA_CFType((obj), CFArrayGetTypeID()))
#define isA_CFBoolean(obj)	((CFBooleanRef)isA_CFType((obj), CFBooleanGetTypeID()))
#define isA_CFData(obj)		((CFDataRef)isA_CFType((obj), CFDataGetTypeID()))
#define isA_CFDate(obj)		((CFDateRef)isA_CFType((obj), CFDateGetTypeID()))
#define isA_CFDictionary(obj)	((CFDictionaryRef)isA_CFType((obj), CFDictionaryGetTypeID()))
#define isA_CFNumber(obj)	((CFNumberRef)isA_CFType((obj), CFNumberGetTypeID()))
#define isA_CFString(obj)	((CFStringRef)isA_CFType((obj), CFStringGetTypeID()))
#endif

#endif /* _PM_SHIM_SCVALIDATION_H_ */
