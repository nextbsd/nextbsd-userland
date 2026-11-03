/*
 * PrivateLib_freebsd.c — the PrivateLib.m, PMAssertions.c and IOPMLib
 * functions the ported Apple files call, until those files build.
 *
 * Declared in PrivateLib_freebsd.h. Each function keeps Apple's name and
 * signature, so the caller is Apple's code unchanged; when the Apple file
 * that owns a function is added to SRCS, the function is deleted here (the
 * linker reports any left behind as a duplicate).
 *
 *   _askNicelyThen{Sleep,Shutdown,Restart}System   PrivateLib.m
 *   auditTokenHasEntitlement                        PrivateLib.m
 *   _IOPMAssertionDescriptionCreate                 PMAssertions.c, verbatim
 *   InternalCreateAssertion{,WithTimeout}           PMAssertions.c
 *   IOPMSchedulePowerEvent, tellClockController,    IOKitUser IOPMAutoWake.c,
 *   doAMaintenanceWake                              the "Immediate" types only
 */
#include "PrivateLib_freebsd.h"

#include <math.h>
#include <signal.h>
#include <strings.h>
#include <unistd.h>

/* ---- PrivateLib.m: scheduled sleep, shutdown and restart --------------- */

/*
 * On macOS these post a distributed notification (PowerManagerAction) and
 * loginwindow carries the request out: it warns the user, logs them out,
 * then shuts down, restarts or calls IOPMSleepSystem. NextBSD has no
 * loginwindow that listens for it and no distributed notification center,
 * so pmconfigd carries the request out itself, the same way the system's
 * own commands do (nextbsd-userland#238).
 */

/*
 * Shutdown and restart: the init(8) signal protocol NextBSD's launchd
 * honours, as reboot(8), halt(8) and shutdown(8) use it
 * (src/shutdown/shutdown.c): SIGUSR2 powers off, SIGINT reboots. launchd
 * stops every job and then calls reboot(2).
 */
static void
pmAskLaunchdTo(int sig, const char *what)
{
    INFO_LOG("Scheduled %s: signalling launchd\n", what);
    if (kill(1, sig) != 0) {
        ERROR_LOG("Scheduled %s failed: kill(1, %d): %m\n", what, sig);
    }
}

__private_extern__ void _askNicelyThenShutdownSystem(void)
{
    pmAskLaunchdTo(SIGUSR2, "shutdown");
}

__private_extern__ void _askNicelyThenRestartSystem(void)
{
    pmAskLaunchdTo(SIGINT, "restart");
}

/*
 * Sleep: pmconfigd asks the root domain itself, with
 * IOPMSleepSystemWithOptions() on its root-domain connection and the reason
 * in kIOPMSleepReasonKey, as pmconfigd.m and PMAssertions.c do for their own
 * sleeps. The kernel sends kIOMessageSystemWillSleep to every app, waits
 * for their answers and then sleeps (nextbsd-kernel#243). "Scheduled Sleep"
 * is not one of xnu's sleep reasons, so the root domain records it as a
 * "Software Sleep", as xnu does.
 */
__private_extern__ void _askNicelyThenSleepSystem(void)
{
    CFMutableDictionaryRef  options = NULL;
    IOReturn                ret;

    options = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks);
    if (options) {
        CFDictionarySetValue(options, CFSTR(kIOPMSleepReasonKey), CFSTR("Scheduled Sleep"));
    }

    INFO_LOG("Scheduled sleep: asking the root domain to sleep\n");
    ret = IOPMSleepSystemWithOptions(getRootDomainConnect(), options);
    if (ret != kIOReturnSuccess) {
        ERROR_LOG("Scheduled sleep not performed: 0x%x%s\n", ret,
                  ret == kIOReturnUnsupported ? " (this machine cannot sleep)" : "");
    }

    if (options) {
        CFRelease(options);
    }
}

/* ---- PrivateLib.m: entitlements ---------------------------------------- */

/*
 * Apple reads the caller's code-signing entitlements through SecTask.
 * NextBSD binaries carry no entitlements, so no caller has one: every
 * check falls back to the root (euid 0) test the caller makes next, as on
 * macOS for an unentitled process.
 */
__private_extern__ bool auditTokenHasEntitlement(
                                     audit_token_t token __unused,
                                     CFStringRef entitlement __unused)
{
    return false;
}

/* ---- PMAssertions.c ---------------------------------------------------- */

__private_extern__ CFMutableDictionaryRef _IOPMAssertionDescriptionCreate(
                                                                          CFStringRef AssertionType,
                                                                          CFStringRef Name,
                                                                          CFStringRef Details,
                                                                          CFStringRef HumanReadableReason,
                                                                          CFStringRef LocalizationBundlePath,
                                                                          CFTimeInterval Timeout,
                                                                          CFStringRef TimeoutBehavior)
{
    CFMutableDictionaryRef  descriptor = NULL;

    if (!AssertionType || !Name) {
        return NULL;
    }

    descriptor = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!descriptor) {
        return NULL;
    }

    CFDictionarySetValue(descriptor, kIOPMAssertionNameKey, Name);

    int _on = kIOPMAssertionLevelOn;
    CFNumberRef _on_num = CFNumberCreate(0, kCFNumberIntType, &_on);
    CFDictionarySetValue(descriptor, kIOPMAssertionLevelKey, _on_num);
    CFRelease(_on_num);

    CFDictionarySetValue(descriptor, kIOPMAssertionTypeKey, AssertionType);

    if (Details) {
        CFDictionarySetValue(descriptor, kIOPMAssertionDetailsKey, Details);
    }
    if (HumanReadableReason) {
        CFDictionarySetValue(descriptor, kIOPMAssertionHumanReadableReasonKey, HumanReadableReason);
    }
    if (LocalizationBundlePath) {
        CFDictionarySetValue(descriptor, kIOPMAssertionLocalizationBundlePathKey, LocalizationBundlePath);
    }
    if (Timeout) {
        CFNumberRef Timeout_num = CFNumberCreate(0, kCFNumberDoubleType, &Timeout);
        CFDictionarySetValue(descriptor, kIOPMAssertionTimeoutKey, Timeout_num);
        CFRelease(Timeout_num);
    }
    if (TimeoutBehavior)
    {
        CFDictionarySetValue(descriptor, kIOPMAssertionTimeoutActionKey, TimeoutBehavior);
    }

    return descriptor;
}

/*
 * pmconfigd's own assertions are PMAssertions.c's, which is not built yet
 * (nextbsd-userland#233). Until it is, an internal assertion is not taken.
 * AutoWakeScheduler.c takes two, both short-lived: the 2 s UserIsActive
 * that lights the display when a wake event comes due while the system is
 * already awake, and the PreventUserIdleSystemSleep that holds off idle
 * sleep until an imminent wake request. Neither changes whether a
 * scheduled event fires.
 */
__private_extern__ IOReturn InternalCreateAssertion(
                                CFMutableDictionaryRef properties __unused,
                                IOPMAssertionID *outID __unused)
{
    return kIOReturnUnsupported;
}

__private_extern__ IOReturn
InternalCreateAssertionWithTimeout(CFStringRef type __unused, CFStringRef name __unused,
                                   int timerSecs __unused, IOPMAssertionID *outID __unused)
{
    return kIOReturnUnsupported;
}

/* ---- IOKitUser IOPMAutoWake.c: IOPMSchedulePowerEvent ------------------ */

/*
 * pmconfigd calls IOPMSchedulePowerEvent() itself to hand wake and
 * power-on times to the hardware: PMConnection's
 * PMScheduleWakeEventChooseBest() at every kIOMessageSystemWillSleep (the
 * earliest wake, kIOPMAutoWakeScheduleImmediate or
 * kIOPMMaintenanceScheduleImmediate), and AutoWakeScheduler's
 * poweronScheduleCallout() whenever the next power-on changes
 * (kIOPMAutoPowerScheduleImmediate). The code below is IOKitUser's
 * (IOKitUser-100231.120.3, pwr_mgt.subproj/IOPMAutoWake.c): the calendar and
 * seconds root-domain properties for wake and power-on, and
 * kPMSetMaintenanceWakeCalendar for a maintenance wake. The kernel's root
 * domain arms the RTC alarm with them (nextbsd-kernel#245): the wake as the
 * system sleeps, the power-on as it powers off. On a machine without an RTC
 * wake alarm (arm64) the root domain answers kIOReturnUnsupported; as on
 * Darwin the error is dropped and the event stays scheduled and listed.
 *
 * Only the "Immediate" types are served here; the queued types go to
 * pmconfigd over MIG and are libIOKit's (L2, nextbsd-userland#232). Delete
 * this when pmconfigd links libIOKit's IOPMSchedulePowerEvent.
 */
static void
tellClockController(
    CFStringRef command,
    CFDateRef power_date)
{
    CFAbsoluteTime          now, wake_time;
    CFGregorianDate         gmt_calendar;
    CFTimeZoneRef           gmt_tz = NULL;
    long int                diff_secs;
    IOReturn                ret;
    CFNumberRef             seconds_delta = NULL;
    IOPMCalendarStruct      *cal_date = NULL;
    CFMutableDataRef        date_data = NULL;

    if(!command) goto exit;

    // We broadcast the wakeup time both as calendar date struct and as seconds.
    //  * AppleRTC hardware needs the date in a structured calendar format

    // ******************** Calendar struct ************************************

    date_data = CFDataCreateMutable(NULL, sizeof(IOPMCalendarStruct));
    CFDataSetLength(date_data, sizeof(IOPMCalendarStruct));
    cal_date = (IOPMCalendarStruct *)CFDataGetMutableBytePtr(date_data);
    bzero(cal_date, sizeof(IOPMCalendarStruct));

    if(!power_date) {

        // Zeroed out calendar means "clear wakeup timer"

    } else {

        // A calendar struct stuffed with meaningful date and time
        // schedules a wake or power event for then.

        wake_time = CFDateGetAbsoluteTime(power_date);

        gmt_tz = CFTimeZoneCreateWithTimeIntervalFromGMT(0, 0.0);
        gmt_calendar = CFAbsoluteTimeGetGregorianDate(wake_time, gmt_tz);
        CFRelease(gmt_tz);

        cal_date->second    = lround(gmt_calendar.second);
        if (60 == cal_date->second)
            cal_date->second = 59;
        cal_date->minute    = gmt_calendar.minute;
        cal_date->hour      = gmt_calendar.hour;
        cal_date->day       = gmt_calendar.day;
        cal_date->month     = gmt_calendar.month;
        cal_date->year      = gmt_calendar.year;
    }

    if(CFEqual(command, CFSTR(kIOPMAutoWake))) {

        // Set AutoWake calendar property
        ret = _setRootDomainProperty(CFSTR(kIOPMSettingAutoWakeCalendarKey), date_data);
    } else {

        // Set AutoPower calendar property
        ret = _setRootDomainProperty(CFSTR(kIOPMSettingAutoPowerCalendarKey), date_data);
    }

    if(kIOReturnSuccess != ret) {
#ifdef __FreeBSD__
        if (power_date) {
            INFO_LOG("%@ at %@ not handed to the RTC: 0x%x\n",
                     command, power_date, ret);
        }
#endif
        goto exit;
    }


    // *************************** Seconds *************************************
    // ApplePMU/AppleSMU seconds path
    // Machine needs to be told alarm in seconds relative to current time.

    if(!power_date) {
        // NULL dictionary argument, clear wakeup timer
        diff_secs = 0;
    } else {
        // Assume a well-formed entry since we've been doing thorough
        // type-checking in the find & purge functions
        now = CFAbsoluteTimeGetCurrent();
        wake_time = CFDateGetAbsoluteTime(power_date);

        diff_secs = lround(wake_time - now);
        if(diff_secs < 0) goto exit;
    }

    // Package diff_secs as a CFNumber
    seconds_delta = CFNumberCreate(0, kCFNumberLongType, &diff_secs);
    if(!seconds_delta) goto exit;

    if(CFEqual(command, CFSTR(kIOPMAutoWake))) {

        // Set AutoWake seconds property
        ret = _setRootDomainProperty(CFSTR(kIOPMSettingAutoWakeSecondsKey), seconds_delta);
    } else {

        // Set AutoPower seconds property
        ret = _setRootDomainProperty(CFSTR(kIOPMSettingAutoPowerSecondsKey), seconds_delta);
    }

    if(kIOReturnSuccess != ret) {
        goto exit;
    }

exit:
    if(date_data) CFRelease(date_data);
    if(seconds_delta) CFRelease(seconds_delta);
    return;
}

/*
 * IOKitUser opens its own root-domain connection here and calls
 * IOConnectCallStructMethod(); pmconfigd's connection (getRootDomainConnect())
 * and IOConnectCallMethod() reach the same kPMSetMaintenanceWakeCalendar
 * selector (RootDomain_freebsd.c).
 */
static IOReturn doAMaintenanceWake(
    CFDateRef                   earliestRequest,
    int                         type)
{
    CFGregorianDate         maintGregorian;
    IOPMCalendarStruct      pmMaintenanceDate;
    IOReturn                connectReturn = 0;
    size_t                  connectReturnSize = sizeof(IOReturn);
    io_connect_t            root_domain_connect = IO_OBJECT_NULL;
    IOReturn                ret = kIOReturnError;
    kern_return_t           kr = -1;

    // Package maintenance time as a PMCalendarType and pass it into IOPMrootDomain
    CFTimeZoneRef  myTimeZone = NULL;

    myTimeZone = CFTimeZoneCreateWithTimeIntervalFromGMT(0, 0.0);
    if (!myTimeZone)
        goto exit;

    maintGregorian = CFAbsoluteTimeGetGregorianDate(CFDateGetAbsoluteTime(earliestRequest), myTimeZone);

    CFRelease(myTimeZone);

    // Stuff into PM Calendar struct
    bzero(&pmMaintenanceDate, sizeof(pmMaintenanceDate));
    pmMaintenanceDate.year      = maintGregorian.year;
    pmMaintenanceDate.month     = maintGregorian.month;
    pmMaintenanceDate.day       = maintGregorian.day;
    pmMaintenanceDate.hour      = maintGregorian.hour;
    pmMaintenanceDate.minute    = maintGregorian.minute;
    pmMaintenanceDate.second    = maintGregorian.second;
    pmMaintenanceDate.selector  = type;

    root_domain_connect = getRootDomainConnect();
    if (root_domain_connect != IO_OBJECT_NULL)
    {
        kr = IOConnectCallMethod(
                root_domain_connect,
                kPMSetMaintenanceWakeCalendar,
                NULL, 0,                                            // no scalars
                &pmMaintenanceDate, sizeof(pmMaintenanceDate),      // inputs struct
                NULL, NULL,
                &connectReturn, &connectReturnSize);                // outputs struct

        if (kIOReturnSuccess == kr) {
            ret = connectReturn;
        } else {
            ret = kr;
        }
    }

exit:
    return ret;
}

IOReturn IOPMSchedulePowerEvent(
    CFDateRef time_to_wake,
    CFStringRef my_id __unused,
    CFStringRef type)
{
    IOReturn                ret = kIOReturnError;

    if (!isA_CFString(type)) {
        return kIOReturnBadArgument;
    }

    if( CFEqual(type, CFSTR(kIOPMMaintenanceScheduleImmediate)) )
    {
        if (!isA_CFDate(time_to_wake)) {
            return kIOReturnBadArgument;
        }
        ret = doAMaintenanceWake(time_to_wake, kPMCalendarTypeMaintenance);
    } else if( CFEqual(type, CFSTR(kIOPMAutoWakeScheduleImmediate)) )
    {

        // Just send down the wake event immediately
        tellClockController(CFSTR(kIOPMAutoWake), time_to_wake);
        ret = kIOReturnSuccess;

    } else if( CFEqual(type, CFSTR(kIOPMAutoPowerScheduleImmediate)) )
    {

        // Just send down the power on event immediately
        tellClockController(CFSTR(kIOPMAutoPowerOn), time_to_wake);
        ret = kIOReturnSuccess;

    } else {
        ret = kIOReturnUnsupported;
    }

    return ret;
}
