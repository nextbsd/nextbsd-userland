/*
 * CPUPower_freebsd.c — the CPU power policy that follows the power source
 * (nextbsd-userland#231, E17 D7).
 *
 * On an Intel Mac pmconfigd does not drive the CPU's clock. PMSettings.m
 * picks the Energy Saver profile of the power source in use (AC Power,
 * Battery Power, UPS Power), tells the kernel the source changed
 * (IOPMSetAggressiveness kPMPowerSource) and sends it the profile's
 * settings; the platform plugin in the kernel (X86PlatformPlugin, with
 * xnu's XCPM) turns that into the processor's energy/performance policy,
 * and the hardware picks the frequency. There is no user-space polling
 * loop, and FreeBSD's powerd(8) is not part of the design.
 *
 * NextBSD's kernel has no platform plugin, but on Intel processors with
 * hardware P-states (HWP, Skylake and later) the hardware already picks the
 * frequency, and hwpstate_intel(4) exposes the one knob a platform plugin
 * sets: the energy/performance preference, dev.hwpstate_intel.N.epp, from
 * 0 (performance) to 100 (energy efficiency). This file is that part of the
 * platform plugin. PMSettings.m's ActivatePMSettings() hands it every
 * profile it activates (at start, on each power-source change, on a
 * settings change and on each "pmset force"/assertion override), and it
 * sets the preference of every CPU:
 *
 *   Low Power Mode on ("LowPowerMode" = 1)     100  energy efficiency
 *   high speed forced ("Dynamic Power Step" 0)   0  performance
 *   reduced speed ("Reduce Processor Speed" 1) 100  energy efficiency
 *   otherwise, on AC power                      50  Intel's balanced default
 *   otherwise, on battery or UPS power          75  balanced, towards power
 *
 * "Dynamic Power Step" = 0 and "Reduce Processor Speed" = 0 are what
 * PMSettings.m's kPMForceHighSpeed override writes while a CPUBoundAssertion
 * (kIOPMAssertionTypeNeedsCPU) is held; "Reduce Processor Speed" = 1 is its
 * kPMForceLowSpeedProfile override. Low Power Mode is a per-source Energy
 * Saver setting (pmset -b lowpowermode 1), offered only where this file can
 * act on it, the way a Mac's platform plugin publishes the "LowPowerMode"
 * feature (IOPMEnergyPrefs_freebsd.c asks cpuPowerFeatureIsAvailable()).
 *
 * Processors without HWP (Intel before Skylake through est(4), AMD through
 * hwpstate_amd(4)) have only discrete frequency levels, dev.cpu.0.freq.
 * xnu's XCPM steps them in the kernel; NextBSD's kernel does not, and
 * stepping them from here would be powerd's polling loop. So on those
 * machines this file acts on the explicit settings only: Low Power Mode and
 * reduced speed set the lowest level, and leaving them (or forcing high
 * speed) sets the highest. The source change alone leaves the frequency
 * where the firmware and the kernel put it.
 *
 * With neither control (the CI virtual machines, arm64 boards without a
 * cpufreq driver, desktops whose firmware hides HWP) nothing is offered,
 * nothing is written and nothing is logged.
 *
 * The kernel keeps the preference across sleep: hwpstate_intel(4)'s resume
 * method writes the saved request back to each CPU. Everything here runs on
 * pmconfigd's main queue, where PMSettings.m runs.
 */
#include "PrivateLib_freebsd.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dev.hwpstate_intel.N.epp, percent */
#define kCPUEPPPerformance          0
#define kCPUEPPBalancedAC           50      /* 0x80, Intel's default */
#define kCPUEPPBalancedBattery      75      /* 0xC0 */
#define kCPUEPPEfficiency           100

typedef enum {
    kCPUPolicyBalanced = 0,
    kCPUPolicyPerformance,
    kCPUPolicyEfficiency
} CPUPolicy;

typedef enum {
    kCPUControlUnknown = 0,
    kCPUControlNone,
    kCPUControlHWP,                         /* dev.hwpstate_intel.N.epp */
    kCPUControlFreqLevels                   /* dev.cpu.0.freq */
} CPUControl;

static CPUControl   gCPUControl = kCPUControlUnknown;   /* cpuPowerControl() */
static int          gCPUMaxID;              /* kern.smp.maxid */
static int          gFreqLowest;            /* MHz, dev.cpu.0.freq_levels */
static int          gFreqHighest;
static int          gAppliedEPP = -1;
static bool         gFreqLowered;

/*
 * The first and last levels of dev.cpu.0.freq_levels ("2601/15000
 * 2600/15000 ... 800/4000", highest first). Fewer than two levels means
 * there is nothing to choose between.
 */
static bool
copyFreqLevels(int *lowest, int *highest)
{
    char        *levels = NULL, *p, *end;
    size_t      len = 0;
    long        mhz;
    int         count = 0;
    bool        ok = false;

    if (sysctlbyname("dev.cpu.0.freq_levels", NULL, &len, NULL, 0) != 0 || len == 0)
        return false;
    levels = malloc(len + 1);
    if (!levels)
        return false;
    if (sysctlbyname("dev.cpu.0.freq_levels", levels, &len, NULL, 0) != 0)
        goto exit;
    levels[len] = '\0';

    for (p = levels; *p; ) {
        mhz = strtol(p, &end, 10);
        if (end == p)
            break;
        if (mhz > 0) {
            if (count == 0 || mhz > *highest)
                *highest = (int)mhz;
            if (count == 0 || mhz < *lowest)
                *lowest = (int)mhz;
            count++;
        }
        /* skip "/power" and the separating space */
        p = end + strcspn(end, " ");
        p += strspn(p, " ");
    }
    ok = (count > 1 && *lowest < *highest);

exit:
    free(levels);
    return ok;
}

/*
 * Which control this machine has, found once. HWP CPUs also publish
 * dev.cpu.0.freq_levels, but hwpstate_intel(4) is an information-only
 * cpufreq driver there, so the preference is the control.
 */
static void
cpuPowerProbe(void *context __unused)
{
    int         value;
    size_t      len;

    gCPUControl = kCPUControlNone;

    len = sizeof(gCPUMaxID);
    if (sysctlbyname("kern.smp.maxid", &gCPUMaxID, &len, NULL, 0) != 0)
        gCPUMaxID = 0;

    len = sizeof(value);
    if (sysctlbyname("dev.hwpstate_intel.0.epp", &value, &len, NULL, 0) == 0) {
        gCPUControl = kCPUControlHWP;
        INFO_LOG("CPU power: hardware P-states, energy/performance preference %d\n", value);
    } else if (copyFreqLevels(&gFreqLowest, &gFreqHighest)) {
        gCPUControl = kCPUControlFreqLevels;
        INFO_LOG("CPU power: frequency levels %d-%d MHz\n", gFreqLowest, gFreqHighest);
    }
}

/* The energy prefs code may ask from another queue than the main one. */
static CPUControl
cpuPowerControl(void)
{
    static dispatch_once_t  once;

    dispatch_once_f(&once, NULL, cpuPowerProbe);
    return gCPUControl;
}

/* The settings' number for key, or -1 when the profile does not carry it. */
static int
settingValue(CFDictionaryRef settings, const char *key)
{
    CFStringRef     k;
    CFNumberRef     n;
    int             value = -1;

    k = CFStringCreateWithCString(0, key, kCFStringEncodingUTF8);
    if (!k)
        return -1;
    n = isA_CFNumber(CFDictionaryGetValue(settings, k));
    if (n)
        CFNumberGetValue(n, kCFNumberIntType, &value);
    CFRelease(k);
    return value;
}

static CPUPolicy
cpuPolicyForSettings(CFDictionaryRef settings)
{
    if (settingValue(settings, kIOPMLowPowerModeKey) > 0)
        return kCPUPolicyEfficiency;
    if (settingValue(settings, kIOPMDynamicPowerStepKey) == 0)
        return kCPUPolicyPerformance;
    if (settingValue(settings, kIOPMReduceSpeedKey) > 0)
        return kCPUPolicyEfficiency;
    return kCPUPolicyBalanced;
}

static void
setEnergyPerformancePreference(int epp)
{
    char        name[64];
    int         cpu, set = 0;

    if (epp == gAppliedEPP)
        return;

    for (cpu = 0; cpu <= gCPUMaxID; cpu++) {
        snprintf(name, sizeof(name), "dev.hwpstate_intel.%d.epp", cpu);
        if (sysctlbyname(name, NULL, NULL, &epp, sizeof(epp)) == 0) {
            set++;
        } else if (errno != ENOENT) {
            ERROR_LOG("CPU power: %s = %d failed: %s\n", name, epp, strerror(errno));
        }
    }
    if (set) {
        INFO_LOG("CPU power: energy/performance preference %d on %d CPUs\n", epp, set);
        gAppliedEPP = epp;
    }
}

static void
setFrequency(int mhz)
{
    if (sysctlbyname("dev.cpu.0.freq", NULL, NULL, &mhz, sizeof(mhz)) != 0) {
        ERROR_LOG("CPU power: dev.cpu.0.freq = %d failed: %s\n", mhz, strerror(errno));
        return;
    }
    INFO_LOG("CPU power: frequency %d MHz\n", mhz);
}

/*
 * PMSettings.m's ActivatePMSettings(): the profile now in force, after
 * PMSettings.m's overrides, for the power source getActivePSType() reports.
 */
__private_extern__ void
cpuPowerActivateSettings(CFDictionaryRef settings)
{
    CPUPolicy   policy;
    int         epp;

    if (!isA_CFDictionary(settings))
        return;

    switch (cpuPowerControl()) {
    case kCPUControlHWP:
        policy = cpuPolicyForSettings(settings);
        if (policy == kCPUPolicyEfficiency) {
            epp = kCPUEPPEfficiency;
        } else if (policy == kCPUPolicyPerformance) {
            epp = kCPUEPPPerformance;
        } else if (getActivePSType() == kIOPSProvidedByAC) {
            epp = kCPUEPPBalancedAC;
        } else {
            epp = kCPUEPPBalancedBattery;
        }
        setEnergyPerformancePreference(epp);
        break;

    case kCPUControlFreqLevels:
        policy = cpuPolicyForSettings(settings);
        if (policy == kCPUPolicyEfficiency) {
            if (!gFreqLowered) {
                setFrequency(gFreqLowest);
                gFreqLowered = true;
            }
        } else if (gFreqLowered || policy == kCPUPolicyPerformance) {
            setFrequency(gFreqHighest);
            gFreqLowered = false;
        }
        break;

    default:
        break;
    }
}

/*
 * IOPMEnergyPrefs_freebsd.c: whether the platform offers one of the CPU
 * settings. A Mac's platform plugin publishes "LowPowerMode" in the root
 * domain's "Supported Features" where it can honour it; NextBSD's root
 * domain cannot publish a dictionary, so the energy prefs code asks here.
 * The Intel-era "Reduce Processor Speed" and "Dynamic Power Step" settings
 * are not offered, as on current Macs; only the overrides above use them.
 */
__private_extern__ bool
cpuPowerFeatureIsAvailable(CFStringRef feature)
{
    if (!isA_CFString(feature) || !CFEqual(feature, CFSTR(kIOPMLowPowerModeKey)))
        return false;
    return (cpuPowerControl() == kCPUControlHWP
            || cpuPowerControl() == kCPUControlFreqLevels);
}
