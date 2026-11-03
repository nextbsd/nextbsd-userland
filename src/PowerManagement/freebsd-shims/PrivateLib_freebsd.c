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
 *   IOPMSchedulePowerEvent                          IOKitUser IOPMAutoWake.c,
 *                                                   the "Immediate" types only
 */
#include "PrivateLib_freebsd.h"

#include <signal.h>
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
 * Sleep: pmconfigd asks the root domain with
 * IOPMSleepSystemWithOptions(getRootDomainConnect(), options), the reason in
 * kIOPMSleepReasonKey (pmconfigd.m and PMAssertions.c do the same for their
 * own sleeps). Both ends are still to come: the kernel root domain and
 * /dev/iopmroot's IOPMROOT_SLEEP (K4, nextbsd-kernel#243), and libIOKit's
 * IOPMLib calls over it (L4, nextbsd-userland#234). Until they land a
 * scheduled sleep is logged as not supported and the system stays awake;
 * the event is consumed like any other that has fired.
 *
 * When L4 lands, replace pmSleepSystemWithOptions() with
 * IOPMSleepSystemWithOptions() on the root-domain connection.
 */
static IOReturn
pmSleepSystemWithOptions(CFDictionaryRef options __unused)
{
    return kIOReturnUnsupported;
}

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
    ret = pmSleepSystemWithOptions(options);
    if (ret != kIOReturnSuccess) {
        ERROR_LOG("Scheduled sleep not performed: 0x%x%s\n", ret,
                  ret == kIOReturnUnsupported ? " (system sleep is not supported yet)" : "");
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
 * pmconfigd calls IOPMSchedulePowerEvent() itself for one thing: to hand
 * the next power-on time to the hardware, type kIOPMAutoPowerScheduleImmediate
 * (poweronScheduleCallout). IOKitUser's tellClockController() writes it to
 * the root domain as kIOPMSettingAutoPowerCalendarKey and
 * kIOPMSettingAutoPowerSecondsKey, where the platform's RTC driver picks it
 * up. NextBSD has no RTC alarm behind the root domain yet (S3,
 * nextbsd-kernel#245), so the time reaches no hardware. Apple's code
 * handles a platform without an RTC alarm the same way: the property write
 * fails, tellClockController() drops the error, IOPMSchedulePowerEvent()
 * returns kIOReturnSuccess, and the event stays scheduled and listed.
 *
 * Only the "Immediate" types are served here; the queued types go to
 * pmconfigd over MIG and are libIOKit's (L2, nextbsd-userland#232). Delete
 * this when pmconfigd links libIOKit's IOPMSchedulePowerEvent.
 */
IOReturn IOPMSchedulePowerEvent(
    CFDateRef time_to_wake,
    CFStringRef my_id __unused,
    CFStringRef type)
{
    if (!isA_CFString(type)) {
        return kIOReturnBadArgument;
    }

    if (!CFEqual(type, CFSTR(kIOPMAutoWakeScheduleImmediate))
        && !CFEqual(type, CFSTR(kIOPMAutoPowerScheduleImmediate)))
    {
        return kIOReturnUnsupported;
    }

    // NULL clears the alarm: nothing to clear.
    if (isA_CFDate(time_to_wake)) {
        ERROR_LOG("%@ at %@ not set: no RTC alarm on this system\n",
                  CFEqual(type, CFSTR(kIOPMAutoWakeScheduleImmediate)) ? CFSTR(kIOPMAutoWake) : CFSTR(kIOPMAutoPowerOn),
                  time_to_wake);
    }

    return kIOReturnSuccess;
}
