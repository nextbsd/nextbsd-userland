/*
 * PrivateLib_freebsd.h — the part of pmconfigd's PrivateLib.h that the
 * ported Apple files use, until PrivateLib.h itself builds.
 *
 * Every pmconfigd source includes "PrivateLib.h". That header pulls in the
 * whole of IOKit's private power-management SDK (IOPMPrivate.h,
 * IOPMLibPrivate.h, IOPowerSourcesPrivate.h, IOReportMacros.h ...),
 * SystemConfiguration's private headers and PMAssertions.h, most of which
 * NextBSD does not have yet. A ported Apple file includes this header in
 * its place, under __FreeBSD__:
 *
 *	#ifdef __FreeBSD__
 *	#include "PrivateLib_freebsd.h"
 *	#else
 *	#include "PrivateLib.h"
 *	#endif
 *
 * Each block below names the Apple header it copies from; values and names
 * are Apple's (IOKitUser-100231.120.3, PowerManagement-1846.120.8.0.1, and
 * the macOS SDK's IOKit/pwr_mgt/IOPM.h). Definitions that libIOKit or
 * libSystemConfiguration may publish later are #ifndef-guarded, so the SDK
 * copy wins once it exists. When PrivateLib.h builds, the #ifdef above goes
 * away and so does this file. Add to it as more Apple files are ported.
 *
 * The functions declared at the end live in PrivateLib_freebsd.c.
 */
#ifndef _PM_SHIM_PRIVATELIB_FREEBSD_H_
#define _PM_SHIM_PRIVATELIB_FREEBSD_H_

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <IOKit/pwr_mgt/powermanagement_mig.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <os/log.h>

#include <stdbool.h>
#include <stdint.h>

/* ---- IOKit/IOReturn.h: the codes libIOKit's IOKitLib.h does not define -- */
/*
 * Same numbering as libIOKit's (the low 14 bits of Apple's values), so a
 * libIOKit client compares them correctly. Note that libIOKit's own
 * kIOReturnBadArgument is 0x2c9, Apple's kIOReturnInternalError; the two
 * are indistinguishable until libIOKit's table is corrected.
 */
#ifndef kIOReturnNoMemory
#define kIOReturnNoMemory		0x2bd
#endif
#ifndef kIOReturnNotPrivileged
#define kIOReturnNotPrivileged		0x2c1
#endif
#ifndef kIOReturnInternalError
#define kIOReturnInternalError		0x2c9
#endif
#ifndef kIOReturnNoSpace
#define kIOReturnNoSpace		0x2db
#endif
#ifndef kIOReturnIsoTooOld
#define kIOReturnIsoTooOld		0x2ee
#endif

/* ---- SystemConfiguration/SCValidation.h -------------------------------- */
#ifndef isA_CFType
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

/* ---- IOKit/pwr_mgt/IOPM.h (public SDK) --------------------------------- */
#ifndef _IOKIT_IOPM_H
struct IOPMSystemCapabilityChangeParameters {
	uint32_t	notifyRef;
	uint32_t	maxWaitForReply;
	uint32_t	changeFlags;
	uint32_t	__reserved1;
	uint32_t	fromCapabilities;
	uint32_t	toCapabilities;
	uint32_t	__reserved2[4];
};

enum {
	kIOPMSystemCapabilityWillChange = 0x01,
	kIOPMSystemCapabilityDidChange  = 0x02
};

enum {
	kIOPMSystemCapabilityCPU        = 0x01,
	kIOPMSystemCapabilityGraphics   = 0x02,
	kIOPMSystemCapabilityAudio      = 0x04,
	kIOPMSystemCapabilityNetwork    = 0x08,
	kIOPMSystemCapabilityAOT        = 0x10,
};
#endif

/* ---- IOKit/pwr_mgt/IOPMKeys.h (IOKitUser) ------------------------------ */
#ifndef kIOPMAutoWake
#define kIOPMAutoWake			"wake"
#define kIOPMAutoPowerOn		"poweron"
#define kIOPMAutoWakeOrPowerOn		"wakepoweron"
#define kIOPMAutoSleep			"sleep"
#define kIOPMAutoShutdown		"shutdown"
#define kIOPMAutoRestart		"restart"
#define kIOPMPowerEventTimeKey		"time"
#define kIOPMPowerEventAppNameKey	"scheduledby"
#define kIOPMPowerEventAppPIDKey	"appPID"
#define kIOPMPowerEventTypeKey		"eventtype"
#endif

/* ---- IOKit/pwr_mgt/IOPMLib.h (IOKitUser): assertion property keys ------ */
#ifndef kIOPMAssertionNameKey
#define kIOPMAssertionNameKey			CFSTR("AssertName")
#define kIOPMAssertionLevelKey			CFSTR("AssertLevel")
#define kIOPMAssertionTypeKey			CFSTR("AssertType")
#define kIOPMAssertionDetailsKey		CFSTR("Details")
#define kIOPMAssertionHumanReadableReasonKey	CFSTR("HumanReadableReason")
#define kIOPMAssertionLocalizationBundlePathKey	CFSTR("BundlePath")
#define kIOPMAssertionTimeoutKey		CFSTR("TimeoutSeconds")
#define kIOPMAssertionTimeoutActionKey		CFSTR("TimeoutAction")
#endif
#ifndef kIOPMAssertPreventUserIdleSystemSleep
#define kIOPMAssertPreventUserIdleSystemSleep	CFSTR("PreventUserIdleSystemSleep")
#endif

/* ---- IOKit/pwr_mgt/IOPMLibPrivate.h (IOKitUser): scheduled events ------ */
#ifndef kIOPMAutoWakePrefsPath
/* An SCPreferences prefsID, so <prefs dir>/com.apple.AutoWake.plist. */
#define kIOPMAutoWakePrefsPath		"com.apple.AutoWake.xml"
#endif
#ifndef kIOPMWakeRequestEntitlement
#define kIOPMWakeRequestEntitlement	CFSTR("com.apple.iokit.wakerequest")
#endif
#ifndef kIOPMAutoWakeScheduleImmediate
#define kIOPMAutoWakeScheduleImmediate	"WakeImmediate"
#define kIOPMAutoPowerScheduleImmediate	"PowerOnImmediate"
#endif
#ifndef kIOPMSchedulePowerEventNotification
#define kIOPMSchedulePowerEventNotification \
	"com.apple.system.IOPMSchedulePowerEventNotification"
#define kIOPMUserVisiblePowerEventNotification \
	"com.apple.system.powermanagement.uservisiblepowerevent"
#endif
#ifndef kIOPMRepeatingPowerOnKey
#define kIOPMRepeatingPowerOnKey	"RepeatingPowerOn"
#define kIOPMRepeatingPowerOffKey	"RepeatingPowerOff"
#define kIOPMPowerEventLeewayKey	"leeway"
#define kIOPMPowerEventUserVisible	"UserVisible"
#define kIOPMDaysOfWeekKey		"weekdays"
#endif
#ifndef kIOPMAssertInternalPreventSleep
#define kIOPMAssertInternalPreventSleep	CFSTR("InternalPreventSleep")
#endif
#ifndef kIOPMScheduleEvent
#define kIOPMCancelScheduledEvent	0
#define kIOPMScheduleEvent		1
#define kIOPMCancelAllScheduledEvents	2
#endif

/* ---- IOKit/pwr_mgt/IOPMPrivate.h (xnu) --------------------------------- */
#ifndef kIOPMSleepReasonKey
#define kIOPMSleepReasonKey		"Sleep Reason"
#endif

/* ---- PrivateLib.h ------------------------------------------------------ */
#define BIT_IS_SET(x,b)                     ((x & b)==b)
#define BIT_IS_NOT_SET(x,b)                 ((x & (b))==0)
#define IS_CAP_GAIN(c, f)       \
        ((((c)->fromCapabilities & (f)) == 0) && \
         (((c)->toCapabilities & (f)) != 0))
#define IS_CAP_LOSS(c, f)       \
        ((((c)->fromCapabilities & (f)) != 0) && \
         (((c)->toCapabilities & (f)) == 0))

#define PM_LOG_SYSTEM               "com.apple.powerd"
#define WAKEREQUESTS_LOG            "wakeRequests"

#ifndef LOG_STREAM
#define LOG_STREAM OS_LOG_DEFAULT
#endif

#define INFO_LOG(fmt, args...) \
{ \
    os_log(LOG_STREAM, fmt, ##args); \
}
#define DEBUG_LOG(fmt, args...) \
{ \
    os_log_debug(LOG_STREAM, fmt, ##args); \
}

#define ERROR_LOG(fmt, args...) \
{  \
    os_log_error(LOG_STREAM, fmt, ##args); \
}

__private_extern__ dispatch_queue_t	_getPMMainQueue(void);
__private_extern__ void			_askNicelyThenShutdownSystem(void);
__private_extern__ void			_askNicelyThenRestartSystem(void);
__private_extern__ void			_askNicelyThenSleepSystem(void);
__private_extern__ bool			auditTokenHasEntitlement(
					    audit_token_t token,
					    CFStringRef entitlement);

/* ---- PMAssertions.h ---------------------------------------------------- */
__private_extern__ IOReturn InternalCreateAssertion(
                                CFMutableDictionaryRef properties,
                                IOPMAssertionID *outID);
__private_extern__ IOReturn
InternalCreateAssertionWithTimeout(CFStringRef type, CFStringRef name, int timerSecs, IOPMAssertionID *outID);
__private_extern__ CFMutableDictionaryRef	_IOPMAssertionDescriptionCreate(
                            CFStringRef AssertionType,
                            CFStringRef Name,
                            CFStringRef Details,
                            CFStringRef HumanReadableReason,
                            CFStringRef LocalizationBundlePath,
                            CFTimeInterval Timeout,
                            CFStringRef TimeoutBehavior);

/* ---- AutoWakeScheduler.c / RepeatingAutoWake.c ------------------------- */
/* Served on io_pm_assertion_copy_details (powermanagement_stubs.c). */
__private_extern__ CFArrayRef		copyScheduledPowerEvents(void);
__private_extern__ CFDictionaryRef	copyRepeatPowerEvents(void);

/* ---- IOKit/pwr_mgt/IOPMLib.h (IOKitUser): IOPMAutoWake.c --------------- */
IOReturn IOPMSchedulePowerEvent(CFDateRef time_to_wake, CFStringRef my_id,
    CFStringRef type);

#endif /* !_PM_SHIM_PRIVATELIB_FREEBSD_H_ */
