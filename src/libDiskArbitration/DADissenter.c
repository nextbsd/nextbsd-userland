/*
 * DADissenter.c — libDiskArbitration's dissenter (nextbsd-userland#197).
 *
 * Port of Apple's DiskArbitration/DADissenter.c
 * (DiskArbitration-535.120.1.0.1): a dissenter is a CFDictionary with a
 * status, an optional status string and the dissenting process ID, which
 * is what crosses the wire in both directions.
 */
#include "DAClient.h"

#include <unistd.h>

DADissenterRef
DADissenterCreate(CFAllocatorRef allocator, DAReturn status,
    CFStringRef string)
{
	CFMutableDictionaryRef dissenter;

	dissenter = CFDictionaryCreateMutable(allocator, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (dissenter == NULL)
		return (NULL);
	___CFDictionarySetIntegerValue(dissenter, _kDADissenterProcessIDKey,
	    getpid());
	___CFDictionarySetIntegerValue(dissenter, _kDADissenterStatusKey,
	    (SInt64)(uint32_t)status);
	if (string != NULL)
		CFDictionarySetValue(dissenter, _kDADissenterStatusStringKey,
		    string);
	return ((DADissenterRef)dissenter);
}

pid_t
DADissenterGetProcessID(DADissenterRef dissenter)
{
	return ((pid_t)___CFDictionaryGetIntegerValue((CFDictionaryRef)dissenter,
	    _kDADissenterProcessIDKey));
}

DAReturn
DADissenterGetStatus(DADissenterRef dissenter)
{
	return ((DAReturn)(uint32_t)___CFDictionaryGetIntegerValue(
	    (CFDictionaryRef)dissenter, _kDADissenterStatusKey));
}

CFStringRef
DADissenterGetStatusString(DADissenterRef dissenter)
{
	CFTypeRef string;

	string = CFDictionaryGetValue((CFDictionaryRef)dissenter,
	    _kDADissenterStatusStringKey);
	if (string != NULL && CFGetTypeID(string) != CFStringGetTypeID())
		return (NULL);
	return ((CFStringRef)string);
}
