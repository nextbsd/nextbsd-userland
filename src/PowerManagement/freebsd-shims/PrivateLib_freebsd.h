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
 * The functions declared at the end live in PrivateLib_freebsd.c, except
 * the root-domain ones, which live in RootDomain_freebsd.c.
 */
#ifndef _PM_SHIM_PRIVATELIB_FREEBSD_H_
#define _PM_SHIM_PRIVATELIB_FREEBSD_H_

/*
 * PrivateLib.h's own include guard. Apple's smaller headers that a ported
 * file uses for their prototypes (PMSettings.h, PMConnection.h, Platform.h
 * ...) #include "PrivateLib.h" first; with the guard taken, that include is
 * empty and this file stands in for it.
 */
#ifndef _privatelib_h_
#define _privatelib_h_
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/pwr_mgt/powermanagement_mig.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <os/log.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * syslog's <asl.h> (libsystem_asl, which CommonLib.h includes) marks its
 * routines with __API_DEPRECATED and __not_tail_called; the
 * <Availability.h> pmconfigd sees defines neither, so define them away, as
 * syslog's own freebsd-shims/Availability.h does.
 */
#ifndef __API_DEPRECATED
#define __API_DEPRECATED(...)
#endif
#ifndef __not_tail_called
#define __not_tail_called
#endif

#include <asl.h>
#include <xpc/xpc.h>

/*
 * IOKitUser's <IOKit/ps/IOPowerSources.h> (libIOKit/IOKit/ps/) marks
 * one routine with __OSX_AVAILABLE_STARTING; NextBSD's CoreFoundation does
 * not pull in <Availability.h>, so define it away before the ps headers,
 * as Libnotify's and libxpc's shims do.
 */
#ifndef __OSX_AVAILABLE_STARTING
#define __OSX_AVAILABLE_STARTING(...)
#endif

#include <IOKit/IOMessage.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPowerSourcesPrivate.h>

/* ---- SystemConfiguration/SCValidation.h -------------------------------- */
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

/* kIOPMMessageClamshellStateChange's argument. */
enum {
	kClamshellStateBit = (1 << 0),
	kClamshellSleepBit = (1 << 1)
};
#endif
#ifndef sub_iokit_powermanagement
#define sub_iokit_powermanagement	0x34000		/* err_sub(13) */
#endif
#ifndef kIOPMMessageClamshellStateChange
#define kIOPMMessageClamshellStateChange \
	iokit_family_msg(sub_iokit_powermanagement, 0x100)
#endif
/* IOPMPrivate.h: a HID driver saw the user (nextbsd-kernel IOHIDSystem). */
#ifndef kIOPMMessageRequestUserActive
#define kIOPMMessageRequestUserActive \
	iokit_family_msg(sub_iokit_powermanagement, 0x460)
#endif
#ifndef kAppleClamshellStateKey
#define kAppleClamshellStateKey		"AppleClamshellState"
#define kAppleClamshellCausesSleepKey	"AppleClamshellCausesSleep"
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
#ifndef _IOKIT_IOPM_H	/* IOPMPrivate.h includes IOPM.h */
/* System Sleep Types (kPMGetSystemSleepType) */
enum {
    kIOPMSleepTypeInvalid                   = 0,
    kIOPMSleepTypeAbortedSleep              = 1,
    kIOPMSleepTypeNormalSleep               = 2,
    kIOPMSleepTypeSafeSleep                 = 3,
    kIOPMSleepTypeHibernate                 = 4,
    kIOPMSleepTypeStandby                   = 5,
    kIOPMSleepTypePowerOff                  = 6,
    kIOPMSleepTypeDeepIdle                  = 7,
    kIOPMSleepTypeLast                      = 8
};
#endif

/* ---- IOKit/pwr_mgt/IOPMPrivate.h (xnu): root-domain property keys ------ */
#ifndef kIOPMRootDomainWakeReasonKey
#define kIOPMRootDomainWakeReasonKey	"Wake Reason"
#define kIOPMRootDomainWakeTypeKey	"Wake Type"
#endif

/* ---- IOKit/pwr_mgt/IOPM.h, IOPMPrivate.h (xnu): RTC wake settings ------ */
/*
 * What IOKitUser's tellClockController() and doAMaintenanceWake() hand the
 * root domain, which arms the RTC alarm with them (nextbsd-kernel#245).
 */
#ifndef kIOPMSettingAutoWakeSecondsKey
#define kIOPMSettingAutoWakeSecondsKey		"wake"
#define kIOPMSettingAutoWakeCalendarKey		"WakeByCalendarDate"
#define kIOPMSettingAutoPowerSecondsKey		"poweron"
#define kIOPMSettingAutoPowerCalendarKey	"PowerByCalendarDate"
#endif
#ifndef _IOKIT_IOPM_H
struct IOPMCalendarStruct {
	uint32_t    year;
	uint8_t     month;
	uint8_t     day;
	uint8_t     hour;
	uint8_t     minute;
	uint8_t     second;
	uint8_t     selector;
};
typedef struct IOPMCalendarStruct IOPMCalendarStruct;
#endif
#ifndef kIOPMSettingSleepServiceWakeCalendarKey	/* IOPMPrivate.h */
enum {
    kPMCalendarTypeInvalid = 0,
    kPMCalendarTypeMaintenance = 1,
    kPMCalendarTypeSleepService = 2
};
#endif
#ifndef kIOPMSystemCapabilitiesKey
#define kIOPMSystemCapabilitiesKey	"System Capabilities"
#endif
#ifndef kIOPMSettingSleepOnPowerButtonKey
#define kIOPMSettingSleepOnPowerButtonKey "Sleep On Power Button"
#endif

/* ---- IOKit/pwr_mgt/IOPMLibPrivate.h (IOKitUser): IOPMConnection -------- */
#ifndef kIOPMSystemPowerStateNotify
#define kIOPMSystemPowerStateNotify	"com.apple.powermanagement.systempowerstate"
#endif
#ifndef kIOPMSystemPowerCapabilitiesKeySuffix
#define kIOPMSystemPowerCapabilitiesKeySuffix "/IOKit/SystemPowerCapabilities"
#endif
#ifndef kIOPMCapabilityCPU
typedef uint32_t IOPMCapabilityBits;
typedef uint32_t IOPMConnectionMessageToken;
#define kIOPMCapabilityCPU			0x1
#define kIOPMCapabilityVideo			0x2
#define kIOPMCapabilityAudio			0x4
#define kIOPMCapabilityNetwork			0x8
#define kIOPMCapabilityDisk			0x10
#define kIOPMCapabilityPushServiceTask		0x20
#define kIOPMCapabilityBackgroundTask		0x40
#define kIOPMCapabilitySilentRunning		0x80
#define kIOPMCapabilityBitsMask			0xFF
#define kIOPMEarlyWakeNotification		0x8000
#endif
#ifndef kIOPMAckClientInfoKey
#define kIOPMAckNetworkMaintenanceWakeDate	CFSTR("NetworkMaintenanceWakeDate")
#define kIOPMAckSUWakeDate			CFSTR("SoftwareUpdate")
#define kIOPMAckWakeDate			CFSTR("WakeDate")
#define kIOPMAckDHCPRenewWakeDate		CFSTR("DHCPRenew")
#define kIOPMAckClientInfoKey			CFSTR("ClientInfo")
#define kIOPMAckClientInfoBGTaskKey		CFSTR("ClientInfoBGTask")
#define kIOPMAckClientInfoAppRefreshKey		CFSTR("ClientInfoAppRefresh")
#endif
#ifndef kIOPMMaintenanceScheduleImmediate
#define kIOPMMaintenanceScheduleImmediate	"MaintenanceImmediate"
#endif

/* ---- The root domain's user client (RootDomain_freebsd.c) -------------- */
/*
 * On macOS pmconfigd opens the IOPMrootDomain's user client with
 * IOServiceOpen() and calls it with IOConnectCallMethod() and the
 * kPM* selectors of xnu's IOPMLibDefs.h. NextBSD's root domain is in the
 * kernel too (nextbsd-kernel#243), but its user client is /dev/iopmroot and
 * libIOKit has no IOServiceOpen() or IOConnectCallMethod(). These two names
 * are routed to RootDomain_freebsd.c, which serves the root domain's
 * selectors over the device and nothing else, so the Apple call sites build
 * unchanged. Drop the two #defines when libIOKit grows the real calls.
 */
#include <IOKit/pwr_mgt/IOPMLibDefs.h>

__private_extern__ kern_return_t	pmRootDomainServiceOpen(
					    io_service_t service,
					    mach_port_t owningTask,
					    uint32_t type,
					    io_connect_t *connect);
__private_extern__ kern_return_t	pmRootDomainCallMethod(
					    io_connect_t connection,
					    uint32_t selector,
					    const uint64_t *input,
					    uint32_t inputCnt,
					    const void *inputStruct,
					    size_t inputStructCnt,
					    uint64_t *output,
					    uint32_t *outputCnt,
					    void *outputStruct,
					    size_t *outputStructCnt);
#define IOServiceOpen		pmRootDomainServiceOpen
#define IOConnectCallMethod	pmRootDomainCallMethod

/*
 * IOPMSetAggressiveness() goes through the shim too, so the idle timers
 * that xnu's root domain runs in the kernel (RootDomainIdle_freebsd.c) see
 * the sleep and display sleep minutes the kernel is sent. libIOKit's call
 * does the rest.
 */
__private_extern__ IOReturn		pmRootDomainSetAggressiveness(
					    io_connect_t fb,
					    unsigned long type,
					    unsigned long aggressiveness);
#define IOPMSetAggressiveness	pmRootDomainSetAggressiveness

/* The kernel root domain's state (IOPMROOT_GETSTATE), from the shim. */
#include "iopmroot.h"		/* libIOKit's vendored <sys/iopmroot.h> */
__private_extern__ IOReturn		pmRootDomainCopyState(struct iopmroot_state *st);

/* RootDomainIdle_freebsd.c: the root domain's and wrangler's idle timers */
__private_extern__ void			pmIdle_prime(void);
__private_extern__ void			pmIdleAggressivenessChanged(unsigned long type,
					    unsigned long value);
__private_extern__ void			pmIdleUserAssertionLevelsChanged(uint32_t levels);
__private_extern__ void			pmIdleActivityTickle(void);
__private_extern__ void			pmIdleSystemDidWake(void);
__private_extern__ void			pmIdleDisplaySleepNow(void);
__private_extern__ CFTimeInterval	_getHIDIdleTime(void);

/*
 * CPUPower_freebsd.c: the CPU energy/performance policy a Mac's platform
 * plugin applies from the active Energy Saver profile.
 */
__private_extern__ void			cpuPowerActivateSettings(CFDictionaryRef settings);

/* common/CommonLib.c */
__private_extern__ io_registry_entry_t	getRootDomain(void);
/* PrivateLib.m */
__private_extern__ IOReturn		_setRootDomainProperty(CFStringRef key,
					    CFTypeRef val);
__private_extern__ CFTypeRef		_copyRootDomainProperty(CFStringRef key);
/* PMConnection.m (PMConnection_freebsd.c) */
__private_extern__ io_connect_t		getRootDomainConnect(void);
/* pmconfigd.m */
__private_extern__ void			initializeRootDomainInterestNotifications(void);
/* The sleep policy NextBSD's root domain leaves to pmconfigd */
__private_extern__ void			claimRootDomainSleepPolicy(void);
__private_extern__ void			initializeLowPowerSleep(void);

/* ---- PrivateLib.h ------------------------------------------------------ */
#define BIT_IS_SET(x,b)                     ((x & b)==b)
#define BIT_IS_NOT_SET(x,b)                 ((x & (b))==0)
#define IS_CAP_GAIN(c, f)       \
        ((((c)->fromCapabilities & (f)) == 0) && \
         (((c)->toCapabilities & (f)) != 0))
#define IS_CAP_LOSS(c, f)       \
        ((((c)->fromCapabilities & (f)) != 0) && \
         (((c)->toCapabilities & (f)) == 0))

#define CAPABILITY_BIT_CHANGED(x, y, b)     ((x ^ y) & b)
#define CHANGED_CAP_BITS(x, y)              ((x) ^ (y))

/* gDebugFlags bits */
#define kIOPMDebugAssertionASLLog           0x01
#define kIOPMDebugLogAssertionSynchronous   0x02
#define kIOPMDebugLogCallbacks              0x04
#define kIOPMDebugLogWakeRequests           0x08
#define kIOPMDebugLogUserActivity           0x10
#define kIOPMDebugEnableSpindumpOnFullwake  0x20
#define kIOPMDebugLogAssertionActivity      0x40  // Logs assertion data to log archive
#define kIOPMDebugLogAssertionNameChange    0x80
#define kIOPMDebugTraceAssertionActivity    0x100 // Logs assertion data to log archive

#define kProcNameBufLen 64

#define PM_LOG_SYSTEM               "com.apple.powerd"
#define ASSERTIONS_LOG              "assertions"
#define PMSETTINGS_LOG              "pmSettings"
#define DISPLAY_LOG                 "displayState"
#define BATTERY_LOG                 "battery"
#define BATTERY_HEALTH_LOG          "batteryhealth"
#define WAKEREQUESTS_LOG            "wakeRequests"
#define SLEEPWAKE_LOG               "sleepWake"

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
__private_extern__ void			powerd_notify_and_log(os_log_t log,
					    const char *token);

/* ---- PrivateLib.h: batteries ------------------------------------------ */
typedef enum {
   kBatteryPowered = 0,
   kACPowered
} PowerSources;

/*
 * PrivateLib.h's struct IOPMBattery, plus the two fields
 * BatteryTimeRemaining.m reads in this drop but PrivateLib.h's copy lacks
 * (fullyCharged, adapterUnsupported), plus one NextBSD field.
 */
struct IOPMBattery {
    io_registry_entry_t     me;
    io_object_t             msg_port;
    CFMutableDictionaryRef  properties;
    uint32_t                     externalConnected:1;
    uint32_t                     rawExternalConnected:1;
    uint32_t                     externalChargeCapable:1;
    uint32_t                     isCharging:1;
    uint32_t                     showChargingUI:1;
    uint32_t                     playChargingChime:1;
    uint32_t                     isPresent:1;
    uint32_t                     markedDeclining:1;
    uint32_t                     isTimeRemainingUnknown:1;
    uint32_t                     isCritical:1;
    uint32_t                     fullyCharged:1;
    uint32_t                     adapterUnsupported:1;
    uint32_t                pfStatus;
    int                     currentCap;
    int                     maxCap;
    int                     designCap;
    int                     voltage;
    int                     avgAmperage;
    int                     instantAmperage;
    int                     maxerr;
    int                     cycleCount;
    int                     location;
    int                     hwAverageTR;
    int                     swCalculatedTR;
    int                     swCalculatedPR;
    int                     invalidWakeSecs;
    CFStringRef             batterySerialNumber;
    CFStringRef             health;
    CFStringRef             failureDetected;
    CFStringRef             name;
    CFStringRef             dynamicStoreKey;
    CFStringRef             chargeStatus;
    time_t                  lowCapRatioSinceTime;
    boolean_t               hasLowCapRatio;
    /*
     * NextBSD: the IOPMPowerSource entry's registry name ("BAT0"). One
     * entry per ACPI battery; BatteryTimeRemaining_freebsd.c orders them
     * by it when it folds them into the one internal battery.
     */
    CFStringRef             ioregName;
};
typedef struct IOPMBattery IOPMBattery;

/* ---- xnu IOPM.h: the IOPMPowerSource keys ------------------------------ */

#define kIOPMPSExternalConnectedKey                 "ExternalConnected"
#define kIOPMPSExternalChargeCapableKey             "ExternalChargeCapable"
#define kIOPMPSBatteryInstalledKey                  "BatteryInstalled"
#define kIOPMPSIsChargingKey                        "IsCharging"
#define kIOPMFullyChargedKey                        "FullyCharged"
#define kIOPMPSAtWarnLevelKey                       "AtWarnLevel"
#define kIOPMPSAtCriticalLevelKey                   "AtCriticalLevel"
#define kIOPMPSCurrentCapacityKey                   "CurrentCapacity"
#define kIOPMPSMaxCapacityKey                       "MaxCapacity"
#define kIOPMPSDesignCapacityKey                    "DesignCapacity"
#define kIOPMPSTimeRemainingKey                     "TimeRemaining"
#define kIOPMPSAmperageKey                          "Amperage"
#define kIOPMPSVoltageKey                           "Voltage"
#define kIOPMPSCycleCountKey                        "CycleCount"
#define kIOPMPSMaxErrKey                            "MaxErr"
#define kIOPMPSLocationKey                          "Location"
#define kIOPMPSErrorConditionKey                    "ErrorCondition"
#define kIOPMPSManufacturerKey                      "Manufacturer"
#define kIOPMPSModelKey                             "Model"
#define kIOPMPSSerialKey                            "Serial"
#define kIOPMPSBatteryChargeStatusKey               "ChargeStatus"
#define kIOPMPSAdapterDetailsKey                    "AdapterDetails"
#define kIOPMPSInvalidWakeSecondsKey                "BatteryInvalidWakeSeconds"

/*
 * iokit_family_msg(sub_iokit_pmu, 0x100); sub_iokit_pmu is err_sub(9).
 * The kernel's IOPMPowerSource entries send it after every change.
 */
#ifndef kIOPMMessageBatteryStatusHasChanged
#define kIOPMMessageBatteryStatusHasChanged         ((uint32_t)0xe0024100)
#endif


/* ioupsd/IOUPSPrivate.h */
#ifndef MAKE_UNIQ_SOURCE_ID
#define MAKE_UNIQ_SOURCE_ID(pid, psID)      (((pid & 0xffff) << 16) | (psID & 0xffff))
#endif

/* ---- IOKit/pwr_mgt/IOPMLib.h (IOKitUser): assertions -------------------- */
#ifndef kIOPMAssertionFrameworkIDKey
enum {
    kIOPMNullAssertionID = 0
};
#define kIOPMAssertionFrameworkIDKey		CFSTR("FrameworkBundleID")
#define kIOPMAssertNetworkClientActive		CFSTR("NetworkClientActive")
#define kIOPMAssertionTypeNoIdleSleep		CFSTR("NoIdleSleepAssertion")
#define kIOPMAssertionTypeNoDisplaySleep	CFSTR("NoDisplaySleepAssertion")
#endif

/* ---- IOKit/pwr_mgt/IOPMLibPrivate.h (IOKitUser): assertions ------------- */
#ifndef kIOPMAssertionTypeApplePushServiceTask
/* Entitlements (no NextBSD caller holds one; see auditTokenHasEntitlement) */
#define kIOPMDarkWakeControlEntitlement		CFSTR("com.apple.private.iokit.darkwake-control")
#define kIOPMInteractivePushEntitlement		CFSTR("com.apple.private.iokit.interactive-push")
#define kIOPMReservePwrCtrlEntitlement		CFSTR("com.apple.private.iokit.reservepower-control")
#define kIOPMAssertOnBatteryEntitlement		CFSTR("com.apple.private.iokit.assertonbattery")
#define kIOPMAssertOnLidCloseEntitlement	CFSTR("com.apple.private.iokit.assertonlidclose")
#define kIOPMAssertionSuspendResumeEntitlement	CFSTR("com.apple.private.iokit.assertion-suspendresume")
#define kIOPMAssertionTypeSoftwareUpdateEntitlement CFSTR("com.apple.private.iokit.assertion-softwareupdate")

/* Assertion types */
#define kIOPMAssertionTypeApplePushServiceTask	CFSTR("ApplePushServiceTask")
#define kIOPMAssertInteractivePushServiceTask	CFSTR("InteractivePushServiceTask")
#define kIOPMAssertionTypeBackgroundTask	CFSTR("BackgroundTask")
#define kIOPMAssertAwakeReservePower		CFSTR("AwakeOnReservePower")
#define kIOPMAssertionTypeDenySystemSleep	CFSTR("DenySystemSleep")
#define kIOPMAssertDisplayWake			CFSTR("DisplayWake")
#define kIOPMAssertInternalPreventDisplaySleep	CFSTR("InternalPreventDisplaySleep")
#define kIOPMAssertRequiresDisplayAudio		CFSTR("RequiresDisplayAudio")
#define kIOPMAssertMaintenanceActivity		CFSTR("MaintenanceWake")
#define kIOPMAssertionTypeSystemIsActive	CFSTR("SystemIsActive")
#define kIOPMAssertionTypeSoftwareUpdateTask	CFSTR("SoftwareUpdateTask")
#define kIOPMAssertionTypeDisableInflow		CFSTR("DisableInflow")
#define kIOPMInflowDisableAssertion		kIOPMAssertionTypeDisableInflow
#define kIOPMAssertionTypeInhibitCharging	CFSTR("ChargeInhibit")
#define kIOPMChargeInhibitAssertion		kIOPMAssertionTypeInhibitCharging
#define kIOPMAssertionTypeDisableLowBatteryWarnings CFSTR("DisableLowPowerBatteryWarnings")
#define kIOPMAssertionTypeEnableIdleSleep	CFSTR("EnableIdleSleep")
#define kIOPMAssertionTypeNeedsCPU		CFSTR("CPUBoundAssertion")

/* Assertion dictionary keys */
#define kIOPMAssertionCreateDateKey		CFSTR("AssertStartWhen")
#define kIOPMAssertionTimedOutDateKey		CFSTR("AssertTimedOutWhen")
#define kIOPMAssertionTimeoutTimeLeftKey	CFSTR("AssertTimeoutTimeLeft")
#define kIOPMAssertionTimeoutUpdateTimeKey	CFSTR("AssertTimeoutUpdateTime")
#define kIOPMAssertionPIDKey			CFSTR("AssertPID")
#define kIOPMAssertionGlobalUniqueIDKey		CFSTR("GlobalUniqueID")
#define kIOPMAssertionAppliesToLimitedPowerKey	CFSTR("AppliesToLimitedPower")
#define kIOPMAssertionAppliesOnLidClose		CFSTR("AppliesOnLidClose")
#define kIOPMAssertionExitSilentRunning		CFSTR("ExitSilentRunning")
#define kIOPMAssertionResourcesUsed		CFSTR("ResourcesUsed")
#define kIOPMAssertionResourceAudioIn		CFSTR("audio-in")
#define kIOPMAssertionResourceAudioOut		CFSTR("audio-out")
#define kIOPMAssertionResourceGPS		CFSTR("GPS")
#define kIOPMAssertionResourceBaseband		CFSTR("baseband")
#define kIOPMAssertionResourceBluetooth		CFSTR("bluetooth")
#define kIOPMAssertionResourcePerfUnrestricted	CFSTR("perf-unrestricted")
#define kIOPMAssertionResourceCamera		CFSTR("Camera")
#define kIOPMAssertionCategoryKey		CFSTR("Category")
#define kIOPMAssertionActivityBudgeted		CFSTR("ActivityBudgeted")
#define kIOPMAssertionAllowsDeviceRestart	CFSTR("AllowsDeviceRestart")
#define kIOPMAssertionOnBehalfOfBundleID	CFSTR("AssertionOnBehalfOfBundleID")
#define kIOPMAssertionTrueTypeKey		CFSTR("AssertionTrueType")
#define kIOPMAssertionIdKey			CFSTR("AssertionId")
#define kIOPMAsyncClientAssertionIdKey		CFSTR("AsyncClientAssertionId")
#define kIOPMAssertionIsStateSuspendedKey	CFSTR("AssertionIsStateSuspended")
#define kIOPMAssertionIsCoalescedKey		CFSTR("IsCoalesced")
#define kIOPMAssertionTimeoutActionKillProcess	CFSTR("TimeoutActionKillProcess")

/* notify(3) names */
#define kIOPMAssertionTimedOutNotifyString	"com.apple.system.powermanagement.assertions.timeout"
#define kIOPMAssertionsAnyChangedNotifyString	"com.apple.system.powermanagement.assertions.anychange"
#define kIOPMAssertionsChangedNotifyString	"com.apple.system.powermanagement.assertions"
#define kIOPMAssertionsCollectBTString		"com.apple.powermanagement.collectbt"
#define kIOPMDisableAppSleepPrefix		"com.apple.system.powermanagement.disableappsleep"
#define kIOPMEnableAppSleepPrefix		"com.apple.system.powermanagement.enableappsleep"
#define kIOPMAssertionExceptionNotifyName	"com.apple.powermanagement.assertionexception"

/* Assertion exception limits (IOPMSetAssertionExceptionLimits) */
#define kIOPMAssertionDurationLimit		CFSTR("Asssertion Duration Limit")
#define kIOPMAggregateAssertionLimit		CFSTR("Aggregate Assertion Limit")
#define kIOPMDefaultLimtsKey			CFSTR("Default Limits")
#define kIOPMFrequentSleepWakeLimit		CFSTR("Frequent SW Limit")

typedef enum  {
    kIOPMAssertionDurationException = 1,
    kIOPMAssertionAggregateException,
    kIOPMFrequentSleepWakeException,
    kIOPMAssertionSystemTimeoutException,
} IOPMAssertionException;

typedef uintptr_t IOPMNotificationHandle;

typedef enum {
    kIOPMSystemSleepReverted,
    kIOPMSystemSleepNotReverted
} IOPMSystemState;

enum
{
    kIOPMIdleSleepPreventers    = 0,
    kIOPMSystemSleepPreventers  = 1
};

/* io_pm_ctl_assertion_type() actions */
#define kIOPMDisableAssertionType 0x1
#define kIOPMEnableAssertionType 0x2

/* Request type(req_type) for IOPMAssertionNotify() */
enum {
    kIOPMNotifyRegister = 0x1,
    kIOPMNotifyDeRegister = 0x2
};

typedef enum {
    kIOPMAssertionProcessStateSuspend = 0x1, /* Set State to Suspended */
    kIOPMAssertionProcessStateResume = 0x2, /* Set State to Resumed */
} IOPMAssertionProcessStateType;
#endif /* !kIOPMAssertionTypeApplePushServiceTask */

/*
 * The ASL "Action" values PMAssertionLog.c writes for each assertion event
 * (what pmset -g log shows). IOKitUser's public IOPMLibPrivate.h carried
 * the first ten; the IOKitUser-100231.120.3 drop has none of them, and the
 * last five are newer than any public copy, so those use the same wording.
 * Log text only: no client parses them.
 */
#ifndef kPMASLAssertionActionCreate
#define kPMASLAssertionActionCreate		"Created"
#define kPMASLAssertionActionRetain		"Retained"
#define kPMASLAssertionActionRelease		"Released"
#define kPMASLAssertionActionClientDeath	"ClientDied"
#define kPMASLAssertionActionTimeOut		"TimedOut"
#define kPMASLAssertionActionSummary		"Summary"
#define kPMASLAssertionActionTurnOff		"TurnedOff"
#define kPMASLAssertionActionTurnOn		"TurnedOn"
#define kPMASlAssertionActionCapTimeOut		"CapExpired"
#define kPMASLAssertionActionNameChange		"NameChanged"
#define kPMASLAssertionActionSuspend		"Suspended"
#define kPMASLAssertionActionResume		"Resumed"
#define kPMASLAssertionActionSystemTimeout	"SystemTimedOut"
#define kPMASLAssertionActionSessionStart	"SessionStarted"
#define kPMASLAssertionActionSessionEnd		"SessionEnded"
#endif

/* ---- IOKit/pwr_mgt/IOPMAssertionCategories.h (IOKitUser) ---------------- */
#ifndef _IOPMASSERTIONCATEGORIES_H_
typedef enum {
    kIOPMAssertionPolicyNone = 0,
    kIOPMAssertionPolicyActiveUse,
    kIOPMAssertionPolicyInactive,
    kIOPMAssertionPolicyDisplayOff,
} IOPMAssertionPolicy;
#endif

/* ---- IOKit/pwr_mgt/IOPM.h (xnu): driver assertions ---------------------- */
#ifndef _IOKIT_IOPM_H
enum {
    kIOPMDriverAssertionCPUBit                      = 0x01,
    kIOPMDriverAssertionUSBExternalDeviceBit        = 0x04,
    kIOPMDriverAssertionBluetoothHIDDevicePairedBit = 0x08,
    kIOPMDriverAssertionExternalMediaMountedBit     = 0x10,
    kIOPMDriverAssertionReservedBit5                = 0x20,
    kIOPMDriverAssertionPreventDisplaySleepBit      = 0x40,
    kIOPMDriverAssertionReservedBit7                = 0x80,
    kIOPMDriverAssertionMagicPacketWakeEnabledBit   = 0x100,
    kIOPMDriverAssertionNetworkKeepAliveActiveBit   = 0x200
};
#define kIOPMDriverAssertionIDKey               "ID"
#define kIOPMDriverAssertionCreatedTimeKey      "CreatedTime"
#define kIOPMDriverAssertionModifiedTimeKey     "ModifiedTime"
#define kIOPMDriverAssertionOwnerStringKey      "Owner"
#define kIOPMDriverAssertionOwnerServiceKey     "ServicePtr"
#define kIOPMDriverAssertionRegistryEntryIDKey  "RegistryEntryID"
#define kIOPMDriverAssertionLevelKey            "Level"
#define kIOPMDriverAssertionAssertedKey         "Assertions"
#endif

/* ---- IOKit/pwr_mgt/IOPM.h (xnu): IOPMPowerSource keys ------------------- */
#ifndef kIOPMPSExternalConnectedKey
#define kIOPMPSExternalConnectedKey		"ExternalConnected"
#define kIOPMPSIsChargingKey			"IsCharging"
#endif

/* ---- IOKit/pwr_mgt/IOPMLibPrivate.h (IOKitUser): energy settings -------- */
#ifndef kIOPMDisplaySleepKey
#define kIOPMDynamicStoreSettingsKey		"State:/IOKit/PowerManagement/CurrentSettings"
#define kIOPMDisplaySleepKey			"Display Sleep Timer"
#define kIOPMDiskSleepKey			"Disk Sleep Timer"
#define kIOPMSystemSleepKey			"System Sleep Timer"
#define kIOPMWakeOnLANKey			"Wake On LAN"
#define kIOPMWakeOnRingKey			"Wake On Modem Ring"
#define kIOPMRestartOnPowerLossKey		"Automatic Restart On Power Loss"
#define kIOPMRestartOnPowerConnectKey		"Automatic Restart On Power Connect"
#define kIOPMWakeOnACChangeKey			"Wake On AC Change"
#define kIOPMSleepOnPowerButtonKey		"Sleep On Power Button"
#define kIOPMWakeOnClamshellKey			"Wake On Clamshell Open"
#define kIOPMReduceBrightnessKey		"ReduceBrightness"
#define kIOPMDisplaySleepUsesDimKey		"Display Sleep Uses Dim"
#define kIOPMMobileMotionModuleKey		"Mobile Motion Module"
#define kIOPMTTYSPreventSleepKey		"TTYSPreventSleep"
#define kIOPMGPUSwitchKey			"GPUSwitch"
#define kIOPMDarkWakeBackgroundTaskKey		"DarkWakeBackgroundTasks"
#define kIOPMPowerNapSupportedKey		"PowerNap"
#define kIOPMSleepServicesKey			"SleepServices"
#define kIOPMUnifiedSleepSliderPrefKey		"UnifiedSleepSliderPref"
#define kIOPMTCPKeepAlivePrefKey		"TCPKeepAlivePref"
#define kIOPMProximityDarkWakeKey		"ProximityDarkWake"
#define kIOPMLowPowerModeKey			"LowPowerMode"
#define kIOPMHighPowerModeKey			"HighPowerMode"
#define kIOPMUpdateDarkWakeBGSettingKey		"Update DarkWakeBG Setting"
#define kIOPMDarkWakeLingerDurationKey		"DarkWake Linger Duration"
#define kIOPMCarrierMode			"Carrier Mode"
#define kIOPMCarrierModeVh			"Carrier Mode High Voltage"
#define kIOPMCarrierModeVl			"Carrier Mode Low Voltage"
#define kIOPMVact				"VAC-T"
#define kIOPMRestartOnKernelPanicKey		"RestartAfterKernelPanic"
#define kIOPMPrioritizeNetworkReachabilityOverSleepKey "PrioritizeNetworkReachabilityOverSleep"
#define kIOPMReduceSpeedKey			"Reduce Processor Speed"
#define kIOPMDynamicPowerStepKey		"Dynamic Power Step"
#define kIOPMCFPrefsPath			"com.apple.PowerManagement"
#define kIOPMSystemPowerSettingsKey		"SystemPowerSettings"
#define kIOPMPrefsChangeNotify			"com.apple.system.powermanagement.prefschange"
#define kIOPMSleepDisabledKey			CFSTR("SleepDisabled")
#endif

/* The older names for the capability bits (IOPMLibPrivate.h). */
#ifndef kIOPMSystemPowerStateCapabilityCPU
typedef uint32_t IOPMSystemPowerStateCapabilities;
#define kIOPMSystemPowerStateCapabilityCPU              kIOPMCapabilityCPU
#define kIOPMSystemPowerStateCapabilityVideo            kIOPMCapabilityVideo
#define kIOPMSystemPowerStateCapabilityAudio            kIOPMCapabilityAudio
#define kIOPMSystemPowerStateCapabilityNetwork          kIOPMCapabilityNetwork
#define kIOPMSystemPowerStateCapabilityDisk             kIOPMCapabilityDisk
#define kIOPMSystemPowerStateCapabilitiesMask           kIOPMCapabilityBitsMask
#endif

/* ---- IOKit/pwr_mgt/IOPMUPSPrivate.h (IOKitUser) ------------------------- */
#ifndef kIOPMDefaultUPSThresholds
#define kIOPMDefaultUPSThresholds		"UPSDefaultThresholds"
#endif

/* ---- xnu IOPM.h / IOKitUser IOPMLib.h: the providing-source names ------ */
/* (kIOPSProvidedBy* come from the vendored <IOKit/ps/IOPowerSourcesPrivate.h>.) */
#ifndef kIOPMACPowerKey
#define kIOPMACPowerKey				"AC Power"
#define kIOPMBatteryPowerKey			"Battery Power"
#define kIOPMUPSPowerKey			"UPS Power"
#endif

/* ---- IOKit/pwr_mgt/IOPM.h, IOPMPrivate.h (xnu): settings ---------------- */
#ifndef _IOKIT_IOPM_H
/* SetAggressiveness types */
enum {
	kPMGeneralAggressiveness = 0,
	kPMMinutesToDim,
	kPMMinutesToSpinDown,
	kPMMinutesToSleep,
	kPMEthernetWakeOnLANSettings,
	kPMSetProcessorSpeed,
	kPMPowerSource,
	kPMMotionSensor,
	kPMLastAggressivenessType
};
/* SetAggressiveness values for the kPMPowerSource aggressiveness type */
enum {
	kIOPMInternalPower = 1,
	kIOPMExternalPower
};
#define kIOPMDeepSleepEnabledKey		"Standby Enabled"
#define kIOPMDeepSleepDelayKey			"Standby Delay"
#define kIOPMDeepSleepDelayHighKey		"High Standby Delay"
#define kIOPMStandbyBatteryThresholdKey		"Standby Battery Threshold"
#define kIOPMDestroyFVKeyOnStandbyKey		"DestroyFVKeyOnStandby"
#define kIOPMSettingWakeOnRingKey		"Wake On Modem Ring"
#define kIOPMSettingRestartOnPowerLossKey	"Automatic Restart On Power Loss"
#define kIOPMSettingWakeOnACChangeKey		"Wake On AC Change"
#define kIOPMSettingSleepOnPowerButtonKey	"Sleep On Power Button"
#define kIOPMSettingWakeOnClamshellKey		"Wake On Clamshell Open"
#define kIOPMSettingDisplaySleepUsesDimKey	"Display Sleep Uses Dim"
#define kIOPMSettingMobileMotionModuleKey	"MobileMotionModule"
#define kIOPMAutoPowerOffEnabledKey		"AutoPowerOff Enabled"
#define kIOPMAutoPowerOffDelayKey		"AutoPowerOff Delay"
#endif

/* ---- IOKit/IOTypes.h ---------------------------------------------------- */
#ifndef kNilOptions
enum {
    kNilOptions = 0
};
#endif

/* ---- IOKit/IOKitKeys.h -------------------------------------------------- */
#ifndef kIOServicePlane
#define kIOServicePlane			"IOService"
#endif

/* ---- energytrace.h: no energy tracing on NextBSD ------------------------ */
/*
 * PMAssertions.c tags each assertion type with an energytrace quality and
 * reports assertion ends with entr_act_end(). NextBSD has no energytrace;
 * the qualities are kept as zeros and the call does nothing.
 */
#ifndef kEnTrQualNone
enum {
    kEnTrCompSysPower = 0,
    kEnTrActSPPMAssertion = 0,
    kEnTrQualNone = 0,
    kEnTrQualTimedOut = 0,
    kEnTrQualSPKeepSystemAwake = 0,
    kEnTrQualSPKeepDisplayAwake = 0,
    kEnTrQualSPPreventSleepSystem = 0,
    kEnTrQualSPWakeDisplay = 0,
    kEnTrValNone = 0
};
#define entr_act_end(comp, act, id, qual, val)	((void)(id))
#endif

/* ---- RunningBoardServices: no App Nap on NextBSD ------------------------ */
/*
 * PMAssertions.c asks RunningBoard whether an app takes part in App Nap
 * before it tells it about the app's assertions. NextBSD has no App Nap,
 * so no app does: the answer goes back to the caller in disableAppSleep /
 * enableAppSleep, as on a macOS without RunningBoard.
 */
#ifndef RBSAppNapSubFeatureIOPM
#define RBSAppNapSubFeatureIOPM				0
#define rbs_appnap_enabled_for_pid(pid, feature)	((void)(pid), false)
#define rbs_set_iopm_appnap_state(pid, reason, state)	do { } while (0)
#endif

/* ---- PrivateLib.h: settings, the powerd bundle ------------------------ */
/* IOPMAggressivenessFactors: the form of data that the kernel understands. */
typedef struct {
    unsigned int        fMinutesToDim;
    unsigned int        fMinutesToSpin;
    unsigned int        fMinutesToSleep;

    unsigned int        fWakeOnLAN;
    unsigned int        fWakeOnRing;
    unsigned int        fAutomaticRestart;
    unsigned int        fAutomaticRestartAtConnect;
    unsigned int        fSleepOnPowerButton;
    unsigned int        fWakeOnClamshell;
    unsigned int        fWakeOnACChange;
    unsigned int        fDisplaySleepUsesDimming;
    unsigned int        fMobileMotionModule;
    unsigned int        fGPU;
    unsigned int        fDeepSleepEnable;
    unsigned int        fDeepSleepDelay;
    unsigned int        fAutoPowerOffEnable;
    unsigned int        fAutoPowerOffDelay;
} IOPMAggressivenessFactors;

/*
 * The powerd bundle holds Apple's assertion timeout and category plists
 * (pmconfigd/Resources), installed by pmconfigd's Makefile.
 */
#define kPowerManagementBundlePathCString       "/System/Library/CoreServices/powerd.bundle"
#define kPowerdBundleIdentifier                 CFSTR("com.apple.powerd")
#define kPowerManagementBundlePathString        CFSTR(kPowerManagementBundlePathCString)

/* ---- PrivateLib.m ------------------------------------------------------ */
__private_extern__ int			callerIsRoot(int uid);
__private_extern__ int			callerIsAdmin(int uid, int gid);
__private_extern__ uint64_t		getMonotonicTime(void);
__private_extern__ bool			auditTokenIsRunningboardd(audit_token_t token);
__private_extern__ bool			xpcConnectionHasEntitlement(xpc_object_t connection,
					    CFStringRef entitlement);
__private_extern__ bool			isA_installEnvironment(void);
__private_extern__ aslmsg		new_msg_pmset_log(void);
__private_extern__ SCDynamicStoreRef	_getSharedPMDynamicStore(void);
__private_extern__ IOReturn		getNvramArgInt(char *key, int *value);
/* pmconfigd.m */
__private_extern__ bool			isDisplayAsleep(void);

/* ---- PMStore.h -------------------------------------------------------- */
#include "PMStore.h"

/* ---- CommonLib.h (../common) ------------------------------------------- */
#include "CommonLib.h"

/* ---- PMAssertions.h, as PrivateLib.h includes it ----------------------- */
#include "PMAssertions.h"

/* PMAssertions.c functions PMAssertions.h does not declare. */
void initSystemAssertionTimeoutList(void);
void initAssertionCategories(void);

/* PrivateLib.m: the MT2 (dark-wake telemetry) recorder. */
__private_extern__ void			mt2RecordAssertionEvent(assertionOps action,
					    assertion_t *theAssertion);

/* ---- Interim functions of Apple files that are not built yet ----------- */
/*
 * PMAssertions.c calls into most of pmconfigd. Until the Apple file that
 * owns a function is ported, PrivateLib_freebsd.c carries a version of it
 * that answers the way that file does on a running Intel Mac which is fully
 * awake, on AC power, with no display wrangler, SMC or PowerNap. Each block
 * names its file; the ticket that ports the file deletes the block there
 * (the linker reports a leftover as a duplicate symbol). Prototypes for
 * PMSettings.m, PMConnection.m and Platform.c come from Apple's own
 * headers, which PMAssertions.c includes.
 */
/* SystemLoad.c */
__private_extern__ void			SystemLoadUserActiveAssertions(bool _userActiveAssertions);
__private_extern__ bool			userActiveRootDomain(void);
__private_extern__ void			userActiveHandleRootDomainActivity(bool active);
__private_extern__ void			userActiveHandlePowerAssertionsChanged(void);
__private_extern__ void			userActiveHandleSleep(void);
__private_extern__ void			SystemLoadPrefsHaveChanged(void);
__private_extern__ bool			getUserActiveAssertionValid(void);
__private_extern__ uint64_t		getUserActivePostedLevels(void);
/* PMDisplay.m */
__private_extern__ bool			isDesktopMode(void);

/* ---- AutoWakeScheduler.c / RepeatingAutoWake.c ------------------------- */
/* Served on io_pm_assertion_copy_details (powermanagement_stubs.c). */
__private_extern__ CFArrayRef		copyScheduledPowerEvents(void);
__private_extern__ CFDictionaryRef	copyRepeatPowerEvents(void);

/*
 * ---- IOKit/pwr_mgt/IOPMLib.h (IOKitUser): IOPMAutoWake.c ---------------
 * libIOKit's (nextbsd-userland#235). powerd uses the "Immediate" types to
 * hand wake and power-on times to the root domain's RTC alarm
 * (nextbsd-kernel#245): PMScheduleWakeEventChooseBest() at every
 * kIOMessageSystemWillSleep, AutoWakeScheduler's poweronScheduleCallout().
 */
IOReturn IOPMSchedulePowerEvent(CFDateRef time_to_wake, CFStringRef my_id,
    CFStringRef type);

/* ---- PMDisplay.h (the clamshell part, PMDisplay_freebsd.c) ------------- */
__private_extern__ void updateClamshellState(void *message);
uint32_t rootDomainClamshellState(void);
void initializeClamshellState(void);
void postClamshellStateNotification(void);

/* ---- BatteryTimeRemaining.h (the parts built so far) ------------------- */

__private_extern__ IOPMBattery **_batteries(void);
__private_extern__ int _batteryCount(void);
__private_extern__ void BatteryTimeRemaining_prime(void);
__private_extern__ void BatteryTimeRemaining_finish(void);
__private_extern__ void BatteryTimeRemainingWakeNotification(void);
#define kInternalBattery      NULL
__private_extern__ void kernelPowerSourcesDidChange(IOPMBattery *battery_info);
__private_extern__ bool isFullyCharged(IOPMBattery *b);
__private_extern__ int getActivePSType(void);
__private_extern__ bool getPowerState(PowerSources *source, uint32_t *percentage);
__private_extern__ PowerSources _getPowerSource(void);
__private_extern__ CFDictionaryRef CF_RETURNS_RETAINED getActiveUPSDictionary(void);
__private_extern__ CFDictionaryRef CF_RETURNS_RETAINED getActiveBatteryDictionary(void);
__private_extern__ CFDictionaryRef batteryTimeRemaining_copyIOPMPowerSourceDictionary(void);
__private_extern__ int batteryTimeRemaining_getPercentRemaining(void);
__private_extern__ CFStringRef batteryTimeRemaining_getBatterySerialNumber(void);
__private_extern__ dispatch_queue_t BatteryTimeRemaining_getQ(void);

#ifndef kIOPSFailureKey
#define kIOPSFailureKey                         "Failure"
#endif

#ifndef kIOPSDynamicStoreLowBattPathKey
#define kIOPSDynamicStoreLowBattPathKey         "/IOKit/LowBatteryWarning"
#endif

/* BatteryTimeRemaining.h */
#ifndef kIOPSDynamicStorePowerAdapterKey
#define kIOPSDynamicStorePowerAdapterKey        "/IOKit/PowerAdapter"
#endif

typedef enum {
    kPSTypeUnknown          = 0,
    kPSTypeIntBattery       = 1,
    kPSTypeUPS              = 2,
    kPSTypeAccessory        = 3
} psTypes_t;

#endif /* !_PM_SHIM_PRIVATELIB_FREEBSD_H_ */
