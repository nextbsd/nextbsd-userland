/*
 * IOPMLibPrivate.h — Private Power Management assertion definitions for NextBSD.
 *
 * Matching nextbsd-userland#233 / #236.
 */

#ifndef _IOKIT_PWR_MGT_IOPMLIBPRIVATE_H_
#define _IOKIT_PWR_MGT_IOPMLIBPRIVATE_H_

#include <IOKit/pwr_mgt/IOPMLib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Assertion property keys
 */
#define kIOPMAssertionOnBehalfOfPID \
    CFSTR("OnBehalfOfPID")

/*
 * IOPMSleepSystemWithOptions (nextbsd-userland#234): IOPMSleepSystem with an
 * options dictionary whose "Sleep Reason" (kIOPMSleepReasonKey) names the
 * reason the root domain records as "Last Sleep Reason"; Apple's code builds
 * it as CFSTR("Sleep Reason") -> CFSTR(kIOPMIdleSleepKey). "Idle Sleep" asks
 * kIOMessageCanSystemSleep first, so a client can veto it. Without the key,
 * or with another value, it is a software sleep. The keys are C strings, as
 * in xnu's IOPMPrivate.h, and guarded so that header can come in too.
 */
#ifndef kIOPMSleepReasonKey
#define kIOPMSleepReasonKey             "Sleep Reason"
#endif
#ifndef kIOPMClamshellSleepKey
#define kIOPMClamshellSleepKey          "Clamshell Sleep"
#endif
#ifndef kIOPMPowerButtonSleepKey
#define kIOPMPowerButtonSleepKey        "Power Button Sleep"
#endif
#ifndef kIOPMSoftwareSleepKey
#define kIOPMSoftwareSleepKey           "Software Sleep"
#endif
#ifndef kIOPMIdleSleepKey
#define kIOPMIdleSleepKey               "Idle Sleep"
#endif
#ifndef kIOPMLowPowerSleepKey
#define kIOPMLowPowerSleepKey           "Low Power Sleep"
#endif
#ifndef kIOPMThermalEmergencySleepKey
#define kIOPMThermalEmergencySleepKey   "Thermal Emergency Sleep"
#endif
#ifndef kIOPMMaintenanceSleepKey
#define kIOPMMaintenanceSleepKey        "Maintenance Sleep"
#endif

IOReturn IOPMSleepSystemWithOptions(
    io_connect_t        fb,
    CFDictionaryRef     options);

#ifdef __cplusplus
}
#endif

#endif /* _IOKIT_PWR_MGT_IOPMLIBPRIVATE_H_ */
