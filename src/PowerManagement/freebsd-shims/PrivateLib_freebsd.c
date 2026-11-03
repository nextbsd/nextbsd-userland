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
