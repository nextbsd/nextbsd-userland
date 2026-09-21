/*
 * IOPMLib.h — Power Management assertion API stub for NextBSD.
 *
 * Provides Apple IOPMLib definitions and prototypes for assertion clients
 * such as caffeinate(8) and pmset(1), matching nextbsd-userland#233 / #236.
 */

#ifndef _IOKIT_PWR_MGT_IOPMLIB_H_
#define _IOKIT_PWR_MGT_IOPMLIB_H_

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t IOPMAssertionID;
typedef uint32_t IOPMAssertionLevel;

#define kIOPMAssertionLevelOff  0
#define kIOPMAssertionLevelOn   255

/*
 * Assertion Types
 */
#define kIOPMAssertionTypePreventUserIdleSystemSleep \
    CFSTR("PreventUserIdleSystemSleep")

#define kIOPMAssertionTypePreventUserIdleDisplaySleep \
    CFSTR("PreventUserIdleDisplaySleep")

#define kIOPMAssertionTypePreventSystemSleep \
    CFSTR("PreventSystemSleep")

#define kIOPMAssertionUserIsActive \
    CFSTR("UserIsActive")

#define kIOPMAssertPreventDiskIdle \
    CFSTR("PreventDiskIdle")

/*
 * Timeout actions
 */
#define kIOPMAssertionTimeoutActionRelease \
    CFSTR("TimeoutActionRelease")

#define kIOPMAssertionTimeoutActionLog \
    CFSTR("TimeoutActionLog")

#define kIOPMAssertionTimeoutActionTurnOff \
    CFSTR("TimeoutActionTurnOff")

/*
 * Functions
 */
IOReturn IOPMAssertionCreateWithDescription(
    CFStringRef         AssertionType,
    CFStringRef         Name,
    CFStringRef         Details,
    CFStringRef         HumanReadableReason,
    CFStringRef         LocalizationBundlePath,
    CFTimeInterval      Timeout,
    CFStringRef         TimeoutAction,
    IOPMAssertionID     *AssertionID);

IOReturn IOPMAssertionRelease(
    IOPMAssertionID     AssertionID);

IOReturn IOPMAssertionRetain(
    IOPMAssertionID     AssertionID);

IOReturn IOPMAssertionSetProperty(
    IOPMAssertionID     AssertionID,
    CFStringRef         PropertyKey,
    CFTypeRef           PropertyValue);

#ifdef __cplusplus
}
#endif

#endif /* _IOKIT_PWR_MGT_IOPMLIB_H_ */
