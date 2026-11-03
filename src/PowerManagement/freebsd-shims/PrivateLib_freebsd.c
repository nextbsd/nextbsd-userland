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
 *   IOPMSchedulePowerEvent, tellClockController,    IOKitUser IOPMAutoWake.c,
 *   doAMaintenanceWake                              the "Immediate" types only
 *
 * and the interim versions of the functions PMAssertions.c, PMSettings.m
 * and PMAssertionLog.c call in Apple files that are not built yet, one
 * block per Apple file at the end (see PrivateLib_freebsd.h).
 */
#include <notify.h>

#include "PrivateLib_freebsd.h"
#include "PMConnection.h"
#include "Platform.h"

#include <grp.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
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

/* ---- PrivateLib.m: more of it, verbatim where it can be ---------------- */

__private_extern__ int callerIsRoot(int uid)
{
    return (0 == uid);
}

__private_extern__ int
callerIsAdmin(
    int uid,
    int gid
)
{
    int         ngroups = NGROUPS_MAX+1;
    int         group_list[NGROUPS_MAX+1];
    int         i;
    struct group    *adminGroup;
    struct passwd   *pw;


    pw = getpwuid(uid);
    if (!pw)
        return false;

    getgrouplist(pw->pw_name, pw->pw_gid, group_list, &ngroups);

    adminGroup = getgrnam("admin");
    if (adminGroup != NULL) {
        gid_t    adminGid = adminGroup->gr_gid;
        for(i=0; i<ngroups; i++)
        {
            if (group_list[i] == adminGid) {
                return TRUE;    // if a member of group "admin"
            }
        }
    }
    return false;

}

/*
 * Returns monotonic time in secs. Apple converts mach_absolute_time(), which
 * stops while the system sleeps; CLOCK_UPTIME is the same clock.
 */
__private_extern__ uint64_t getMonotonicTime(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_UPTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

/* No RunningBoard on NextBSD (and no entitlements to recognise it by). */
__private_extern__ bool auditTokenIsRunningboardd(audit_token_t token __unused)
{
    return false;
}

/* No entitlements on NextBSD: see auditTokenHasEntitlement. */
__private_extern__ bool xpcConnectionHasEntitlement(xpc_object_t connection,
                                                    CFStringRef entitlement)
{
    ERROR_LOG("PID %d doesnt have entitlement %@\n",
              xpc_connection_get_pid(connection), entitlement);
    return false;
}

/*
 * Apple reads boot-args from IODeviceTree:/options (NVRAM). NextBSD has no
 * device tree plane in its registry; nothing is set.
 */
__private_extern__ IOReturn getNvramArgInt(char *key __unused, int *value __unused)
{
    return kIOReturnError;
}

__private_extern__ bool isA_installEnvironment(void)
{
    static int installEnv = -1;

    if (installEnv == -1) {
        installEnv = (getenv("__OSINSTALL_ENVIRONMENT") != NULL) ? 1 : 0;
    }

    return (installEnv);
}

__private_extern__ aslmsg new_msg_pmset_log(void)
{
    aslmsg m = asl_new(ASL_TYPE_MSG);

    asl_set(m, ASL_KEY_LEVEL, ASL_STRING_NOTICE);
    asl_set(m, ASL_KEY_FACILITY, kPMFacility);

    return m;
}

/*
 * The MT2 dark-wake recorder: PrivateLib.m only records while mt2 is set
 * up, which happens for PowerNap machines; NextBSD has no dark wake.
 */
void mt2RecordAssertionEvent(assertionOps action __unused, assertion_t *theAssertion __unused)
{
}

/* Sleep services (PowerNap) never run on NextBSD, so they never end. */
__private_extern__ void logASLMessageSleepServiceTerminated(int forcedTimeoutCnt __unused)
{
}

/* ---- common/CommonLib.c, verbatim -------------------------------------- */

const char * descriptiveKernelAssertions(uint32_t val) {

    const char *string = "";
    if (val&kIOPMDriverAssertionCPUBit) {
        string = "CPU";
    }
    if (val&kIOPMDriverAssertionUSBExternalDeviceBit) {
        string = "USB";
    }
    if (val&kIOPMDriverAssertionBluetoothHIDDevicePairedBit) {
        string = "BT-HID";
    }
    if (val&kIOPMDriverAssertionExternalMediaMountedBit) {
        string = "MEDIA";
    }
    if (val&kIOPMDriverAssertionReservedBit5) {
        string = "THNDR";
    }
    if (val&kIOPMDriverAssertionPreventDisplaySleepBit) {
        string = "DSPLY";
    }
    if (val&kIOPMDriverAssertionReservedBit7) {
        string = "STORAGE";
    }
    if (val&kIOPMDriverAssertionMagicPacketWakeEnabledBit) {
        string = "MAGICWAKE";
    }
    if (val&kIOPMDriverAssertionNetworkKeepAliveActiveBit) {
        string = "NETWORK";
    }
    return string;
}

/* ---- PrivateLib.m: the store PMStore.c opens, verbatim ---------------- */

extern SCDynamicStoreRef gSCDynamicStore;	/* PMStore.c */

__private_extern__ SCDynamicStoreRef _getSharedPMDynamicStore(void)
{
    return gSCDynamicStore;
}

/*
 * ---- PMConnection.m (nextbsd-userland#229) -----------------------------
 * The parts PMConnection_freebsd.c (nextbsd-userland#229) does not carry:
 * no silent running, dark wake, sleep services or perf restriction.
 */
uint32_t gDebugFlags = kIOPMDebugAssertionASLLog|
                       kIOPMDebugEnableSpindumpOnFullwake|
                       kIOPMDebugLogAssertionActivity;

__private_extern__ IOReturn _unclamp_silent_running(bool sendNewCapBits __unused,
                                                    bool forceUpdate __unused)
{
    return kIOReturnSuccess;
}
__private_extern__ bool isInSilentRunningMode(void) { return false; }
__private_extern__ void evaluatePerfMode(void) { }
__private_extern__ bool isA_BTMtnceWake(void) { return false; }
__private_extern__ bool isA_SleepSrvcWake(void) { return false; }
__private_extern__ void cancelPowerNapStates(void) { }
__private_extern__ bool isA_NotificationDisplayWake(void) { return false; }
__private_extern__ void set_NotificationDisplayWake(void) { }
__private_extern__ void cancel_NotificationDisplayWake(void) { }
__private_extern__ int getCurrentSleepServiceCapTimeout(void) { return 0; }
__private_extern__ void setVMDarkwakeMode(bool darkwakeMode __unused) { }
__private_extern__ void setDwlInterval(uint32_t newInterval __unused) { }

/*
 * ---- SystemLoad.c ------------------------------------------------------
 * The user-activity levels SystemLoad.c keeps. Until it is ported the root
 * domain's view of the user is "active", as on a Mac in full wake.
 */
__private_extern__ void SystemLoadUserActiveAssertions(bool _userActiveAssertions __unused) { }
__private_extern__ bool userActiveRootDomain(void) { return true; }
__private_extern__ void userActiveHandleRootDomainActivity(bool active __unused) { }
__private_extern__ void userActiveHandlePowerAssertionsChanged(void) { }
__private_extern__ void userActiveHandleSleep(void) { }
__private_extern__ void SystemLoadPrefsHaveChanged(void) { }
bool getUserActiveAssertionValid(void) { return false; }
uint64_t getUserActivePostedLevels(void) { return 0; }

/* ---- PMDisplay.m: no desktop (lid closed, external display) mode ------- */
__private_extern__ bool isDesktopMode(void) { return false; }

/* ---- Platform.c: no TCP keepalive during sleep ------------------------- */
__private_extern__ tcpKeepAliveStates_et getTCPKeepAliveState(char *buf, int buflen,
                                                            bool quiet __unused)
{
    if (buf && buflen > 0) {
        buf[0] = '\0';
    }
    return kNotSupported;
}

/*
 * PrivateLib.m, verbatim: post a notify(3) name and log it.
 */
__private_extern__ void
powerd_notify_and_log(os_log_t log, const char *token)
{
    uint32_t rc = notify_post(token);

    if (rc == NOTIFY_STATUS_OK) {
        os_log(log, "posted '%s'\n", token);
    } else {
        os_log_error(log, "failed to post '%s'. rc:%#x\n", token, rc);
    }
}
