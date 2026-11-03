/*
 * RootDomainIdle_freebsd.c — the root domain's idle sleep timer and the
 * display wrangler's idle timer, which xnu runs in the kernel, run in
 * pmconfigd until NextBSD's root domain grows them (nextbsd-userland#228,
 * E17 D4).
 *
 * On an Intel Mac pmconfigd decides the timer values and the kernel keeps
 * the time:
 *
 *   - PMSettings.m sends the energy settings with IOPMSetAggressiveness():
 *     kPMMinutesToSleep (the "sleep" setting, 0 while a
 *     PreventUserIdleSystemSleep assertion is held) and kPMMinutesToDim
 *     ("displaysleep");
 *   - PMAssertions.c sends the user assertion levels with
 *     kPMSetUserAssertionLevels: kIOPMDriverAssertionPreventDisplaySleepBit
 *     while PreventUserIdleDisplaySleep is held, kIOPMDriverAssertionCPUBit
 *     while PreventSystemSleep is held;
 *   - user activity reaches the kernel as HID tickles, and as
 *     kPMActivityTickle from IOPMAssertionDeclareUserActivity();
 *   - IODisplayWrangler turns the display off after kPMMinutesToDim idle
 *     minutes, unless told to ignore its idle timer, and the user becomes
 *     inactive;
 *   - IOPMrootDomain's idle sleep timer (evaluatePolicy(),
 *     getTimeToIdleSleep(), startIdleSleepTimer(),
 *     handleSleepTimerExpiration()) then sleeps the system kPMMinutesToSleep
 *     minutes after the last user activity, asking kIOMessageCanSystemSleep
 *     first, unless the CPU assertion bit holds it awake.
 *
 * NextBSD's root domain (nextbsd-kernel#243) stores the aggressiveness
 * values, the assertion levels and the tickle time, but runs no timer. This
 * file is that part of xnu's IOPMrootDomain.cpp and IODisplayWrangler,
 * with xnu's names and its macOS-with-a-display-wrangler policy. It sees
 * the same inputs at the same points: RootDomain_freebsd.c hands it every
 * IOPMSetAggressiveness(), kPMSetUserAssertionLevels and kPMActivityTickle
 * pmconfigd sends the kernel. When its timer expires it asks the kernel for
 * the sleep, as the root domain does for itself: IOPMSleepSystemWithOptions()
 * with "Idle Sleep", for which the kernel sends kIOMessageCanSystemSleep and
 * lets any app veto it.
 *
 * HID idle time. The time of the last user input comes from the
 * IOHIDSystem registry entry's HIDIdleTime, as PrivateLib.m's
 * _getHIDIdleTime() reads it. The kernel publishes that entry
 * (nextbsd-kernel IOHIDSystem, nextbsd-userland#239) and computes
 * HIDIdleTime on each read from the last evdev input. Like Darwin before
 * the WindowServer opens the event system, it reads 0 while no input
 * device has an open client (no window server). With no entry at all (an
 * older kernel) _getHIDIdleTime() also returns 0. Either way the user
 * counts as active right now, the display never goes idle and the system
 * never idle-sleeps, which keeps headless machines and the CI virtual
 * machines awake.
 *
 * Input wakes the display at once: after a quiet spell the kernel's
 * IOHIDSystem has the root domain send kIOPMMessageRequestUserActive,
 * which RootDomain_freebsd.c answers as PMConnection.m does, with a
 * UserIsActive assertion whose kPMActivityTickle reaches
 * pmIdleActivityTickle(). Input with no message is still seen whenever a
 * timer looks at HIDIdleTime.
 *
 * Display state. xnu's IODisplayWrangler powers the displays down itself;
 * on NextBSD the window server (XLibre, Gershwin) does the blanking, so
 * the wrangler state is published for it as on Apple's HID event system:
 * notify(3) kIOHIDEventSystemDisplayStatusNotifyKey,
 * "com.apple.iokit.hid.displayStatus", whose state is 1 while the display
 * is on and 0 once it should be off (posted on every change, set to 1 at
 * start). A window server watches it and blanks (DPMS off) on 0 and
 * unblanks on 1.
 *
 * Everything runs on pmconfigd's main queue; the entry points below hop onto
 * it, since Apple's code calls some of them from other queues.
 */
#include "PrivateLib_freebsd.h"

#include <notify.h>
#include <time.h>

/* xnu IOPMrootDomain.cpp */
#define kIdleSleepRetryInterval         (3 * 60 * 1000)     /* ms */
#define kMinimumTimeBeforeIdleSleep     1000                /* ms, macOS */

/* PrivateLib.m */
#ifndef kIOHIDIdleTimeKey
#define kIOHIDIdleTimeKey               "HIDIdleTime"
#endif

/* IOHIDEventSystemKeysPrivate.h: notify(3) state 1 display on, 0 off */
#ifndef kIOHIDEventSystemDisplayStatusNotifyKey
#define kIOHIDEventSystemDisplayStatusNotifyKey "com.apple.iokit.hid.displayStatus"
#endif

/* IOPMrootDomain state, as xnu names it */
static uint32_t             pmIdleSleepSlider;      /* kPMMinutesToSleep, as last set */
static uint32_t             sleepSlider;            /* ... as evaluatePolicy() took it */
static uint32_t             extraSleepDelay;
static bool                 idleSleepEnabled;
static bool                 idleSleepTimerPending;
static bool                 userIsActive = true;
static uint64_t             userActivityTime;       /* ns, last tickle */
static uint64_t             userBecameInactiveTime; /* ns */
static uint64_t             lastWakeTime;           /* ns, gIOLastWakeAbsTime */
static uint32_t             userAssertionLevels;    /* kPMSetUserAssertionLevels */
static dispatch_source_t    extraSleepTimer;

/* IODisplayWrangler state */
static uint32_t             displaySlider;          /* kPMMinutesToDim */
static bool                 wranglerAsleep;
static dispatch_source_t    displayIdleTimer;
static int                  displayStatusToken = -1;

/* NextBSD: the kernel root domain's sleep count, to notice a wake. */
static uint64_t             sleepCountSeen;
static bool                 cannotSleepLogged;

enum {
    kStimulusDisplayWranglerSleep,
    kStimulusDisplayWranglerWake,
    kStimulusAggressivenessChanged,
    kStimulusEnterUserActiveState,
    kStimulusLeaveUserActiveState
};

static void evaluatePolicy(int stimulus);
static void evaluateDisplayIdle(void);

/* mach_absolute_time() in nanoseconds: time awake since boot. */
static uint64_t
pmUptimeNanos(void)
{
    struct timespec     ts;

    clock_gettime(CLOCK_UPTIME, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

/* ---- PrivateLib.m ------------------------------------------------------ */

static const CFTimeInterval kTimeNSPerSec = 1000000000.0;

__private_extern__ CFTimeInterval _getHIDIdleTime(void)
{
    static io_registry_entry_t hidsys = IO_OBJECT_NULL;
    CFNumberRef     hidsys_idlenum = NULL;
    CFTimeInterval  ret_time = 0.0;
    uint64_t        idle_nanos = 0;

    if (IO_OBJECT_NULL == hidsys) {
        hidsys = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOHIDSystem"));
    }
    if (!hidsys)
        goto exit;

    hidsys_idlenum = IORegistryEntryCreateCFProperty(hidsys, CFSTR(kIOHIDIdleTimeKey), 0, 0);

    if (!isA_CFNumber(hidsys_idlenum))
        goto exit;

    if (CFNumberGetValue(hidsys_idlenum, kCFNumberSInt64Type, &idle_nanos))
    {
        ret_time = ((CFTimeInterval)idle_nanos)/kTimeNSPerSec;
    }

exit:
    if (hidsys_idlenum)
        CFRelease(hidsys_idlenum);
    return ret_time;
}

/*
 * NextBSD: is there an IOHIDSystem entry to read HIDIdleTime from? Without
 * one _getHIDIdleTime() is always 0, the user is always active, and the
 * display idle timer would only ever re-arm itself; it is left off.
 */
static bool
hidIdleTimeAvailable(void)
{
    static io_registry_entry_t hidsys = IO_OBJECT_NULL;

    if (IO_OBJECT_NULL == hidsys) {
        hidsys = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOHIDSystem"));
    }
    return (IO_OBJECT_NULL != hidsys);
}

/*
 * NextBSD: publish the wrangler's state for the window server, which does
 * the blanking (see "Display state" above).
 */
static void
publishDisplayStatus(bool displayOn)
{
    if (displayStatusToken == -1 &&
        notify_register_check(kIOHIDEventSystemDisplayStatusNotifyKey,
                              &displayStatusToken) != NOTIFY_STATUS_OK) {
        displayStatusToken = -1;
        return;
    }
    notify_set_state(displayStatusToken, displayOn ? 1 : 0);
    notify_post(kIOHIDEventSystemDisplayStatusNotifyKey);
}

/* ---- pmconfigd.m ------------------------------------------------------- */

/* The display state as the wrangler reports it to pmconfigd. */
__private_extern__ bool isDisplayAsleep(void)
{
    return wranglerAsleep;
}

/* ---- user activity ----------------------------------------------------- */

/*
 * The last user activity: the later of the last activity tickle and the
 * last HID input. xnu's root domain is told of both as they happen; here
 * the HID side is read when a timer needs it.
 */
static uint64_t
lastUserActivityTime(uint64_t now)
{
    CFTimeInterval      hidIdle = _getHIDIdleTime();
    uint64_t            hidActivity;

    if (hidIdle * kTimeNSPerSec >= (CFTimeInterval)now) {
        hidActivity = 0;
    } else {
        hidActivity = now - (uint64_t)(hidIdle * kTimeNSPerSec);
    }
    return (hidActivity > userActivityTime) ? hidActivity : userActivityTime;
}

/*
 * NextBSD: notice a wake on the way through. The kernel counts its sleeps
 * (IOPMROOT_GETSTATE); when the count has moved, the system slept and woke
 * since the last look. Every NextBSD wake is a full wake, with the display
 * on and the user active, as xnu's wrangler is after a user wake. The
 * sleep/wake ticket (nextbsd-userland#229) calls pmIdleSystemDidWake() from
 * its HasPoweredOn handling, which gets there first.
 */
static bool
checkForWake(void)
{
    struct iopmroot_state   st;

    if (pmRootDomainCopyState(&st) != kIOReturnSuccess) {
        return false;
    }
    if (st.sleep_count == sleepCountSeen) {
        return false;
    }
    sleepCountSeen = st.sleep_count;
    lastWakeTime = pmUptimeNanos();
    userActivityTime = lastWakeTime;
    evaluatePolicy(kStimulusDisplayWranglerWake);
    evaluatePolicy(kStimulusEnterUserActiveState);
    return true;
}

/* ---- IODisplayWrangler: the display idle timer ------------------------- */

static void
displayIdleTimerExpired(void)
{
    uint64_t    now = pmUptimeNanos();
    uint64_t    idleNanos = (uint64_t)displaySlider * 60 * NSEC_PER_SEC;

    if (checkForWake()) {
        return;
    }
    if (wranglerAsleep || (displaySlider == 0) ||
        (userAssertionLevels & kIOPMDriverAssertionPreventDisplaySleepBit)) {
        return;
    }
    if (now - lastUserActivityTime(now) < idleNanos) {
        /* Input since the timer was armed: wait for the rest. */
        evaluateDisplayIdle();
        return;
    }

    INFO_LOG("Display is turned off\n");
    evaluatePolicy(kStimulusDisplayWranglerSleep);
    evaluatePolicy(kStimulusLeaveUserActiveState);
}

/*
 * IODisplayWrangler's idle timer: arm it for the time left before
 * kPMMinutesToDim idle minutes. It does not run while the display is
 * already off, while display sleep is "never", or while the root domain
 * has told the wrangler to ignore it (setIgnoreIdleTimer(), for
 * kIOPMDriverAssertionPreventDisplaySleepBit).
 */
static void
evaluateDisplayIdle(void)
{
    uint64_t    now, last, idleNanos;

    if (!displayIdleTimer) {
        displayIdleTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _getPMMainQueue());
        if (!displayIdleTimer) {
            return;
        }
        dispatch_source_set_event_handler(displayIdleTimer, ^{ displayIdleTimerExpired(); });
        dispatch_source_set_timer(displayIdleTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        dispatch_resume(displayIdleTimer);
    }

    if (wranglerAsleep || (displaySlider == 0) ||
        (userAssertionLevels & kIOPMDriverAssertionPreventDisplaySleepBit) ||
        !hidIdleTimeAvailable()) {
        dispatch_source_set_timer(displayIdleTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        return;
    }

    now = pmUptimeNanos();
    last = lastUserActivityTime(now);
    idleNanos = (uint64_t)displaySlider * 60 * NSEC_PER_SEC;
    dispatch_source_set_timer(displayIdleTimer,
        dispatch_time(DISPATCH_TIME_NOW, (now - last >= idleNanos) ? 0 : (int64_t)(idleNanos - (now - last))),
        DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
}

/* ---- IOPMrootDomain: the idle sleep timer ------------------------------ */

/*
 * getTimeToIdleSleep: milliseconds left before idle sleep, counted in whole
 * minutes of user inactivity, as xnu counts them.
 */
static uint32_t
getTimeToIdleSleep(void)
{
    uint64_t    now, lastActivityTime;
    uint32_t    minutesSinceUserInactive = 0;
    uint32_t    sleepDelay = 0;

    if (!idleSleepEnabled) {
        return 0xffffffff;
    }

    if (userActivityTime) {
        lastActivityTime = userActivityTime;
    } else {
        lastActivityTime = userBecameInactiveTime;
    }

    now = pmUptimeNanos();
    if ((lastActivityTime >= lastWakeTime) && (now > lastActivityTime)) {
        minutesSinceUserInactive = (uint32_t)((now - lastActivityTime) / (60 * NSEC_PER_SEC));

        if (minutesSinceUserInactive >= sleepSlider) {
            sleepDelay = 0;
        } else {
            sleepDelay = sleepSlider - minutesSinceUserInactive;
        }
    } else {
        sleepDelay = sleepSlider;
    }

    DEBUG_LOG("user inactive %u min, time to idle sleep %u min\n",
              minutesSinceUserInactive, sleepDelay);

    return sleepDelay * 60 * 1000;
}

static void handleSleepTimerExpiration(void);

static void
startIdleSleepTimer(uint32_t inMilliSeconds)
{
    if (!extraSleepTimer) {
        extraSleepTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _getPMMainQueue());
        if (!extraSleepTimer) {
            return;
        }
        dispatch_source_set_event_handler(extraSleepTimer, ^{ handleSleepTimerExpiration(); });
        dispatch_source_set_timer(extraSleepTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        dispatch_resume(extraSleepTimer);
    }

    if (inMilliSeconds < kMinimumTimeBeforeIdleSleep) {
        uint64_t    msec_since_wake = (pmUptimeNanos() - lastWakeTime) / NSEC_PER_MSEC;

        /*
         * Do not expire until at least kMinimumTimeBeforeIdleSleep ms after
         * the most recent wake, so clients can take assertions first.
         */
        if (msec_since_wake < kMinimumTimeBeforeIdleSleep) {
            uint32_t newIdleTimer = kMinimumTimeBeforeIdleSleep - (uint32_t)msec_since_wake;

            if (newIdleTimer > inMilliSeconds) {
                inMilliSeconds = newIdleTimer;
            }
        }
    }

    dispatch_source_set_timer(extraSleepTimer,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)inMilliSeconds * NSEC_PER_MSEC),
        DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
    idleSleepTimerPending = true;
    DEBUG_LOG("idle timer set for %u milliseconds\n", inMilliSeconds);
}

static void
cancelIdleSleepTimer(void)
{
    if (idleSleepTimerPending) {
        DEBUG_LOG("idle timer cancelled\n");
        dispatch_source_set_timer(extraSleepTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        idleSleepTimerPending = false;
    }
}

/*
 * adjustPowerState(true) and privateSleepSystem(kIOPMSleepReasonIdle): the
 * timer has run out. Sleep if the root domain allows it now, else try again
 * later, as xnu's systemDidNotSleep() does.
 */
static void
requestIdleSleep(void)
{
    struct iopmroot_state   st;
    CFMutableDictionaryRef  options;
    IOReturn                ret;

    if (!idleSleepEnabled) {
        return;
    }

    if (pmRootDomainCopyState(&st) != kIOReturnSuccess) {
        return;
    }
    if (!(st.flags & IOPMROOT_F_SLEEP_SUPPORTED)) {
        /* No S3 (QEMU without it, arm64): nothing to retry until settings change. */
        if (!cannotSleepLogged) {
            INFO_LOG("Idle sleep timer expired; this machine cannot sleep\n");
            cannotSleepLogged = true;
        }
        return;
    }

    /*
     * checkSystemSleepAllowed(): the sleep kill switch, a shutdown under
     * way, and the CPU assertion (PreventSystemSleep and friends) hold the
     * system awake. A sleep already under way needs no second request.
     */
    if ((st.flags & (IOPMROOT_F_SLEEP_DISABLED | IOPMROOT_F_SYSTEM_SHUTDOWN)) ||
        (userAssertionLevels & kIOPMDriverAssertionCPUBit) ||
        (st.phase != IOPMROOT_PHASE_ON))
    {
        DEBUG_LOG("Idle sleep prevented (flags 0x%x, assertions 0x%x, phase %u)\n",
                  st.flags, userAssertionLevels, st.phase);
        startIdleSleepTimer(kIdleSleepRetryInterval);
        return;
    }

    options = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks);
    if (options) {
        CFDictionarySetValue(options, CFSTR(kIOPMSleepReasonKey), CFSTR(kIOPMIdleSleepKey));
    }

    INFO_LOG("Idle sleep timer expired; requesting idle sleep\n");
    ret = IOPMSleepSystemWithOptions(getRootDomainConnect(), options);
    if (options) {
        CFRelease(options);
    }

    if (ret == kIOReturnUnsupported) {
        if (!cannotSleepLogged) {
            INFO_LOG("Idle sleep not performed: this machine cannot sleep\n");
            cannotSleepLogged = true;
        }
        return;
    }

    /*
     * The kernel asks kIOMessageCanSystemSleep and sleeps if no app
     * objects. If one does, or the request failed, xnu's systemDidNotSleep()
     * restarts the timer with kIdleSleepRetryInterval; on a wake,
     * checkForWake() starts over from the user's first activity.
     */
    if (ret != kIOReturnSuccess) {
        ERROR_LOG("Idle sleep request failed: 0x%x\n", ret);
    }
    startIdleSleepTimer(kIdleSleepRetryInterval);
}

static void
handleSleepTimerExpiration(void)
{
    uint64_t    now = pmUptimeNanos();

    DEBUG_LOG("sleep timer expired\n");
    idleSleepTimerPending = false;
    dispatch_source_set_timer(extraSleepTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);

    if (checkForWake()) {
        return;
    }

    /*
     * NextBSD: HID input reaches pmconfigd as kIOPMMessageRequestUserActive
     * only after a quiet spell; look at HIDIdleTime before sleeping too, and
     * treat input since the user became inactive as the tickle xnu would
     * have had.
     */
    if (lastUserActivityTime(now) > ((userActivityTime > userBecameInactiveTime) ?
                                     userActivityTime : userBecameInactiveTime)) {
        userActivityTime = lastUserActivityTime(now);
        if (wranglerAsleep) {
            INFO_LOG("Display is turned on\n");
        }
        evaluatePolicy(kStimulusDisplayWranglerWake);
        evaluatePolicy(kStimulusEnterUserActiveState);
        return;
    }

    requestIdleSleep();
}

/*
 * evaluatePolicy: xnu's IOPMrootDomain::evaluatePolicy() for the stimuli
 * that drive the idle sleep timer on a Mac with a display wrangler.
 */
static void
evaluatePolicy(int stimulus)
{
    union {
        struct {
            int idleSleepEnabled    : 1;
            int idleSleepDisabled   : 1;
            int sleepDelayChanged   : 1;
            int userBecameInactive  : 1;
        } bit;
        uint32_t u32;
    } flags;

    flags.u32 = 0;

    switch (stimulus) {
    case kStimulusDisplayWranglerSleep:
        if (!wranglerAsleep) {
            wranglerAsleep = true;
            publishDisplayStatus(false);
        }
        break;

    case kStimulusDisplayWranglerWake:
        if (wranglerAsleep) {
            wranglerAsleep = false;
            publishDisplayStatus(true);
        }
        break;

    case kStimulusEnterUserActiveState:
        if (!userIsActive) {
            userIsActive = true;
        }
        flags.bit.idleSleepDisabled = true;
        break;

    case kStimulusLeaveUserActiveState:
        if (userIsActive) {
            userIsActive = false;
            userBecameInactiveTime = pmUptimeNanos();
            flags.bit.userBecameInactive = true;
        }
        break;

    case kStimulusAggressivenessChanged:
    {
        uint32_t    minutesToIdleSleep  = 0;
        uint32_t    minutesToDisplayDim = 0;
        uint32_t    minutesDelta        = 0;

        minutesToIdleSleep = pmIdleSleepSlider;
        minutesToDisplayDim = displaySlider;

        /*
         * How long to wait before sleeping the system once the display
         * turns off is indicated by 'extraSleepDelay'.
         */
        if (minutesToIdleSleep > minutesToDisplayDim) {
            minutesDelta = minutesToIdleSleep - minutesToDisplayDim;
        } else if (minutesToIdleSleep == minutesToDisplayDim) {
            minutesDelta = 1;
        }

        if ((!idleSleepEnabled) && (minutesToIdleSleep != 0)) {
            idleSleepEnabled = flags.bit.idleSleepEnabled = true;
        }

        if ((idleSleepEnabled) && (minutesToIdleSleep == 0)) {
            flags.bit.idleSleepDisabled = true;
            idleSleepEnabled = false;
        }

        if ((minutesDelta != extraSleepDelay) &&
            !flags.bit.idleSleepEnabled && !flags.bit.idleSleepDisabled) {
            flags.bit.sleepDelayChanged = true;
        }

        sleepSlider = minutesToIdleSleep;
        extraSleepDelay = minutesDelta;
        cannotSleepLogged = false;
    }   break;
    }

    if (flags.bit.userBecameInactive || flags.bit.sleepDelayChanged) {
        if (flags.bit.sleepDelayChanged) {
            cancelIdleSleepTimer();
        }
        if (!userIsActive && idleSleepEnabled) {
            startIdleSleepTimer(getTimeToIdleSleep());
        }
    }

    if (flags.bit.idleSleepEnabled) {
        /* Start the timer if prefs now allow sleep and the user is inactive. */
        if (!userIsActive) {
            startIdleSleepTimer(getTimeToIdleSleep());
        }
    }

    if (flags.bit.idleSleepDisabled) {
        cancelIdleSleepTimer();
    }
}

/* ---- entry points ------------------------------------------------------ */

/*
 * IOPMSetAggressiveness() as pmconfigd sends it (RootDomain_freebsd.c):
 * kPMMinutesToSleep and kPMMinutesToDim. xnu's setAggressiveness()
 * queues kStimulusAggressivenessChanged; so does this.
 */
__private_extern__ void
pmIdleAggressivenessChanged(unsigned long type, unsigned long value)
{
    if ((type != kPMMinutesToSleep) && (type != kPMMinutesToDim)) {
        return;
    }

    dispatch_async(_getPMMainQueue(), ^{
        if (type == kPMMinutesToSleep) {
            pmIdleSleepSlider = (uint32_t)value;
        } else {
            displaySlider = (uint32_t)value;
        }
        evaluatePolicy(kStimulusAggressivenessChanged);
        evaluateDisplayIdle();
    });
}

/*
 * kPMSetUserAssertionLevels: xnu's evaluateAssertions(). The display
 * assertion makes the wrangler ignore its idle timer; the CPU assertion is
 * checked when the idle sleep timer expires.
 */
__private_extern__ void
pmIdleUserAssertionLevelsChanged(uint32_t levels)
{
    dispatch_async(_getPMMainQueue(), ^{
        userAssertionLevels = levels;
        evaluateDisplayIdle();
    });
}

/*
 * kPMActivityTickle (IOPMAssertionDeclareUserActivity, and the UserIsActive
 * assertion pmconfigd takes for the kernel's kIOPMMessageRequestUserActive
 * on input): the user is active now. The display wakes and the idle sleep
 * timer stops.
 */
__private_extern__ void
pmIdleActivityTickle(void)
{
    dispatch_async(_getPMMainQueue(), ^{
        (void)checkForWake();
        userActivityTime = pmUptimeNanos();
        if (wranglerAsleep) {
            INFO_LOG("Display is turned on\n");
            evaluatePolicy(kStimulusDisplayWranglerWake);
        }
        evaluatePolicy(kStimulusEnterUserActiveState);
        evaluateDisplayIdle();
    });
}

/*
 * IODisplayWrangler's "IORequestIdle" (pmset displaysleepnow,
 * kIOPMSetDisplaySleepNow): the display goes off now, as when its idle timer
 * expires, and the user counts as inactive, so the idle sleep timer starts.
 * Input or a user activity tickle turns it back on (pmIdleActivityTickle).
 */
__private_extern__ void
pmIdleDisplaySleepNow(void)
{
    dispatch_async(_getPMMainQueue(), ^{
        (void)checkForWake();
        if (wranglerAsleep) {
            return;
        }
        INFO_LOG("Display is turned off (requested)\n");
        evaluatePolicy(kStimulusDisplayWranglerSleep);
        evaluatePolicy(kStimulusLeaveUserActiveState);
        evaluateDisplayIdle();
    });
}

/* The system woke (kIOMessageSystemHasPoweredOn): every wake is a full wake. */
__private_extern__ void
pmIdleSystemDidWake(void)
{
    dispatch_async(_getPMMainQueue(), ^{
        (void)checkForWake();
        evaluateDisplayIdle();
    });
}

/* powerd_init(): note the kernel's sleep count, then start the wrangler. */
__private_extern__ void
pmIdle_prime(void)
{
    struct iopmroot_state   st;

    if (pmRootDomainCopyState(&st) == kIOReturnSuccess) {
        sleepCountSeen = st.sleep_count;
    }
    publishDisplayStatus(true);
    evaluateDisplayIdle();
}
