/*
 * BatteryTimeRemaining_freebsd.c — the power-source core of Apple's
 * pmconfigd/BatteryTimeRemaining.m, as C, until that file builds.
 *
 * BatteryTimeRemaining.m is Objective-C (Foundation types run through its
 * power-source paths) and depends on most of pmconfigd (PrivateLib.h,
 * PMSettings, PMAssertions, SystemLoad, UPSLowPower, battery health), so it
 * cannot be compiled yet. This file carries the part that turns the
 * kernel's IOPMPowerSource registry entries into what IOKit's power-source
 * clients read, kept in Apple's order, with Apple's names and, where it
 * does not touch Foundation, Apple's code as it stands:
 *
 *   initNotifictions()            IOServiceAddMatchingNotification on
 *   ioregBatteryMatch()           IOPMPowerSource, then a general-interest
 *   ioregBatteryInterest()        notification per battery; each
 *   ioregBatteryProcess()         kIOPMMessageBatteryStatusHasChanged
 *   _batteryChanged()             re-reads the entry's properties
 *   _unpackBatteryState()         (the Intel macOS branch)
 *   kernelPowerSourcesDidChange() percent, discontinuities, then
 *   packageKernelPowerSource()    the kIOPS dictionary of the internal
 *                                 battery (gPSList slot 0, psid 99)
 *   HandlePublishAllPowerSources() the notify(3) state and posts:
 *     publish_IOPSGetTimeRemainingEstimate  com.apple.system.powersources.
 *                                           timeremaining (packed bits)
 *     publish_IOPSGetPercentRemaining       ...powersources.percent
 *     publish_IOPSBatteryGetWarningLevel    State:/IOKit/LowBatteryWarning
 *                                           + ...powersources.lowbattery
 *   _io_ps_new_pspowersource / _io_ps_update_pspowersource /
 *   _io_ps_release_pspowersource / _io_ps_copy_powersources_info
 *                                 the MIG back end of IOPSCreatePowerSource,
 *                                 IOPSSetPowerSourceDetails and
 *                                 IOPSCopyPowerSourcesInfo
 *   getActivePSType(), getPowerState(), _getPowerSource(), _batteryCount(),
 *   isFullyCharged(), BatteryTimeRemainingWakeNotification() for the rest
 *                                 of pmconfigd
 *
 * Where NextBSD differs from the Mac, the code says so (#ifdef __FreeBSD__
 * blocks in the original are folded in; each difference has a "NextBSD:"
 * comment):
 *
 *   - One internal battery from several entries. The kernel publishes one
 *     IOPMPowerSource per ACPI battery (a ThinkPad T460s has BAT0 and BAT1);
 *     a Mac has exactly one AppleSmartBattery. Apple's model is one internal
 *     battery (control.internal, gPSList slot 0), so the per-entry
 *     IOPMBatteries are folded into one, InternalBattery-0, before Apple's
 *     publishing code sees them: capacities and current are summed, and the
 *     time remaining is computed over the sum.
 *   - Time remaining. This drop of BatteryTimeRemaining.m no longer fills
 *     swCalculatedTR. It is computed from the folded battery's capacity and
 *     current, and held "unknown" for the battery's InvalidWakeSeconds
 *     (Apple's 16 s default) after a discontinuity, as Apple's comment for
 *     kInvalidWakeSecsDefault describes.
 *   - No battery. With no IOPMPowerSource (QEMU, desktops, the RPi 5) the
 *     state bits still say "valid, external, provided by AC", so
 *     IOPSGetProvidingPowerSourceType() is "AC Power" and
 *     IOPSDrawingUnlimitedPower() is true; IOPSCopyPowerSourcesInfo() gets
 *     an empty list rather than an error.
 *   - No polling. There is no Apple Smart Battery Manager to poke: the
 *     kernel re-reads the batteries itself and messages each entry when a
 *     value changes (nextbsd-kernel#244, #399). startBatteryPoll() keeps its
 *     checks but arms no timer, and IOPSRequestBatteryUpdate() is not
 *     called.
 *   - Battery removal. The kernel retires an entry when its battery is
 *     pulled (kIOMessageServiceIsTerminated) and publishes a new one when it
 *     comes back; a Mac's entry never goes away. The retired battery is
 *     dropped from the list.
 *
 * Not ported (about 60% of BatteryTimeRemaining.m): battery health and
 * service state, nominal capacity and calibration, NVRAM/persistent health
 * data, authentication, Low Power Mode, custom battery properties, the
 * adapter-details XPC message (the kernel publishes no AdapterDetails), the
 * precise/imprecise capacity split, FDR and kdebug tracing. The calls into
 * subsystems that are not built yet (SystemLoad, PMAssertions,
 * PMConnection, UPSLowPower, TCP keepalive, proximity) are marked where
 * Apple makes them, for the tickets that port those files.
 */
#include <sys/types.h>

#include <limits.h>
#include <math.h>
#include <notify.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <IOKit/pwr_mgt/powermanagement_mig.h>

#include "PrivateLib_freebsd.h"
#include "PMSettings.h"			/* PMSettingsPSChange() */
#include "powermanagementServer.h"	/* MIG: the _io_ps_* prototypes */

os_log_t    battery_log = NULL;
#undef   LOG_STREAM
#define  LOG_STREAM   battery_log

#define kPSMaxCount   16

/* PSStruct
 * Contains all the details about each power source that the system describes.
 * This struct is the backbone of the IOPowerSources() IOKit API for
 * power source reporting.
 */
typedef struct {
    // powerd will assign a unique psid to all sources.
    int                 psid;
    psTypes_t           psType;

    // Ensure that only the process that created
    // a ps may modify it or destroy it, by recording caller's pid.
    int                 pid;
    dispatch_source_t   procdeathsrc;

    // This is the most current recorded state of this power source.
    CFDictionaryRef     description;
} PSStruct;

static PSStruct gPSList[kPSMaxCount];

#define kDiscontinuitySettle    60

#define kMaxBattMinutes     1200

#define kSpecialInternalBatteryID  99

// static global variables for tracking battery state
typedef struct {
    CFAbsoluteTime   lastDiscontinuity;
    bool             percentageDiscontinuity;
    int              systemWarningLevel;
    bool             warningsShouldResetForSleep;
    bool             readACAdapterAgain;
    bool             selectionHasSwitched;
    int              psTimeRemainingNotifyToken;
    int              psPercentChangeNotifyToken;
    bool             noPoll;
    bool             needsNotifyAC;
    PSStruct         *internal;
} BatteryControl;
static BatteryControl   control;
static CFDictionaryRef  adapterDetails = NULL;
static dispatch_queue_t batteryTimeRemainingQ;
static int currentPercentRemaining = 0;
static long physicalBatteriesCount = 0;
static IOPMBattery **physicalBatteriesArray = NULL;
static CFMutableSetRef physicalBatteriesSet = NULL;
static CFStringRef gBatterySerialNumber = NULL;

/*
 * NextBSD: the internal battery Apple's publishing code works on, folded
 * from the physical batteries above (one per IOPMPowerSource entry). With
 * at least one physical battery present, _batteries() returns this one,
 * and _batteryCountSync() is 1.
 */
static IOPMBattery  gInternalBattery;
static IOPMBattery  *gInternalBatteryArray[1] = { &gInternalBattery };
static bool         gInternalBatteryValid = false;

/*
 * The queue checks. Apple asserts dispatch_assert_queue_barrier() for the
 * mutating paths; batteryTimeRemainingQ is serial, so every block on it is
 * already exclusive and the plain check is the same test.
 */
static void _internal_dispatch_assert_queue(dispatch_queue_t queue) {
    dispatch_assert_queue(queue);
}

static void _internal_dispatch_assert_queue_barrier(dispatch_queue_t queue) {
    dispatch_assert_queue(queue);
}

static void _internal_dispatch_assert_queue_not(dispatch_queue_t queue) {
    dispatch_assert_queue_not(queue);
}

static PSStruct         *iops_newps(int pid, int psid);
static void             checkTimeRemainingValid(IOPMBattery **batts);
static void             packageKernelPowerSource(IOPMBattery *b, PSStruct *ps);
static void             HandlePublishAllPowerSources(void);
static IOReturn         HandleAccessoryPowerSources(PSStruct *ps, CFDictionaryRef update);
static CFDictionaryRef  getPSByType(CFStringRef type);
static int              getActivePSType_sync(void);
static CFDictionaryRef  getActiveUPSDictionary_sync(void);
static int              _batteryCountSync(void);
static PowerSources     _getPowerSourceSync(void);
static void             BatteryTimeRemaining_finishSync(void);
static void             _foldInternalBattery(void);

// Arguments For startBatteryPoll()
typedef enum {
    kPeriodicPoll           = 0,
    kImmediateFullPoll      = 1
} PollCommand;
static bool             startBatteryPoll(PollCommand x);

static void BatteryTimeRemaining_notify_post(const char *token)
{
    powerd_notify_and_log(battery_log, token);
}

/*
 * NextBSD: the MIG audit token's pid, in the Darwin layout (val[5]) that
 * NextBSD's Mach trailer uses; Apple calls audit_token_to_au32().
 */
static pid_t
_auditTokenPID(audit_token_t token)
{
    return ((pid_t)token.val[5]);
}

__private_extern__ dispatch_queue_t BatteryTimeRemaining_getQ(void)
{
    return batteryTimeRemainingQ;
}

/* If the battery doesn't specify an alternative time, we wait 16 seconds
   of ignoring the battery's (or our own) time remaining estimate.
*/
enum {
    kInvalidWakeSecsDefault = 16
};

static void _unpackBatteryState(IOPMBattery *b, CFDictionaryRef prop)
{
    CFBooleanRef    boo;
    CFNumberRef     n;

    if (!isA_CFDictionary(prop)) return;

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSExternalConnectedKey));
    b->externalConnected = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSExternalChargeCapableKey));
    b->externalChargeCapable = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSBatteryInstalledKey));
    b->isPresent = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSIsChargingKey));
    b->isCharging = (kCFBooleanTrue == boo);

    b->rawExternalConnected = b->externalConnected;

    /*
     * NextBSD: Apple reads AtCriticalLevel only in its iOS / Apple-silicon
     * branch; the Intel branch, used everywhere else here, has no source
     * for it. The kernel's entries carry it on every platform, and it is
     * what drives kPSCriticalLevelBit and ...powersources.criticallevel.
     */
    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSAtCriticalLevelKey));
    b->isCritical = (kCFBooleanTrue == boo);

    b->failureDetected = (CFStringRef)CFDictionaryGetValue(prop, CFSTR(kIOPMPSErrorConditionKey));

    b->batterySerialNumber = (CFStringRef)CFDictionaryGetValue(prop, CFSTR(kIOPMPSSerialKey));

    b->chargeStatus = (CFStringRef)CFDictionaryGetValue(prop, CFSTR(kIOPMPSBatteryChargeStatusKey));

    /* _getLowCapRatioTime(): battery-health preferences, not ported. */
    b->hasLowCapRatio = false;
    b->lowCapRatioSinceTime = 0;

    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSVoltageKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->voltage);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCurrentCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->currentCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSMaxCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->maxCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSDesignCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->designCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSTimeRemainingKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->hwAverageTR);
    }


    n = CFDictionaryGetValue(prop, CFSTR("InstantAmperage"));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->instantAmperage);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSAmperageKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->avgAmperage);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSMaxErrKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->maxerr);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCycleCountKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->cycleCount);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSLocationKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->location);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSInvalidWakeSecondsKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->invalidWakeSecs);
    } else {
        b->invalidWakeSecs = kInvalidWakeSecsDefault;
    }
    n = CFDictionaryGetValue(prop, CFSTR("PermanentFailureStatus"));
    if (n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->pfStatus);
    } else {
        b->pfStatus = 0;
    }

    return;
}

static void initializeBatteryCalculations(void)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    if ((_batteryCountSync() == 0) || (control.internal != NULL)) {
        return;
    }

    // Does this Mac have an internal battery
    // reported through IOPMPowerSource?
    // If so, we'll track it in the gPSList.

    // Any other processes that publish power sources (like upsd)
    // will get a powersource id > 5000
    control.internal = iops_newps(getpid(), kSpecialInternalBatteryID);
    control.internal->psType = kPSTypeIntBattery;

    control.lastDiscontinuity = CFAbsoluteTimeGetCurrent();
    BatteryTimeRemaining_notify_post(kIOPSNotifyAttach);

    return;
}

// _readAndPublicACAdapter
static void readAndPublishACAdapter(bool adapterExists, CFDictionaryRef newAdapter)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    if (!adapterExists && !adapterDetails) {
        goto exit;
    }

    // Make sure we re-read the adapter on wake from sleep
    if (control.readACAdapterAgain) {
        control.readACAdapterAgain = false;
        if (adapterDetails) {
            CFRelease(adapterDetails);
            adapterDetails = NULL;
        }
    }

    if (adapterExists) {
        if (!isA_CFDictionary(newAdapter)) {
            goto exit;
        }
        if (adapterDetails && CFEqual(newAdapter, adapterDetails)) {
            goto exit;
        }
    }
    else {
        newAdapter = NULL;
    }

    if (adapterDetails) {
        CFRelease(adapterDetails);
        adapterDetails = NULL;
    }

    if (newAdapter) {
        adapterDetails = CFRetain(newAdapter);
    }

    BatteryTimeRemaining_notify_post(kIOPSNotifyAdapterChange);

exit:
    return ;
}

__private_extern__ CFDictionaryRef batteryTimeRemaining_copyIOPMPowerSourceDictionary(void)
{
    __block CFDictionaryRef ret = NULL;

    _internal_dispatch_assert_queue_not(batteryTimeRemainingQ);

    dispatch_sync(batteryTimeRemainingQ, ^(void){
        IOPMBattery **b = _batteries();
        if (b && b[0] && isA_CFDictionary(b[0]->properties)) {
            ret = CFDictionaryCreateCopy(kCFAllocatorDefault, b[0]->properties);
        }
    });
    if (!ret) {
        ERROR_LOG("CFDictCreate returned nil");
    }
    return ret;
}

__private_extern__ CFStringRef batteryTimeRemaining_getBatterySerialNumber(void)
{
    return gBatterySerialNumber;
}

static void _batteryChanged(IOPMBattery *changed_battery)
{
    kern_return_t       kr;

    CFMutableDictionaryRef props = NULL;
    CFBooleanRef externalConnected = kCFBooleanFalse;
    CFBooleanRef battInstalled = kCFBooleanFalse;

    if (!changed_battery) {
        // This is unexpected; we're not tracking this battery
        return;
    }

    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    // Free the last set of properties
    if (changed_battery->properties) {
        CFRelease(changed_battery->properties);
        changed_battery->properties = NULL;
    }
    kr = IORegistryEntryCreateCFProperties(
            changed_battery->me,
            &props,
            kCFAllocatorDefault, 0);
    if (KERN_SUCCESS != kr) {
        goto exit;
    }

    if (CFDictionaryGetValueIfPresent(props, CFSTR(kIOPMPSBatteryInstalledKey), (const void **)&battInstalled) &&
            (battInstalled == kCFBooleanTrue)) {

        // Skip battery if there isn't a certain set of minimum information.
        // We expect at least current- and max capacity to be reported from
        // every PS. Assume the PS isn't ready yet if those are missing.
        if (!CFDictionaryGetValue(props, CFSTR(kIOPMPSCurrentCapacityKey)) ||
            !CFDictionaryGetValue(props, CFSTR(kIOPMPSMaxCapacityKey))) {
            ERROR_LOG("Battery does not report SOC");
            CFRelease(props);
            props = NULL;
            goto exit;
        }

        _unpackBatteryState(changed_battery, props);
    }
    else {
        // There is no battery here. May be just an adapeter
        CFDictionaryRef cfAdapter = NULL;

        DEBUG_LOG("Battery is not installed\n");
        CFDictionaryGetValueIfPresent(props, CFSTR(kIOPMPSExternalConnectedKey), (const void **)&externalConnected);
        CFDictionaryGetValueIfPresent(props, CFSTR(kIOPMPSAdapterDetailsKey), (const void **)&cfAdapter);

        if (isA_CFDictionary(cfAdapter)) {
            readAndPublishACAdapter((externalConnected == kCFBooleanTrue) ? true : false, cfAdapter);
        }
        CFRelease(props);
        props = NULL;
    }

exit:
    changed_battery->properties = props;
    return;
}

/*
 * NextBSD: fold the physical batteries into gInternalBattery.
 *
 * The kernel publishes one IOPMPowerSource per ACPI battery, each with its
 * own capacity (mAh), current and state; a ThinkPad drains its batteries
 * one after the other, so the idle one reads 0 mA. Apple's code expects one
 * internal battery. It gets one: capacities and currents are summed, the
 * flags are "any" for AC, charging and charge-capable and "all" for
 * critical (one empty battery beside a full one is not critical), and the
 * identity (serial, name, cycle count) is the first battery's by registry
 * name (BAT0). The time remaining is computed over the sums; this drop of
 * BatteryTimeRemaining.m does not fill swCalculatedTR, and a ThinkPad's
 * per-battery estimates cannot be added (the idle battery has none).
 *
 * properties is the first battery's dictionary with the folded numbers in
 * place, for batteryTimeRemaining_copyIOPMPowerSourceDictionary() callers.
 */
static int
_comparePhysicalBatteries(const void *a, const void *b)
{
    const IOPMBattery *ba = *(IOPMBattery * const *)a;
    const IOPMBattery *bb = *(IOPMBattery * const *)b;

    if (ba->ioregName && bb->ioregName)
        return (int)CFStringCompare(ba->ioregName, bb->ioregName, 0);
    return (ba < bb) ? -1 : (ba > bb) ? 1 : 0;
}

static void
_dictSetInt(CFMutableDictionaryRef d, const char *key, int val)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &val);
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);

    if (n && k) {
        CFDictionarySetValue(d, k, n);
    }
    if (n) CFRelease(n);
    if (k) CFRelease(k);
}

static void _foldInternalBattery(void)
{
    IOPMBattery     *c = &gInternalBattery;
    IOPMBattery     *first = NULL;
    int             present = 0, critical = 0;
    int             amperage = 0, instantAmperage = 0;
    int             minutes = -1;

    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    if (physicalBatteriesCount > 1) {
        qsort(physicalBatteriesArray, (size_t)physicalBatteriesCount,
              sizeof(IOPMBattery *), _comparePhysicalBatteries);
    }

    if (c->properties) {
        CFRelease(c->properties);
        c->properties = NULL;
    }

    c->externalConnected = c->rawExternalConnected = 0;
    c->externalChargeCapable = c->isCharging = 0;
    c->isPresent = c->isCritical = c->fullyCharged = 0;
    c->currentCap = c->maxCap = c->designCap = 0;
    c->pfStatus = 0;
    c->invalidWakeSecs = kInvalidWakeSecsDefault;

    for (long i = 0; i < physicalBatteriesCount; i++) {
        IOPMBattery *b = physicalBatteriesArray[i];

        if (!b->properties || !b->isPresent) {
            continue;
        }
        if (!first) {
            first = b;
        }
        present++;
        c->externalConnected |= b->externalConnected;
        c->rawExternalConnected |= b->rawExternalConnected;
        c->externalChargeCapable |= b->externalChargeCapable;
        c->isCharging |= b->isCharging;
        if (b->isCritical) {
            critical++;
        }
        c->currentCap += b->currentCap;
        c->maxCap += b->maxCap;
        c->designCap += b->designCap;
        amperage += b->avgAmperage;
        instantAmperage += b->instantAmperage;
        c->pfStatus |= b->pfStatus;
        if (b->invalidWakeSecs > c->invalidWakeSecs) {
            c->invalidWakeSecs = b->invalidWakeSecs;
        }
    }

    gInternalBatteryValid = (first != NULL);
    if (!first) {
        return;
    }

    c->isPresent = 1;
    c->isCritical = (critical == present);
    c->avgAmperage = amperage;
    c->instantAmperage = instantAmperage;
    c->voltage = first->voltage;
    c->cycleCount = first->cycleCount;
    c->maxerr = first->maxerr;
    c->location = first->location;
    c->batterySerialNumber = first->batterySerialNumber;
    c->failureDetected = first->failureDetected;
    c->chargeStatus = first->chargeStatus;
    c->hasLowCapRatio = false;
    c->lowCapRatioSinceTime = 0;

    if (!gBatterySerialNumber && isA_CFString(c->batterySerialNumber)) {
        gBatterySerialNumber = c->batterySerialNumber;
        CFRetain(gBatterySerialNumber);
    }

    /* Minutes to empty / to full over the summed battery. */
    if (c->isCharging) {
        if (amperage > 0 && c->maxCap >= c->currentCap) {
            minutes = ((c->maxCap - c->currentCap) * 60) / amperage;
        }
    } else if (c->externalConnected) {
        minutes = 0;
    } else if (amperage < 0) {
        minutes = (c->currentCap * 60) / -amperage;
    }
    c->hwAverageTR = (minutes < 0) ? 0 : minutes;
    if (CFAbsoluteTimeGetCurrent() < (control.lastDiscontinuity + c->invalidWakeSecs)) {
        minutes = -1;
    }
    c->swCalculatedTR = minutes;

    c->properties = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, first->properties);
    if (c->properties) {
        _dictSetInt(c->properties, kIOPMPSCurrentCapacityKey, c->currentCap);
        _dictSetInt(c->properties, kIOPMPSMaxCapacityKey, c->maxCap);
        _dictSetInt(c->properties, kIOPMPSDesignCapacityKey, c->designCap);
        _dictSetInt(c->properties, kIOPMPSAmperageKey, c->avgAmperage);
        _dictSetInt(c->properties, kIOPMPSTimeRemainingKey, c->hwAverageTR);
        CFDictionarySetValue(c->properties, CFSTR(kIOPMPSExternalConnectedKey),
                             c->externalConnected ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(c->properties, CFSTR(kIOPMPSIsChargingKey),
                             c->isCharging ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(c->properties, CFSTR(kIOPMPSAtCriticalLevelKey),
                             c->isCritical ? kCFBooleanTrue : kCFBooleanFalse);
    }
}

static void ioregBatteryProcess(IOPMBattery *changed_batt, io_service_t batt)
{
    if (!changed_batt) {
        return;
    }

    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    PowerSources oldPS = _getPowerSourceSync();

    // Update the arbiter
    changed_batt->me = (io_registry_entry_t)batt;
    _batteryChanged(changed_batt);

    if (changed_batt->properties == NULL) {
        // Nothing to do
        return;
    }

    /* NextBSD: publish the folded internal battery, not this entry. */
    _foldInternalBattery();
    initializeBatteryCalculations();
    kernelPowerSourcesDidChange(kInternalBattery);

    /*
     * Apple continues with SystemLoadBatteriesHaveChanged(),
     * InternalEvaluateAssertions(), InternalEvalConnections() and, when
     * the source changed, evalTcpkaForPSChange() / evalProxForPSChange();
     * those subsystems are not built yet.
     */
    PowerSources newPS = _getPowerSourceSync();
    if (newPS != oldPS) {
        INFO_LOG("Power source is now %{public}s\n", (newPS == kACPowered) ? "AC" : "Batt");
    }

    return;
}

/*
 * NextBSD: the kernel retired this battery's entry (battery pulled, or the
 * battery device detached). Drop it, and re-publish what is left; with no
 * battery left, the internal source goes away and the AC-only state is
 * published.
 */
static void _batteryRemoved(IOPMBattery *gone)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    if (!physicalBatteriesSet || !CFSetContainsValue(physicalBatteriesSet, gone)) {
        return;
    }

    INFO_LOG("Battery removed: %@\n", gone->ioregName);

    CFSetRemoveValue(physicalBatteriesSet, gone);
    physicalBatteriesCount = CFSetGetCount(physicalBatteriesSet);
    free(physicalBatteriesArray);
    physicalBatteriesArray = NULL;
    if (physicalBatteriesCount) {
        physicalBatteriesArray = (IOPMBattery **)calloc(physicalBatteriesCount, sizeof(IOPMBattery *));
        CFSetGetValues(physicalBatteriesSet, (const void **)physicalBatteriesArray);
    }

    if (gone->msg_port) {
        IOObjectRelease(gone->msg_port);
    }
    if (gone->properties) {
        CFRelease(gone->properties);
    }
    if (gone->name) {
        CFRelease(gone->name);
    }
    if (gone->ioregName) {
        CFRelease(gone->ioregName);
    }
    free(gone);

    _foldInternalBattery();
    if (!gInternalBatteryValid && control.internal) {
        if (control.internal->description) {
            CFRelease(control.internal->description);
        }
        bzero(control.internal, sizeof(PSStruct));
        control.internal = NULL;
        control.needsNotifyAC = true;
        BatteryTimeRemaining_notify_post(kIOPSNotifyAttach);
        HandlePublishAllPowerSources();
        return;
    }
    kernelPowerSourcesDidChange(kInternalBattery);
}

static void ioregBatteryInterest(
    void *refcon,
    io_service_t batt,
    uint32_t messageType,
    void *messageArgument __unused)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    IOPMBattery *changed_batt = (IOPMBattery *)refcon;

    /* NextBSD: see _batteryRemoved(). */
    if (kIOMessageServiceIsTerminated == messageType) {
        _batteryRemoved(changed_batt);
        return;
    }

    /*
     * The kernel sends kIOMessageServicePropertyChange and then this
     * message for every change; act once, on this one, as Apple does.
     */
    if (kIOPMMessageBatteryStatusHasChanged != messageType) {
        return;
    }
    ioregBatteryProcess(changed_batt, batt);
}

__private_extern__ IOPMBattery **_batteries(void)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    /* NextBSD: the folded internal battery; see _foldInternalBattery(). */
    return gInternalBatteryValid ? gInternalBatteryArray : NULL;
}

static IOPMBattery *_newBatteryFound(io_registry_entry_t where)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    IOPMBattery *new_battery = NULL;
    static int new_battery_index = 0;
    io_name_t   regName;
    // Populate new battery in array
    new_battery = calloc(1, sizeof(IOPMBattery));
    new_battery->me = where;
    new_battery->name = CFStringCreateWithFormat(
                            kCFAllocatorDefault,
                            NULL,
                            CFSTR("InternalBattery-%d"),
                            new_battery_index);
    if (IORegistryEntryGetName(where, regName) == KERN_SUCCESS) {
        new_battery->ioregName = CFStringCreateWithCString(kCFAllocatorDefault,
                                    regName, kCFStringEncodingUTF8);
    }

    new_battery_index++;
    INFO_LOG("Found battery: %d (%@)", new_battery_index, new_battery->ioregName);

    /* Real, physical battery found */
    if (!physicalBatteriesSet) {
        physicalBatteriesSet = CFSetCreateMutable(0, 1, NULL);
    }
    CFSetAddValue(physicalBatteriesSet, new_battery);
    physicalBatteriesCount = CFSetGetCount(physicalBatteriesSet);
    if (physicalBatteriesArray) {
        free(physicalBatteriesArray);
        physicalBatteriesArray = NULL;
    }
    physicalBatteriesArray = (IOPMBattery **)calloc(physicalBatteriesCount, sizeof(IOPMBattery *));
    CFSetGetValues(physicalBatteriesSet, (const void **)physicalBatteriesArray);

    return new_battery;
}


static void ioregBatteryMatch(
    void *refcon,
    io_iterator_t b_iter)
{
    IOPMBattery                 *tracking;
    IONotificationPortRef       notify = (IONotificationPortRef)refcon;
    io_registry_entry_t         battery;
    io_object_t                 notification_ref;
    kern_return_t               kr;

    _internal_dispatch_assert_queue(batteryTimeRemainingQ);

    while ((battery = (io_registry_entry_t)IOIteratorNext(b_iter))) {
        // Add battery to our list of batteries
        tracking = _newBatteryFound(battery);

        // And install an interest notification on it
        notification_ref = IO_OBJECT_NULL;
        kr = IOServiceAddInterestNotification(notify, battery,
                            kIOGeneralInterest, ioregBatteryInterest,
                            (void *)tracking, &notification_ref);

        tracking->msg_port = notification_ref;
        ioregBatteryProcess(tracking, battery);
        /*
         * The registration holds the entry for as long as it lives, so
         * tracking->me stays valid; without one (NextBSD: a kernel without
         * interest messages), keep this reference instead.
         */
        if (KERN_SUCCESS == kr) {
            IOObjectRelease(battery);
        } else {
            ERROR_LOG("Failed to install interest notification for %@: 0x%x\n",
                      tracking->ioregName, kr);
        }
    }
    /*
     * Apple: InternalEvaluateAssertions(), InternalEvalConnections(),
     * evalTcpkaForPSChange(); not built yet.
     */

    /* Do initial full poll and kick off the polling timer */
    dispatch_async(batteryTimeRemainingQ, ^() {
        startBatteryPoll(kImmediateFullPoll);
    });

}

static void initNotifictions(void)
{
    IONotificationPortRef notify = IONotificationPortCreate(0);
    if (!notify) {
        return;
    }

    IONotificationPortSetDispatchQueue(notify, batteryTimeRemainingQ);

    io_iterator_t battery_iter = 0;
    kern_return_t kr = IOServiceAddMatchingNotification(
                                notify,
                                kIOFirstMatchNotification,
                                IOServiceMatching("IOPMPowerSource"),
                                ioregBatteryMatch,
                                (void *)notify,
                                &battery_iter);

    if (kr == KERN_SUCCESS) {
        // Install notifications on existing instances.
        ioregBatteryMatch((void *)notify, battery_iter);
    } else {
        ERROR_LOG("Failed to install notification for IOPMPS: %d", kr);
    }

}

__private_extern__ void BatteryTimeRemaining_prime(void)
{

    battery_log = os_log_create(PM_LOG_SYSTEM, BATTERY_LOG);
    gInternalBattery.name = CFSTR("InternalBattery-0");
    bzero(gPSList, sizeof(gPSList));
    bzero(&control, sizeof(BatteryControl));

     batteryTimeRemainingQ = dispatch_queue_create("com.apple.private.powerd.batteryTimeRemainingQ", DISPATCH_QUEUE_SERIAL);
     if (!batteryTimeRemainingQ) {
         return;
     }

    notify_register_check(kIOPSTimeRemainingNotificationKey,
                          &control.psTimeRemainingNotifyToken);
    notify_register_check(kIOPSNotifyPercentChange,
                          &control.psPercentChangeNotifyToken);

     dispatch_sync(batteryTimeRemainingQ, ^() {
         initNotifictions();

     });

    /* Do initial full poll and kick off the polling timer */
    dispatch_async(batteryTimeRemainingQ, ^() {
        startBatteryPoll(kImmediateFullPoll);
    });

    /*
     * NextBSD: with no battery entry (yet), publish the AC-only state, so
     * clients asking before a battery shows up (or on a machine without
     * one) read "AC Power" rather than nothing. A battery found later
     * replaces it through ioregBatteryMatch().
     */
    dispatch_async(batteryTimeRemainingQ, ^() {
        if (_batteryCountSync() == 0) {
            control.needsNotifyAC = true;
            HandlePublishAllPowerSources();
        }
    });
}

static void BatteryTimeRemaining_finishSync(void)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    /* don't wait for notification if we already have battery info */

    /*
     * Apple re-reads its one IOPMPowerSource here; NextBSD re-reads each
     * physical battery and re-folds them.
     */
    for (long i = 0; i < physicalBatteriesCount; i++) {
        IOPMBattery *b = physicalBatteriesArray[i];
        if (b->me) {
            ioregBatteryProcess(b, b->me);
        }
    }
}

__private_extern__ void BatteryTimeRemaining_finish(void)
{
    dispatch_async(batteryTimeRemainingQ, ^() {
        BatteryTimeRemaining_finishSync();
    });
}

/*
 * A battery time remaining discontinuity has occurred
 * Make sure we don't publish a time remaining estimate at all
 * until a given period has elapsed.
 */
static void _discontinuityOccurred(bool isWake __unused, bool percentageDiscontinuity)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    control.lastDiscontinuity = CFAbsoluteTimeGetCurrent();
    control.percentageDiscontinuity = percentageDiscontinuity;


    // Kick off a battery poll now,
    // and schedule the next poll in exactly 60 seconds.
    startBatteryPoll(kImmediateFullPoll);

    /*
     * NextBSD: Apple's next poll re-evaluates the estimate once the
     * battery's InvalidWakeSeconds have passed. Without a poll, re-evaluate
     * once, then; the kernel's change messages take over after that.
     */
    {
        int64_t secs = (gInternalBatteryValid ? gInternalBattery.invalidWakeSecs : kInvalidWakeSecsDefault) + 1;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, secs * NSEC_PER_SEC),
                       batteryTimeRemainingQ, ^{
            if (gInternalBatteryValid) {
                _foldInternalBattery();
                kernelPowerSourcesDidChange(kInternalBattery);
            }
        });
    }
}

__private_extern__ void
BatteryTimeRemainingWakeNotification(void)
{
    dispatch_barrier_async(batteryTimeRemainingQ, ^() {
        control.warningsShouldResetForSleep = true;
        control.readACAdapterAgain = true;

        _discontinuityOccurred(true, true);
    });
}

/*
 * startBatteryPoll
 *
 * Apple: ask the Apple Smart Battery Manager for fresh data
 * (IOPSRequestBatteryUpdate) and re-arm a 60 s timer, unless the ASBM's
 * BootPathUpdated / FullPathUpdated / UserVisiblePathUpdated timestamps
 * show a recent read.
 *
 * NextBSD: there is no Smart Battery Manager and no such timestamps. The
 * kernel re-reads every battery on each ACPI notify, on AC change, after
 * resume and every 60 s, and messages the entry when anything changed
 * (nextbsd-kernel#244, #399). So the request is a no-op and no timer is
 * armed; the checks stay so callers see Apple's return value.
 */
static bool startBatteryPoll(PollCommand doCommand __unused)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);

    if (!_batteries()) {
        DEBUG_LOG("No Battery to poll.");
        return false;
    }

    if (control.noPoll) {
        ERROR_LOG("Battery polling is disabled. powerd is skipping this battery udpate request.");
        return false;
    }

    return true;
}

#define kSocThresholdEarly          10
#define kSocThresholdFinal          2

static void publish_IOPSBatteryGetWarningLevel(
    IOPMBattery *b,
    int combinedTime,
    int percent)
{
    /* Display a system low battery warning?
     *
     * No Warning == AC Power or > kSocThresholdEarly % UISOC
     * Early Warning == On Battery with <= kSocThresholdEarly % UISOC
     * Final Warning == On Battery with <= kSocThresholdFinal % UISOC
     */

    static CFStringRef lowBatteryKey = NULL;
    static int prevLoggedLevel = kIOPSLowBatteryWarningNone;
    int newWarningLevel = kIOPSLowBatteryWarningNone;

    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    if (control.warningsShouldResetForSleep || b->externalConnected) {
        // We reset the warning level upon system sleep or when external
        // power source is connected

        control.warningsShouldResetForSleep = false;
        if (control.systemWarningLevel != kIOPSLowBatteryWarningNone) {
            control.systemWarningLevel = 0;
            newWarningLevel = kIOPSLowBatteryWarningNone;
        }
    } else if (!control.percentageDiscontinuity) {
        if (percent <= kSocThresholdFinal) {
            newWarningLevel = kIOPSLowBatteryWarningFinal;
        } else if (percent <= kSocThresholdEarly) {
            newWarningLevel = kIOPSLowBatteryWarningEarly;
        }
    }

    if (newWarningLevel < control.systemWarningLevel) {
        // kIOPSLowBatteryWarningNone  = 1,
        // kIOPSLowBatteryWarningEarly = 2,
        // kIOPSLowBatteryWarningFinal = 3
        //
        // Warning level may only increase.
        // Once we enter a >1 warning level, we can only reset it by
        // (1) having AC power re-applied, or (2) hibernating
        // and waking with a new battery.
        //
        // This prevents fluctuations in battery capacity from causing
        // multiple battery warnings.

        newWarningLevel = control.systemWarningLevel;
    }

    if ( (newWarningLevel != control.systemWarningLevel)
        && (0 != newWarningLevel) ) {
        CFNumberRef newlevel = CFNumberCreate(0, kCFNumberIntType, &newWarningLevel);

        if (newlevel) {
            if (!lowBatteryKey) {
                /*
                 * Apple: SCDynamicStoreKeyCreate(), which NextBSD's
                 * SystemConfiguration does not export; same format.
                 */
                lowBatteryKey = CFStringCreateWithFormat(
                        kCFAllocatorDefault, NULL, CFSTR("%@%@"),
                        kSCDynamicStoreDomainState, CFSTR(kIOPSDynamicStoreLowBattPathKey));
            }

            if (lowBatteryKey) {
                PMStoreSetValue(lowBatteryKey, newlevel );
            } else {
                ERROR_LOG("Failed to create lowBatteryKey\n");
            }

            CFRelease(newlevel);

            BatteryTimeRemaining_notify_post(kIOPSNotifyLowBattery);
            if (newWarningLevel != prevLoggedLevel) {
                /* Apple also logs it with logASLLowBatteryWarning(). */
                INFO_LOG("Warning level: %d cap: %d time: %d\n", newWarningLevel, percent, combinedTime);
                prevLoggedLevel = newWarningLevel;
            }
        }
        control.systemWarningLevel = newWarningLevel;
    }

    return;
}

static CFDictionaryRef getActiveBatteryDictionary_sync(void)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    for (int i=0; i<kPSMaxCount; i++) {
        if (!gPSList[i].description) {
            continue;
        }

        CFStringRef transport_type = NULL;
        transport_type = CFDictionaryGetValue(gPSList[i].description,
                                              CFSTR(kIOPSTransportTypeKey));
        if (isA_CFString(transport_type)
            && ( CFEqual(transport_type, CFSTR(kIOPSInternalType)))) {
            return gPSList[i].description;
        }
    }
    return NULL;
}

__private_extern__ CFDictionaryRef getActiveBatteryDictionary(void)
{
    __block CFDictionaryRef battery;
    dispatch_sync(batteryTimeRemainingQ, ^() {
        battery = getActiveBatteryDictionary_sync();
        if (battery) {
            CFRetain(battery);
        }
    });

    return battery;
}

static bool publish_IOPSGetTimeRemainingEstimate(
    int timeRemaining,
    bool external,
    bool rawExternal __unused,
    bool timeRemainingUnknown,
    bool isCharging,
    bool showChargingUI __unused,
    bool playChargingChime __unused,
    bool noPoll __unused,
    bool adapterUnsupported)
{
    uint64_t            powerSourcesBitsForNotify = (uint64_t)(timeRemaining & 0xFFFF);
    static uint64_t     lastPSBitsNotify = 0;
    bool                posted = false;
    uint32_t            rc;

    _internal_dispatch_assert_queue(batteryTimeRemainingQ);

    // Presence of bit kPSTimeRemainingNotifyValidBit means IOPSGetTimeRemainingEstimate
    // should trust this as a valid chunk of battery data.
    powerSourcesBitsForNotify |= kPSTimeRemainingNotifyValidBit;

    if (external) {
        powerSourcesBitsForNotify |= kPSTimeRemainingNotifyExternalBit;
    }
    if (timeRemainingUnknown) {
        powerSourcesBitsForNotify |= kPSTimeRemainingNotifyUnknownBit;
    }
    if (isCharging) {
        powerSourcesBitsForNotify |= kPSTimeRemainingNotifyChargingBit;
    }
    if (control.noPoll) {
        powerSourcesBitsForNotify |= kPSTimeRemainingNotifyNoPollBit;
    }
    if (adapterUnsupported) {
        powerSourcesBitsForNotify |= kPSTimeRemainingNotifyAdapterUnsupported;
    }

    /* These bits feed the SPI IOKit:IOPSGetSupportedPowerSources()
     *      - battery supported, UPS supported, active power sourecs
     */
    if (getActiveBatteryDictionary_sync()) {
        powerSourcesBitsForNotify |= kPSTimeRemainingNotifyBattSupportBit;
    }
    if (getActiveUPSDictionary_sync()) {
        powerSourcesBitsForNotify |= kPSTimeRemainingNotifyUPSSupportBit;
    }
    uint64_t activePS = getActivePSType_sync();
    powerSourcesBitsForNotify |=
    (activePS & 0xFF) << kPSTimeRemainingNotifyActivePS8BitsStarts;

    if (lastPSBitsNotify != powerSourcesBitsForNotify) {
        lastPSBitsNotify = powerSourcesBitsForNotify;
        notify_set_state(control.psTimeRemainingNotifyToken, powerSourcesBitsForNotify);
        rc = notify_post(kIOPSNotifyTimeRemaining);
        if (rc != NOTIFY_STATUS_OK) {
            ERROR_LOG("Failed to post notification for time remaining. rc:0x%x\n", rc);
        }
        else {
            INFO_LOG("Battery time remaining posted(0x%llx) Time:%d Source:%{public}s\n",
                    (unsigned long long)powerSourcesBitsForNotify, timeRemaining, external ? "AC":"Batt");
        }
        posted = true;
    }

    return posted;
}

static void batteryTimeRemaining_setPercentRemaining(int val)
{
    currentPercentRemaining = val;
}

__private_extern__ int batteryTimeRemaining_getPercentRemaining(void)
{
    return currentPercentRemaining;
}

static void publish_IOPSGetPercentRemaining(
    int         percentRemaining,
    bool        isExternal,
    bool        isCharging,
    bool        fullyCharged,
    IOPMBattery *b)
{
    uint64_t            currentStateBits, changedStateBits;
    static uint64_t     lastStateBits = 0;
    uint64_t            ignoreBits;

    _internal_dispatch_assert_queue(batteryTimeRemainingQ);

    // Presence of bit kPSTimeRemainingNotifyValidBit means IOPSGetPercentRemaining
    // should trust this as a valid chunk of battery data.
    currentStateBits = kPSTimeRemainingNotifyValidBit;

    if ((percentRemaining >= 0) && (percentRemaining <= 100))
        currentStateBits |= percentRemaining;
    if (isExternal)
        currentStateBits |= kPSTimeRemainingNotifyExternalBit;
    if (isCharging)
        currentStateBits |= kPSTimeRemainingNotifyChargingBit;
    if (fullyCharged)
        currentStateBits |= kPSTimeRemainingNotifyFullyChargedBit;
    if (b && b->isCritical)
        currentStateBits |= kPSCriticalLevelBit;

    changedStateBits = lastStateBits ^ currentStateBits;
    if (changedStateBits)
    {
        lastStateBits = currentStateBits;
        notify_set_state(control.psPercentChangeNotifyToken, currentStateBits);

        // Suppress notification for charging state changes
        ignoreBits = (kPSTimeRemainingNotifyChargingBit
                                 |kPSTimeRemainingNotifyFullyChargedBit
                                 |kPSCriticalLevelBit
                                 );
        if (changedStateBits & ~ignoreBits)
        {
            notify_post(kIOPSNotifyPercentChange);
            INFO_LOG("Battery capacity change posted(0x%llx). Capacity:%d Source:%{public}s\n",
                    (unsigned long long)currentStateBits, percentRemaining, isExternal ? "AC":"Batt");
        }
        if (changedStateBits & kPSCriticalLevelBit)
            BatteryTimeRemaining_notify_post(kIOPSNotifyCriticalLevel);
    }
}

__private_extern__ void kernelPowerSourcesDidChange(IOPMBattery *b)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    static int  _lastExternalConnected = -1;
    static int  _lastRawExternalConnected = -1;
    static int  _lastPercentRemaining = 100;
    static int  _lastIsCharging = -1;
    int         _nowExternalConnected = 0;
    int         _nowRawExternalConnected = 0;
    int         percentRemaining = 0;
    IOPMBattery **_batts = _batteries();

    /*
     * Initiate the next battery poll; or start a timer to poll
     * when the 60sec user visible polling timer expres.
     */
    startBatteryPoll(kPeriodicPoll);

    if (0 == _batteryCountSync()) {
        return;
    }

    if (!b) {
        b = _batts[0];
    }
    if ( !b || (b->properties == NULL)) {
        INFO_LOG("No batteries found yet..\n");
        return;
    }

    _nowExternalConnected = b->externalConnected;
    _nowRawExternalConnected = b->rawExternalConnected;

    if (_lastExternalConnected != _nowExternalConnected) {
        control.needsNotifyAC = true;
    }

    if (_lastRawExternalConnected != _nowRawExternalConnected) {
        // If AC has changed, we must invalidate time remaining.
        _discontinuityOccurred(false, true);
    } else if (_lastIsCharging != b->isCharging) {
        _discontinuityOccurred(false, false);
    }
    /* NextBSD: the estimate is held back while the discontinuity settles. */
    if (CFAbsoluteTimeGetCurrent() < (control.lastDiscontinuity + b->invalidWakeSecs)) {
        b->swCalculatedTR = -1;
    }

    readAndPublishACAdapter(b->externalConnected,
                             CFDictionaryGetValue(b->properties, CFSTR(kIOPMPSAdapterDetailsKey)));


    checkTimeRemainingValid(_batts);

    if (b->maxCap) {
        double percent = (double)(b->currentCap * 100) / (double)b->maxCap;
        percentRemaining = (int) lround(percent);
        if (percentRemaining > 100)
            percentRemaining = 100;
    }

    // never show 0 to the user
    if (percentRemaining < 1) {
        percentRemaining = 1;
    }

    // prevent percentage to increase when discharging
    if (!control.percentageDiscontinuity && !_lastRawExternalConnected &&
        !_nowRawExternalConnected && (percentRemaining > _lastPercentRemaining)) {
        percentRemaining = _lastPercentRemaining;
    } else if (CFAbsoluteTimeGetCurrent() >= (control.lastDiscontinuity + kDiscontinuitySettle)) {
        control.percentageDiscontinuity = false;
    }

    // b->swCalculatedPR is used by packageKernelPowerSource()
    b->swCalculatedPR = percentRemaining;
    _lastPercentRemaining = percentRemaining;
    _lastExternalConnected = _nowExternalConnected;
    _lastRawExternalConnected = _nowRawExternalConnected;
    _lastIsCharging = b->isCharging;

    /************************************************************************
     *
     * PUBLISH: SCDynamicStoreSetValue / IOPSCopyPowerSourcesInfo()
     *
     ************************************************************************/
    if (control.internal) {
        packageKernelPowerSource(b, control.internal);
    }

    HandlePublishAllPowerSources();
}

static void HandlePublishAllPowerSources(void)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    IOPMBattery               **batteries = _batteries();
    IOPMBattery                *b = NULL;
    int                         combinedTime = 0;
    int                         percentRemaining = 0;
    int                         prev_percentRemaining = batteryTimeRemaining_getPercentRemaining();
    bool                        tr_posted;
    int                         ups_externalConnected = 0;
    static int                  ups_prevExternalConnected = -1;
    bool                        externalConnected, tr_unknown, is_charging, fully_charged;
    bool                        rawExternalConnected = false;
    bool                        showChargingUI = false;
    bool                        playChargingChime = false;
    bool                        adapterUnsupported = false;
    CFDictionaryRef             ups = NULL;
    int                         ups_tr = -1;
    bool                        battcase_change = false;

    ups = getActiveUPSDictionary_sync();

    /*
     * Apple also requires the ASBM's FullPathUpdated timestamp here; the
     * kernel's entries have none.
     */
    if (_batteryCountSync() && (batteries[0]->isPresent)) {
        b = batteries[0];
    }

    /*
     * Apple returns here when there is neither a battery nor a UPS: a Mac
     * desktop publishes nothing. NextBSD publishes the AC-only state
     * instead (valid + external, provided by AC, 100%, nothing charging),
     * so every client gets a definite answer; see the header comment.
     */
    is_charging = fully_charged = false;
    for (int i = 0; i < _batteryCountSync(); i++) {
        if (batteries[i]->isPresent) {
            combinedTime += batteries[i]->swCalculatedTR;
        }
    }


    if (ups) {
        CFNumberRef num_cf = CFDictionaryGetValue(ups, CFSTR(kIOPSTimeToEmptyKey));
        if (num_cf) {
            CFNumberGetValue(num_cf, kCFNumberIntType, &ups_tr);
            if (ups_tr != -1) combinedTime += ups_tr;
        }

        CFStringRef src = CFDictionaryGetValue(ups, CFSTR(kIOPSPowerSourceStateKey));
        if (src && (CFStringCompare(src, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo)) {
            ups_externalConnected = 1;
        }
        if (ups_prevExternalConnected != ups_externalConnected) {
            control.needsNotifyAC = true;
            ups_prevExternalConnected = ups_externalConnected;
        }
    }

    if (b) {
            tr_unknown = b->isTimeRemainingUnknown;
            is_charging = b->isCharging;
            percentRemaining = b->swCalculatedPR;
            fully_charged = isFullyCharged(b);

            if (ups) {
                externalConnected = b->externalConnected && ups_externalConnected;
            }
            else {
                externalConnected = b->externalConnected;
            }
            rawExternalConnected = b->rawExternalConnected;
            showChargingUI = b->showChargingUI;
            playChargingChime = b->playChargingChime;
            adapterUnsupported = b->adapterUnsupported;
    }
    else if (ups) {
        int mcap = 0, ccap = 0;
        CFNumberRef mcap_cf = NULL, ccap_cf = NULL;

        /* ups must be non-NULL */
        externalConnected = ups_externalConnected;

        if (!externalConnected && (ups_tr == -1)) {
            tr_unknown = false;
        }
        else {
            tr_unknown = true;
        }

        if (CFDictionaryGetValue(ups, CFSTR(kIOPSIsChargingKey)) == kCFBooleanTrue)
            is_charging = true;

        ccap_cf = CFDictionaryGetValue(ups, CFSTR(kIOPSCurrentCapacityKey));
        if (ccap_cf)
            CFNumberGetValue(ccap_cf, kCFNumberIntType, &ccap);

        mcap_cf = CFDictionaryGetValue(ups, CFSTR(kIOPSMaxCapacityKey));
        if (mcap_cf)
            CFNumberGetValue(mcap_cf, kCFNumberIntType, &mcap);

        if (ccap && mcap)
            percentRemaining = (ccap*100)/mcap;

        if ((percentRemaining >= 95) && externalConnected && (!is_charging))
            fully_charged = true;
    }
    else {
        /* NextBSD: AC only. */
        externalConnected = true;
        rawExternalConnected = true;
        tr_unknown = false;
        combinedTime = 0;
        percentRemaining = 100;
        fully_charged = true;
    }

    tr_posted = publish_IOPSGetTimeRemainingEstimate(combinedTime,
                                         externalConnected,
                                         rawExternalConnected,
                                         tr_unknown,
                                         is_charging,
                                         showChargingUI,
                                         playChargingChime,
                                         control.noPoll,
                                         adapterUnsupported);

    if (b) {
        publish_IOPSBatteryGetWarningLevel(b, combinedTime, percentRemaining);
    }

    publish_IOPSGetPercentRemaining(percentRemaining,
                                    externalConnected,
                                    is_charging,
                                    fully_charged,
                                    b);

    if (((percentRemaining != prev_percentRemaining) || battcase_change) && !tr_posted) {
        BatteryTimeRemaining_notify_post(kIOPSNotifyTimeRemaining);
    }

    batteryTimeRemaining_setPercentRemaining(percentRemaining);

    /************************************************************************
     *
     * TELL: powerd-internal code that responds to power changes
     ************************************************************************/

     // Notifiy PSLowPower of power sources change
    /* Apple: UPSLowPowerPSChange(); UPSLowPower.c is not built yet. */
    PMSettingsPSChange();

    /************************************************************************
     *
     * NOTIFY: Providing power source changed.
     *          via notify(3)
     ************************************************************************/
    if (control.needsNotifyAC) {
        control.needsNotifyAC = false;

        INFO_LOG("Power Source change. Source:%{public}s", externalConnected ? "AC" : "Batt");
        notify_post(kIOPSNotifyPowerSource);
    }

    notify_post(kIOPSNotifyAnyPowerSource);

    return;
}


/* checkTimeRemainingValid
 * Implicit inputs: battery state; battery's own time remaining estimate
 * Implicit output: estimated time remaining placed in b->swCalculatedTR; or -1 if indeterminate
 *   returns 1 if we reached a valid estimate
 *   returns 0 if we're still calculating
 */
static void checkTimeRemainingValid(IOPMBattery **batts)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    int             i;
    IOPMBattery     *b;
    int             batCount = _batteryCountSync();

    for (i = 0; i < batCount; i++) {
        b = batts[i];
        // Did our calculation come out negative?
        // The average current must still be out of whack!
        if ((b->swCalculatedTR < 0) || (false == b->isPresent)) {
            b->swCalculatedTR = -1;
        }

        // Cap all times remaining to 10 hours. We don't ship any
        // 44 hour batteries just yet.
        if (kMaxBattMinutes < b->swCalculatedTR) {
            b->swCalculatedTR = kMaxBattMinutes;
        }
    }

    if (-1 == batts[0]->swCalculatedTR) {
        batts[0]->isTimeRemainingUnknown = true;
    } else {
        batts[0]->isTimeRemainingUnknown = false;
    }

}

bool isFullyCharged(IOPMBattery *b)
{
    bool is_charged = false;

    if (!b || !b->maxCap) {
        return false;
    }

    unsigned int soc = 100 * b->currentCap / b->maxCap;

    // Set IsCharged if capacity >= 95%
    // - Some portables will not initiate a battery charge if AC is
    //   connected when copacity is >= 95%.
    // - We consider > 95% to be fully charged; the battery will not charge
    //   any higher until AC is unplugged and re-attached.
    // - IsCharged should be true when the external power adapter LED is Green;
    //   should be false when the external power adapter LED is Orange.

    if (b->isPresent) {
        is_charged = soc >= 95;
    }

    return is_charged;
}




/*
 * Implicit argument: All the global variables that track battery state
 */
static void packageKernelPowerSource(IOPMBattery *b, PSStruct *ps)
{
    CFNumberRef     n, n0;
    CFMutableDictionaryRef  mDict = NULL;
    int             temp;
    int             minutes;
    int             set_capacity, set_charge;
    int             psID;

    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    if (!b) {
        IOPMBattery **batts = _batteries();
        b = batts[0];
    }

    // Create the battery info dictionary
    mDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if(!mDict) {
        return;
    }


    // Does the battery provide its own time remaining estimate?
    CFDictionarySetValue(mDict, CFSTR("Battery Provides Time Remaining"), kCFBooleanTrue);

    // Was there an error/failure? Set that.
    if (b->failureDetected) {
        CFDictionarySetValue(mDict, CFSTR(kIOPSFailureKey), b->failureDetected);
    }

    // Is there a charging problem?
    if (b->chargeStatus) {
        CFDictionarySetValue(mDict, CFSTR(kIOPMPSBatteryChargeStatusKey), b->chargeStatus);
    }

    // Type = "InternalBattery", and "Transport Type" = "Internal"
    CFDictionarySetValue(mDict, CFSTR(kIOPSTransportTypeKey), CFSTR(kIOPSInternalType));
    CFDictionarySetValue(mDict, CFSTR(kIOPSTypeKey), CFSTR(kIOPSInternalBatteryType));

    // Set Power Source State to AC/Battery
    CFDictionarySetValue(mDict, CFSTR(kIOPSPowerSourceStateKey),
                            (b->externalConnected ? CFSTR(kIOPSACPowerValue):CFSTR(kIOPSBatteryPowerValue)));

    // Battery provided serial number
    if (b->batterySerialNumber) {
        CFDictionarySetValue(mDict, CFSTR(kIOPSHardwareSerialNumberKey), b->batterySerialNumber);
    }
    // Set Amperage
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &b->avgAmperage);
    if(n) {
        CFDictionarySetValue(mDict, CFSTR(kIOPSCurrentKey), n);
        CFRelease(n);
    }

    // round charge and capacity down to a % scale
    if(0 != b->maxCap)
    {
        set_capacity = 100;
        set_charge = b->swCalculatedPR;

        if( (100 == set_charge) && b->isCharging)
        {
            // We will artificially cap the percentage to 99% while charging
            // Batteries may take 10-20 min beyond 100% of charging to
            // relearn their absolute maximum capacity. Leave cap at 99%
            // to indicate we're not done charging. (4482296, 3285870)
            set_charge = 99;
        }
    } else {
        // Bad battery or bad reading => 0 capacity
        set_capacity = set_charge = 0;
    }

    // Set maximum capacity
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &set_capacity);
    if(n) {
        CFDictionarySetValue(mDict, CFSTR(kIOPSMaxCapacityKey), n);
        CFRelease(n);
    }

    // Set current charge
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &set_charge);
    if(n) {
        CFDictionarySetValue(mDict, CFSTR(kIOPSCurrentCapacityKey), n);
        CFRelease(n);
    }

    // Set isPresent flag
    CFDictionarySetValue(mDict, CFSTR(kIOPSIsPresentKey),
                b->isPresent ? kCFBooleanTrue:kCFBooleanFalse);

    minutes = b->swCalculatedTR;

    temp = 0;
    n0 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &temp);

    if( !b->isPresent ) {
        // remaining time calculations only have meaning if the battery is present
        CFDictionarySetValue(mDict, CFSTR(kIOPSIsChargingKey), kCFBooleanFalse);
        CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToFullChargeKey), n0);
        CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToEmptyKey), n0);
    } else {
        // There IS a battery installed.
        if(b->isCharging) {
            // Set _isCharging to True
            CFDictionarySetValue(mDict, CFSTR(kIOPSIsChargingKey), kCFBooleanTrue);
            // Set IsFinishingCharge
            CFDictionarySetValue(mDict, CFSTR(kIOPSIsFinishingChargeKey),
                    (b->maxCap && (99 <= (100*b->currentCap/b->maxCap))) ? kCFBooleanTrue:kCFBooleanFalse);
            n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &minutes);
            if(n) {
                CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToFullChargeKey), n);
                CFRelease(n);
            }
            CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToEmptyKey), n0);
        } else {
            // Not Charging
            // Set _isCharging to False
            CFDictionarySetValue(mDict, CFSTR(kIOPSIsChargingKey), kCFBooleanFalse);
            // But are we plugged in?
            if(b->externalConnected)
            {
                // plugged in but not charging == fully charged
                CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToFullChargeKey), n0);
                CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToEmptyKey), n0);

                CFDictionarySetValue(mDict, CFSTR(kIOPSIsChargedKey),
                    isFullyCharged(b) ? kCFBooleanTrue:kCFBooleanFalse);
            } else {
                // not charging, not plugged in == d_isCharging
                n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &minutes);
                if(n) {
                    CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToEmptyKey), n);
                    CFRelease(n);
                }
                CFDictionarySetValue(mDict, CFSTR(kIOPSTimeToFullChargeKey), n0);
            }
        }
    }
    if (n0) {
        CFRelease(n0);
    }

    /* Apple: _setBatteryHealthData(mDict, b); battery health is not ported. */

    // Set name
    if(b->name) {
        CFDictionarySetValue(mDict, CFSTR(kIOPSNameKey), b->name);
    } else {
        CFDictionarySetValue(mDict, CFSTR(kIOPSNameKey), CFSTR("Unnamed"));
    }

    // Set ID (UPS psID gets set by upsd)
    if (ps->psType != kPSTypeUPS) {
        psID = MAKE_UNIQ_SOURCE_ID(ps->pid, ps->psid);

        n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &psID);
        if (n) {
            CFDictionarySetValue(mDict, CFSTR(kIOPSPowerSourceIDKey), n);
            CFRelease(n);
        }
    }

    if (ps->description) {
        CFRelease(ps->description);
    }
    ps->description = mDict;
}

/***********************************************************************************/
/***********************************************************************************/
/***********************************************************************************/
/**** User-space power source code lives below here ********************************/
/***********************************************************************************/
/***********************************************************************************/
/***********************************************************************************/

static PSStruct *iops_newps(int pid, int psid)
{
    _internal_dispatch_assert_queue_barrier(batteryTimeRemainingQ);

    // Find the first empty slot in gPSList
    int i = kPSMaxCount;
    if (psid == kSpecialInternalBatteryID) {
        // Reserve 0 for internal battery
        i = 0;
    }
    else {
        for (i=1; i<kPSMaxCount; i++) {
            if (0 == gPSList[i].psid) {
                break;
            }
        }
    }
    if (i < kPSMaxCount) {
        bzero((void *)&gPSList[i], sizeof(PSStruct));
        gPSList[i].pid = pid;
        gPSList[i].psid = psid;
        return &gPSList[i];
    }

    return NULL;
}

static PSStruct *iopsFromPSID(int _pid, int _psid)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    for (int i=0; i<kPSMaxCount; i++) {
        if (gPSList[i].psid == _psid && gPSList[i].pid == _pid) {
            return &gPSList[i];
        }
    }

    return NULL;
}

static CFDictionaryRef getPSByType(CFStringRef type)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    for (int i=0; i<kPSMaxCount; i++) {
        if (!isA_CFDictionary(gPSList[i].description)) {
            continue;
        }

        CFStringRef ps_type = CFDictionaryGetValue(gPSList[i].description, CFSTR(kIOPSTypeKey));
        if (isA_CFString(ps_type) && CFEqual(ps_type, type)) {
            return gPSList[i].description;
        }
    }
    return NULL;
}

static CFDictionaryRef getActiveUPSDictionary_sync(void)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    return getPSByType(CFSTR(kIOPSUPSType));
}

// Returns active UPS dictionary or NULL.
// Caller must release the returned object.
__private_extern__ CFDictionaryRef getActiveUPSDictionary(void)
{
    __block CFDictionaryRef ups;
    dispatch_sync(batteryTimeRemainingQ, ^() {
        ups = getActiveUPSDictionary_sync();
        if (ups) {
            CFRetain(ups);
        }
    });

    return ups;
}

static int getActivePSType_sync(void)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    int rc = kIOPSProvidedByAC;
    CFDictionaryRef activeBattery = getActiveBatteryDictionary_sync();
    CFDictionaryRef activeUPS = getActiveUPSDictionary_sync();
    CFStringRef     ps_state = NULL;

    /* if (!activeBattery) is testing for whether batteries are supported on
     * this system at all, e.g. mobile vs desktop. */
    if (!activeBattery) {
        if(!activeUPS) {
            // no batteries, no UPS -> AC Power
            rc = kIOPSProvidedByAC;
        } else {
            ps_state = CFDictionaryGetValue(activeUPS,
                                            CFSTR(kIOPSPowerSourceStateKey));
            if (ps_state && CFEqual(ps_state, CFSTR(kIOPSACPowerValue))) {
                // no batteries, yes UPS, UPS is running off of AC power -> AC Power
                rc = kIOPSProvidedByAC;
            } else if(ps_state && CFEqual(ps_state, CFSTR(kIOPSBatteryPowerValue))) {
                // no batteries, yes UPS, UPS is running drawing its Battery power -> UPS Power
                rc = kIOPSProvidedByExternalBattery;
            }
        }
    } else {
        ps_state = CFDictionaryGetValue(activeBattery,
                                        CFSTR(kIOPSPowerSourceStateKey));
        if (ps_state && CFEqual(ps_state, CFSTR(kIOPSBatteryPowerValue))) {
            // Yes batteries, yes running on battery power -> Battery power
            rc = kIOPSProvidedByBattery;
        } else {
            // batteries are on AC power. let's check if UPS is present.
            if (!activeUPS) {
                // yes batteries on AC power, no UPS present -> AC Power
                rc = kIOPSProvidedByAC;
            } else {
                ps_state = CFDictionaryGetValue(activeUPS, CFSTR(kIOPSPowerSourceStateKey));
                if (ps_state && CFEqual(ps_state, CFSTR(kIOPSBatteryPowerValue))) {
                    // yes batteries on AC power, UPS is on its battery -> UPS Power
                    rc = kIOPSProvidedByExternalBattery;
                } else if (ps_state && CFEqual(ps_state, CFSTR(kIOPSACPowerValue))) {
                    // yes batteries on AC Power, UPS is drawing AC Power -> AC Power
                    rc = kIOPSProvidedByAC;
                }
            }
        }
    }

    return rc;
}

__private_extern__ int getActivePSType(void)
{
    __block int rc = kIOPSProvidedByAC;
    dispatch_sync(batteryTimeRemainingQ, ^() {
        rc = getActivePSType_sync();
    });

    return rc;
}

static void dump_power_sources(void)
{
    for (size_t i = 0; i < kPSMaxCount; i++) {
        PSStruct *ps = &gPSList[i];

        ERROR_LOG("id:%d pid:%d type:%d desc:%@\n", ps->psid, ps->pid, ps->psType, ps->description);
    }
}

/***********************************************************************************/
// MIG handler - back end for IOKit API IOPSCreatePowerSource
#define PSID_MIN    5000
kern_return_t _io_ps_new_pspowersource(
    mach_port_t                 server __unused,
    audit_token_t               token,
    int                         *psid,              // out
    int                         *result)
{
    static int                  gPSID = PSID_MIN;
    int                         callerPID;
    __block PSStruct            *ps;

    callerPID = _auditTokenPID(token);

    *result = kIOReturnError;

    dispatch_barrier_sync(batteryTimeRemainingQ, ^() {
        if (gPSID == INT_MAX) {
            gPSID = PSID_MIN;
        }

        ps = iops_newps(callerPID, gPSID);
        if (!ps) {
            *result = kIOReturnNoSpace;
            dump_power_sources();
            return;
        }

        *psid = gPSID++;

        ps->procdeathsrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
                                                    callerPID,
                                                    DISPATCH_PROC_EXIT,
                                                    batteryTimeRemainingQ);

        /* Setup automatic cleanup if client process dies */
        dispatch_source_set_cancel_handler(ps->procdeathsrc, ^{
            /*
             * When the client process dies, remove
             * this power source and stop showing it to IOPS API clients.
             */

            if (ps->psType == kPSTypeAccessory) {
                BatteryTimeRemaining_notify_post(kIOPSAccNotifyTimeRemaining);
                BatteryTimeRemaining_notify_post(kIOPSAccNotifyAttach);
            }
            else {
                BatteryTimeRemaining_notify_post(kIOPSNotifyTimeRemaining);
                BatteryTimeRemaining_notify_post(kIOPSNotifyAttach);
            }
            INFO_LOG("Posted notifications for loss of power source id %d\n", ps->psid);
            dispatch_release(ps->procdeathsrc);
            ps->procdeathsrc = NULL;
            if (ps->description) {
                CFRelease(ps->description);
            }
            bzero((void *)ps, sizeof(PSStruct));

            HandlePublishAllPowerSources();
        });

        dispatch_source_set_event_handler(ps->procdeathsrc, ^{
            dispatch_source_cancel(ps->procdeathsrc);
        });

        dispatch_resume(ps->procdeathsrc);

        *result = kIOReturnSuccess;
        INFO_LOG("Created new power source id %d for pid %d\n", *psid, callerPID);
    });

    return KERN_SUCCESS;
}

/***********************************************************************************/
// MIG handler - back end for IOKit API IOPSSetPowerSourceDetails

// Log lines are limited in size. Avoid truncation by removing the
// kIOPSDebugInformationKey subdictionary from the standard logging and log it
// separately in debug level logs.
static void log_ps_dictionary(int psid, int callerPID, CFDictionaryRef details)
{
    CFMutableDictionaryRef logDict = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, details);

    if (!logDict) {
        return;
    }
    CFDictionaryRemoveValue(logDict, CFSTR(kIOPSDebugInformationKey));

    INFO_LOG("Received power source(psid:%d) update from pid %d: %@\n", psid, callerPID, logDict);
    CFRelease(logDict);
}

/*
 * NextBSD: IOKitUser's client flattens the details with IOCFSerialize(),
 * which NextBSD's libIOKit does not have; Apple parses them back with
 * IOCFUnserialize(). Here they are a CFPropertyList (XML or binary), which
 * is what IOPSSetPowerSourceDetails() in libIOKit sends.
 */
static CFMutableDictionaryRef
_copyPSDetails(vm_offset_t details_ptr, mach_msg_type_number_t details_len)
{
    CFDataRef           data;
    CFPropertyListRef   plist = NULL;

    if (!details_ptr || !details_len) {
        return NULL;
    }
    data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                (const UInt8 *)details_ptr, details_len, kCFAllocatorNull);
    if (data) {
        plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
                    kCFPropertyListMutableContainers, NULL, NULL);
        CFRelease(data);
    }
    return (CFMutableDictionaryRef)plist;
}

kern_return_t _io_ps_update_pspowersource(
    mach_port_t         server __unused,
    audit_token_t       token,
    int                 psid,
    vm_offset_t         details_ptr,
    mach_msg_type_number_t  details_len,
    int                 *return_code)
{
    dispatch_sync(batteryTimeRemainingQ, ^() {
        CFMutableDictionaryRef     details = NULL;
        int                 callerPID;
        CFStringRef         psTypeStr = NULL;
        CFNumberRef         psIDKey = NULL;
        int                 psID = 0;

        callerPID = _auditTokenPID(token);

        *return_code = kIOReturnError;

        details = _copyPSDetails(details_ptr, details_len);

        if (!isA_CFDictionary(details)) {
            *return_code = kIOReturnBadArgument;
            if (details) {
                CFRelease(details);
            }
            goto exit;
        }

        PSStruct *next = iopsFromPSID(callerPID, psid);
        if (!next) {
            ERROR_LOG("Failed to find the power source for psid 0x%x from pid %d\n", psid, callerPID);
            *return_code = kIOReturnNotFound;
            CFRelease(details);
            goto exit;
        }

        psIDKey = CFDictionaryGetValue(details, CFSTR(kIOPSPowerSourceIDKey));
        if (!isA_CFNumber(psIDKey)) {
            psID = MAKE_UNIQ_SOURCE_ID(next->pid, next->psid);

            psIDKey = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &psID);
            if (psIDKey) {
                CFDictionarySetValue(details, CFSTR(kIOPSPowerSourceIDKey), psIDKey);
                CFRelease(psIDKey);
            }
        }

        if (next->psType == kPSTypeUnknown) {
            psTypeStr = CFDictionaryGetValue(details, CFSTR(kIOPSTypeKey));
            if (isA_CFString(psTypeStr)) {
                if (CFStringCompare(psTypeStr, CFSTR(kIOPSAccessoryType), 0) == kCFCompareEqualTo)
                    next->psType = kPSTypeAccessory;
                else if ((CFStringCompare(psTypeStr, CFSTR(kIOPSUPSType), 0) == kCFCompareEqualTo)
                        )
                    next->psType = kPSTypeUPS;
                else if (CFStringCompare(psTypeStr, CFSTR(kIOPSInternalBatteryType), 0) == kCFCompareEqualTo)
                    next->psType = kPSTypeIntBattery;
            }
        }

        log_ps_dictionary(psid, callerPID, details);
        if ((next->psType == kPSTypeIntBattery) || (next->psType == kPSTypeUPS)) {
            if (next->description) {
                CFRelease(next->description);
            }
            else {
                // This is the first update for this source
                BatteryTimeRemaining_notify_post(kIOPSNotifyAttach);
                INFO_LOG("Posted \"%s\" for new power source id %d\n", kIOPSNotifyAttach, psid);
            }
            next->description = details;
            dispatch_async(batteryTimeRemainingQ, ^() {
                HandlePublishAllPowerSources();
            });
            *return_code = kIOReturnSuccess;
        }
        else if (next->psType == kPSTypeAccessory) {
           *return_code = HandleAccessoryPowerSources(next, details);
        } else {
            CFRelease(details);
        }

exit:
        if (details_ptr && details_len) {
            vm_deallocate(mach_task_self(), details_ptr, details_len);
        }
    });

    return 0;
}

kern_return_t _io_ps_release_pspowersource(
    mach_port_t         server __unused,
    audit_token_t       token,
    int                 psid)
{
    int                         callerPID;
    callerPID = _auditTokenPID(token);

    dispatch_async(batteryTimeRemainingQ, ^() {
        INFO_LOG("Releasing power source id = %d\n", psid);

        PSStruct *toRelease = iopsFromPSID(callerPID, psid);
        if (toRelease && toRelease->procdeathsrc) {
            dispatch_source_cancel(toRelease->procdeathsrc);
        }
    });

    return 0;
}

static CFArrayRef copy_powersources_info(audit_token_t token __unused, int type, bool preciseInfo __unused)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);

    CFMutableArrayRef return_value = NULL;
    for (int i=0; i<kPSMaxCount; i++) {
        if (gPSList[i].description == NULL) {
            continue;
        }

        switch(type) {
        case kIOPSSourceInternal:
            if (gPSList[i].psType != kPSTypeIntBattery)
                continue;
            break;

        case kIOPSSourceUPS:
            if (gPSList[i].psType != kPSTypeUPS)
                continue;
            break;

        case kIOPSSourceInternalAndUPS:
            if ((gPSList[i].psType != kPSTypeIntBattery) && (gPSList[i].psType != kPSTypeUPS))
                continue;
            break;

        case kIOPSSourceForAccessories:
            if (gPSList[i].psType != kPSTypeAccessory)
                continue;
            break;

        case kIOPSSourceAll:
            break;

        default:
            continue;
        }

        if (!return_value) {
            return_value = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
            if (!return_value) {
                return NULL;
            }
        }

        /*
         * Apple adds the precise (unrounded) battery data for entitled
         * callers, rounds the capacity to 5% for the rest when the
         * ImprecisePowerData feature is on, and merges the battery health
         * keys (updateBatteryHealthData()). None of that is ported; every
         * caller gets the description as published.
         */
        CFArrayAppendValue(return_value, gPSList[i].description);
    }

    return return_value;
}


kern_return_t _io_ps_copy_powersources_info(
    mach_port_t             server __unused,
    audit_token_t           token,
    int                     type,
    vm_offset_t             *ps_ptr,
    mach_msg_type_number_t  *ps_len,
    int                     *return_code)
{
    dispatch_sync(batteryTimeRemainingQ, ^() {
        CFArrayRef return_value = copy_powersources_info(token, type, false);

        *ps_ptr = 0;
        *ps_len = 0;

        /*
         * NextBSD: no source at all (a machine on AC with no battery or
         * UPS) is an empty list, not an empty reply; IOKit's client turns
         * either into an empty array, but this way the answer is explicit.
         */
        if (!return_value) {
            return_value = CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
        }
        if (return_value) {
            CFDataRef d = CFPropertyListCreateData(0, return_value,
                                                     kCFPropertyListBinaryFormat_v1_0,
                                                     0, NULL);
            if (d) {
                vm_address_t addr = 0;
                mach_msg_type_number_t len = (mach_msg_type_number_t)CFDataGetLength(d);

                if (len && vm_allocate(mach_task_self(), &addr, len, TRUE) == KERN_SUCCESS) {
                    memcpy((void *)addr, CFDataGetBytePtr(d), len);
                    *ps_ptr = (vm_offset_t)addr;
                    *ps_len = len;
                }
                CFRelease(d);
            }
            CFRelease(return_value);
        }
        *return_code = kIOReturnSuccess;
    });

    return 0;
}


static IOReturn HandleAccessoryPowerSources(PSStruct *ps, CFDictionaryRef update)
{
    CFNumberRef     n = NULL;
    int  old_cap = 0, new_cap = 0;
    CFStringRef old_src = NULL, new_src = NULL;

    /* update dictionary is validated by the caller */

    new_src = CFDictionaryGetValue(update, CFSTR(kIOPSPowerSourceStateKey));
    n = CFDictionaryGetValue(update, CFSTR(kIOPSCurrentCapacityKey));
    if (n) {
        CFNumberGetValue(n, kCFNumberIntType, &new_cap);
    }

    if (!new_src || !n) {
        ERROR_LOG("PS update is missing SourceState or Capacity\n");
        CFRelease(update);
        return kIOReturnBadArgument;
    }

    if (ps->description != NULL) {
        bool do_notify_ps = false;
        bool do_notify_tr = false;

        old_src = CFDictionaryGetValue(ps->description, CFSTR(kIOPSPowerSourceStateKey));
        if (old_src && CFStringCompare(new_src, old_src, 0) != kCFCompareEqualTo) {
            do_notify_ps = true;
        }

        n = CFDictionaryGetValue(ps->description, CFSTR(kIOPSCurrentCapacityKey));
        if (n) {
            CFNumberGetValue(n, kCFNumberIntType, &old_cap);
        }
        if (new_cap != old_cap) {
            do_notify_tr = true;
        }

        // notify if kIOPSIsChargingKey changes
        int old_is_charging = 0;
        int new_is_charging = 0;
        n = CFDictionaryGetValue(ps->description, CFSTR(kIOPSIsChargingKey));
        if (n) {
            CFNumberGetValue(n, kCFNumberIntType, &old_is_charging);
        }

        n = CFDictionaryGetValue(update, CFSTR(kIOPSIsChargingKey));
        if (n) {
            CFNumberGetValue(n, kCFNumberIntType, &new_is_charging);
        }

        if (old_is_charging != new_is_charging) {
            do_notify_ps = true;
        }


        if (do_notify_ps) {
            BatteryTimeRemaining_notify_post(kIOPSAccNotifyPowerSource);
            INFO_LOG("Posted \"%s\" for power source id %d\n", kIOPSAccNotifyPowerSource, ps->psid);
        }

        if (do_notify_tr) {
            BatteryTimeRemaining_notify_post(kIOPSAccNotifyTimeRemaining);
            INFO_LOG("Posted \"%s\" for power source id %d\n", kIOPSAccNotifyTimeRemaining, ps->psid);
        }

        CFRelease(ps->description);
    }
    else {
        /* This is a new accessory with power source */
        BatteryTimeRemaining_notify_post(kIOPSAccNotifyAttach);
        BatteryTimeRemaining_notify_post(kIOPSAccNotifyTimeRemaining);
        INFO_LOG("Posted notifications for new power source id %d\n", ps->psid);
    }

    ps->description = update;
    return kIOReturnSuccess;
}

static int _batteryCountSync(void)
{
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    /* NextBSD: the folded internal battery counts as one. */
    return gInternalBatteryValid ? 1 : 0;
}

__private_extern__ int _batteryCount(void)
{
    int __block ret;

    _internal_dispatch_assert_queue_not(batteryTimeRemainingQ);

    dispatch_sync(batteryTimeRemainingQ, ^() {
            ret = _batteryCountSync();
    });
    return ret;
}

static bool getPowerStateSync(PowerSources *source, uint32_t *percentage)
{
    IOPMBattery **batteries;
    int         batteryCount = 0;
    bool        ret = false;
    _internal_dispatch_assert_queue(batteryTimeRemainingQ);
    batteryCount = _batteryCountSync();
    if (0 < batteryCount) {
        int validBattCount = 0;
        uint32_t capPercent = 0;

        batteries = _batteries();
        for (int i = 0; i < batteryCount; i++) {
            if (batteries[i]->isPresent == false) {
                continue;
            }
            validBattCount++;
            if (0 != batteries[i]->maxCap) {
                capPercent += (batteries[i]->currentCap * 100) / batteries[i]->maxCap;
            }
        }
        if (validBattCount) {
            *source = batteries[0]->externalConnected ? kACPowered : kBatteryPowered;
            *percentage = capPercent;
            ret = true;
        }
    }

    return ret;
}

__private_extern__ bool getPowerState(PowerSources *source, uint32_t *percentage)
{
    bool __block ret = false;

    // at powerd startup this may get called before prime()
    if (!batteryTimeRemainingQ) {
        return false;
    }

    _internal_dispatch_assert_queue_not(batteryTimeRemainingQ);

    *source = kACPowered;

    dispatch_sync(batteryTimeRemainingQ, ^() {
        ret = getPowerStateSync(source, percentage);
    });

    return ret;
}

static PowerSources _getPowerSourceSync(void)
{
   IOPMBattery      **batteries;
   _internal_dispatch_assert_queue(batteryTimeRemainingQ);
   if (_batteryCountSync() && (batteries = _batteries()) && (batteries[0]->isPresent && !batteries[0]->externalConnected) )
      return kBatteryPowered;
   else
      return kACPowered;
}

__private_extern__ PowerSources _getPowerSource(void)
{
   PowerSources __block ret;
   _internal_dispatch_assert_queue_not(batteryTimeRemainingQ);



   dispatch_sync(batteryTimeRemainingQ, ^() {
       ret = _getPowerSourceSync();
   });

   return ret;
}
