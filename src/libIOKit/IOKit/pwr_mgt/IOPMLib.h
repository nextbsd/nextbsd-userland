/*
 * IOPMLib.h — Power Management assertion API stub for NextBSD.
 *
 * Provides Apple IOPMLib definitions and prototypes for assertion clients
 * such as caffeinate(8) and pmset(1), matching nextbsd-userland#233 / #236.
 *
 * System power (nextbsd-userland#234): IORegisterForSystemPower and the calls
 * around it, against the in-kernel IOPMrootDomain (IOPMLib.c).
 */

#ifndef _IOKIT_PWR_MGT_IOPMLIB_H_
#define _IOKIT_PWR_MGT_IOPMLIB_H_

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>

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

/*
 * System power ------------------------------------------------------------
 *
 * Apple's IOPMLib calls against the root domain, which on NextBSD is in the
 * kernel too (nextbsd-kernel#243). The io_connect_t is a descriptor on
 * /dev/iopmroot standing in for the root domain's user client; close it with
 * IOServiceClose.
 *
 * IORegisterForSystemPower — register `callback` for the system sleep/wake
 * messages, delivered through the new IONotificationPort returned in
 * *thePortRef (add IONotificationPortGetRunLoopSource to a run loop, or set
 * a dispatch queue). Returns the root domain connection to answer with, and
 * the registration in *notifier; IO_OBJECT_NULL on failure, including on a
 * kernel without the root domain. The callback gets messageArgument ==
 * notificationID:
 *
 *   kIOMessageCanSystemSleep      idle sleep is proposed: IOAllowPowerChange,
 *                                 or IOCancelPowerChange to veto it
 *   kIOMessageSystemWillSleep     the system is going to sleep, no veto:
 *                                 IOAllowPowerChange once ready
 *   kIOMessageSystemWillNotSleep  a proposed sleep was called off
 *   kIOMessageSystemWillPowerOn   waking, before devices are back
 *   kIOMessageSystemHasPoweredOn  awake
 *
 * The root domain waits up to 30 s (kern.pmtimeout) for each answer.
 *
 * IODeregisterForSystemPower — release *notifier and clear it. As on Darwin
 * the caller still closes the connection and destroys the port.
 */
io_connect_t IORegisterForSystemPower(
    void                        *refcon,
    IONotificationPortRef       *thePortRef,
    IOServiceInterestCallback   callback,
    io_object_t                 *notifier);

IOReturn IODeregisterForSystemPower(
    io_object_t                 *notifier);

IOReturn IOAllowPowerChange(
    io_connect_t                kernelPort,
    intptr_t                    notificationID);

IOReturn IOCancelPowerChange(
    io_connect_t                kernelPort,
    intptr_t                    notificationID);

/*
 * IOPMFindPowerManagement — a root domain connection without registering;
 * IO_OBJECT_NULL without the root domain. IOPMSleepSystem then puts the
 * system to sleep ("Software Sleep"): it returns at once and the
 * sleep/wake messages follow. Root only (kIOReturnNotPrivileged);
 * kIOReturnUnsupported where the machine cannot sleep (no ACPI S3, arm64),
 * kIOReturnNotPermitted while sleep is disabled.
 */
io_connect_t IOPMFindPowerManagement(
    mach_port_t                 master_device_port);

IOReturn IOPMSleepSystem(
    io_connect_t                fb);

/* kPMMinutesToSleep and the rest are stored for pmconfigd; setting is root
 * only. */
IOReturn IOPMSetAggressiveness(
    io_connect_t                fb,
    unsigned long               type,
    unsigned long               aggressiveness);

IOReturn IOPMGetAggressiveness(
    io_connect_t                fb,
    unsigned long               type,
    unsigned long               *aggressiveness);

boolean_t IOPMSleepEnabled(void);

/* The calendar time of the last wake (kern.waketime); kIOReturnNotReady if
 * the system has not slept since boot. */
IOReturn IOPMGetLastWakeTime(
    CFAbsoluteTime              *lastWakeTime,
    CFTimeInterval              *adjustedForPhysicalWake);

#ifdef __cplusplus
}
#endif

#endif /* _IOKIT_PWR_MGT_IOPMLIB_H_ */
