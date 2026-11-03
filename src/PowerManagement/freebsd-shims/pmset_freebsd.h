/*
 * pmset_freebsd.h — what Apple's pmset/pmset.m takes from Darwin headers and
 * frameworks NextBSD does not have (nextbsd-userland#235, E17 C1).
 *
 * pmset.m includes this header under __FreeBSD__ in place of its include
 * list; the code that uses the names below is Apple's, unchanged. pmset is
 * built as C (pmset_freebsd.c), since NextBSD's base has no Objective-C
 * runtime and pmset.m's only Objective-C is the SkyLight import Intel Macs
 * do not take.
 *
 *  - TargetConditionals. NextBSD's CoreFoundation sets TARGET_OS_OSX to 0
 *    (CFBase.h would otherwise want <MacTypes.h>), which would send pmset
 *    down its iOS paths. pmset is the Intel Mac's: once every header pmset.m
 *    names has been read, TARGET_OS_OSX is 1 and TARGET_CPU_ARM64 is 0 for
 *    pmset.m's own code, on amd64 and arm64 alike (NextBSD's arm64 machines
 *    are not Apple silicon Macs: no SkyLight, Lockdown Mode or ldmHibernate).
 *  - IOReport (libIOReport): NextBSD's kernel publishes no IOReporting
 *    channels. The calls return nothing, so -g powerstate, -g stats and
 *    -g sleepblockers report that there is no data, as pmset does when a
 *    channel is missing.
 *  - SkyLight, DisplayServices, MobileGestalt, Lockdown Mode: not there.
 *    displaysleepnow takes the Intel path (the display wrangler);
 *    resetdisplayambientparams says it is unsupported; the MobileGestalt
 *    question (autorestartatconnect) answers no.
 *  - IOHibernatePrivate.h: the hibernation setting keys only. NextBSD does
 *    not hibernate, and the root domain lists no hibernation feature, so
 *    pmset prunes them as unsupported.
 *  - IOPMUPSPrivate.h: libIOKit has no UPS shutdown-threshold preferences
 *    (IOKitUser's IOPMUPSPrefs.c). Reading them finds none; writing them is
 *    kIOReturnUnsupported.
 *  - IORegistryEntryFromPath: libIOKit has no planes but IOService. The
 *    two paths pmset names are looked up by class: the root domain (the one
 *    IOPMrootDomain entry, as libIOKit's pwr_mgt_freebsd.h does) and the
 *    display wrangler (IODisplayWrangler; none today, see displaySleepNow).
 *  - CFDateFormatter and CFCalendar (ICU): pmset parses its schedule dates
 *    with strptime(3) and prints dates with strftime(3), in the local time
 *    zone. The formatter calls pmset.m makes are mapped onto them below,
 *    with the same patterns ("MM/dd/yy HH:mm:ss" and the rest), so the
 *    command line and the output read as on macOS.
 *  - CommonLib.c: the few functions pmset uses are in pmset_freebsd.c,
 *    verbatim; the rest of CommonLib.c is powerd's.
 */
#ifndef _PMSET_FREEBSD_H_
#define _PMSET_FREEBSD_H_

#include <CoreFoundation/CoreFoundation.h>

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>

#include <sys/types.h>
#include <sys/sysctl.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOReturn.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPowerSourcesPrivate.h>

/*
 * syslog's <asl.h> (CommonLib.h includes it) marks its routines with
 * __API_DEPRECATED and __not_tail_called, which the <Availability.h> seen
 * here does not define; define them away, as powerd's
 * PrivateLib_freebsd.h does.
 */
#ifndef __API_DEPRECATED
#define __API_DEPRECATED(...)
#endif
#ifndef __not_tail_called
#define __not_tail_called
#endif

#include "CommonLib.h"

/* dynamically mig generated (build-userland.sh: $PM_MIG) */
#include "powermanagement.h"

#include <servers/bootstrap.h>
#include <bootstrap_priv.h>
#include <mach/mach.h>
#include <mach/mach_port.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <notify.h>
#include <asl.h>
#include <dirent.h>
#include <sysexits.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>

/* ---- TargetConditionals: pmset.m is the Intel Mac's ------------------- */
#undef  TARGET_OS_OSX
#define TARGET_OS_OSX                   1
#undef  TARGET_CPU_ARM64
#define TARGET_CPU_ARM64                0

/* ---- Objective-C ------------------------------------------------------ */
#ifndef nil
#define nil                             NULL
#endif

/* ---- IOKit/IOTypes.h, IOKit/IOKitKeys.h (xnu) ------------------------- */
#ifndef kNilOptions
#define kNilOptions                     0
#endif
#ifndef kIOUserClientCreatorKey
#define kIOUserClientCreatorKey         "IOUserClientCreator"
#endif

/* ---- IOKit/pwr_mgt/IOPMLibPrivate.h (newer than the IOKitUser drop) --- */
#ifndef kIOPMAssertionTypeDisableRealPowerSources_Debug
#define kIOPMAssertionTypeDisableRealPowerSources_Debug \
        CFSTR("DisableRealPowerSources_Debug")
#endif

/* ---- SkyLight, DisplayServices ---------------------------------------- */
#define SLSDISPLAYMANAGER               0
#define PLATFORM_HAS_DISPLAYSERVICES    0

/* ---- MobileGestalt ---------------------------------------------------- */
#define kMGQDeviceSupportsActionAfterPowerConnect \
        CFSTR("DeviceSupportsActionAfterPowerConnect")
static __inline bool
MGGetBoolAnswer(CFStringRef question __unused)
{
    return false;
}

/* ---- IOKit/IOHibernatePrivate.h (xnu): the setting keys --------------- */
#ifndef kIOHibernateModeKey
#define kIOHibernateModeKey             "Hibernate Mode"
#define kIOHibernateFileKey             "Hibernate File"
#define kIOHibernateFreeRatioKey        "Hibernate Free Ratio"
#define kIOHibernateFreeTimeKey         "Hibernate Free Time"
#endif

/* ---- IOKit/pwr_mgt/IOPMUPSPrivate.h (IOKitUser) ----------------------- */
#ifndef kIOPMDefaultUPSThresholds
#define kIOPMDefaultUPSThresholds       "UPSDefaultThresholds"
#define kIOUPSShutdownAtLevelKey        "UPSShutdownAtLevel"
#define kIOUPSShutdownAfterMinutesOn    "UPSShutdownAfterMinutes"
#define kIOUPSShutdownAtMinutesLeft     "UPSShutdownAtMinutesLeft"
#define kIOUPSShutdownLevelEnabledKey   "Enabled"
#define kIOUPSShutdownLevelValueKey     "Value"
#endif

static __inline CFDictionaryRef
IOPMCopyUPSShutdownLevels(CFTypeRef whichUPS __unused)
{
    return NULL;
}

static __inline IOReturn
IOPMSetUPSShutdownLevels(CFTypeRef whichUPS __unused,
    CFDictionaryRef UPSPrefs __unused)
{
    return kIOReturnUnsupported;
}

/* ---- IOReport.h, IOKit/IOReportTypes.h -------------------------------- */
#define IOREPORT_MAKEID(A, B, C, D, E, F, G, H) \
    (((uint64_t)(A) << 56) | ((uint64_t)(B) << 48) | ((uint64_t)(C) << 40) | \
     ((uint64_t)(D) << 32) | ((uint64_t)(E) << 24) | ((uint64_t)(F) << 16) | \
     ((uint64_t)(G) << 8) | ((uint64_t)(H)))

typedef CFDictionaryRef                 IOReportChannelRef;
typedef CFDictionaryRef                 IOReportSampleRef;
typedef struct __IOReportSubscription  *IOReportSubscriptionRef;
typedef int                             IOReportIterationResult;
enum {
    kIOReportIterOk = 0
};
typedef IOReportIterationResult (^ioreportiterateblock)(IOReportSampleRef ch);

static __inline CFMutableDictionaryRef
IOReportCopyChannelsWithID(CFDictionaryRef matching __unused,
    uint64_t channel_id __unused, CFMutableDictionaryRef *legend __unused)
{
    return NULL;
}

static __inline IOReportSubscriptionRef
IOReportCreateSubscription(void *a __unused, CFMutableDictionaryRef chans __unused,
    CFMutableDictionaryRef *subbed, uint64_t channel_id __unused,
    CFTypeRef b __unused)
{
    if (subbed) *subbed = NULL;
    return NULL;
}

static __inline CFDictionaryRef
IOReportCreateSamples(IOReportSubscriptionRef sub __unused,
    CFMutableDictionaryRef chans __unused, CFTypeRef a __unused)
{
    return NULL;
}

static __inline CFDictionaryRef
IOReportCreateSamplesDelta(CFDictionaryRef prev __unused,
    CFDictionaryRef current __unused, CFTypeRef a __unused)
{
    return NULL;
}

static __inline IOReportIterationResult
IOReportMergeChannels(CFMutableDictionaryRef a __unused,
    CFDictionaryRef b __unused, CFTypeRef c __unused)
{
    return kIOReportIterOk;
}

static __inline void
IOReportIterate(CFDictionaryRef samples __unused,
    ioreportiterateblock callback __unused)
{
}

static __inline CFStringRef IOReportChannelGetDriverName(IOReportChannelRef ch __unused) { return NULL; }
static __inline uint64_t IOReportChannelGetChannelID(IOReportChannelRef ch __unused) { return 0; }
static __inline int64_t IOReportSimpleGetIntegerValue(IOReportChannelRef ch __unused, int32_t *rv __unused) { return 0; }
static __inline int64_t IOReportArrayGetValueAtIndex(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }
static __inline int32_t IOReportStateGetCount(IOReportChannelRef ch __unused) { return 0; }
static __inline int32_t IOReportStateGetCurrent(IOReportChannelRef ch __unused) { return -1; }
static __inline uint64_t IOReportStateGetIDForIndex(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }
static __inline int64_t IOReportStateGetInTransitions(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }
static __inline int64_t IOReportStateGetResidency(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }
static __inline int32_t IOReportHistogramGetBucketCount(IOReportChannelRef ch __unused) { return 0; }
static __inline int64_t IOReportHistogramGetBucketHits(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }
static __inline int64_t IOReportHistogramGetBucketMinValue(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }
static __inline int64_t IOReportHistogramGetBucketMaxValue(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }
static __inline int64_t IOReportHistogramGetBucketSum(IOReportChannelRef ch __unused, int32_t i __unused) { return 0; }

/* ---- CFDateFormatter, CFCalendar: strptime(3) and strftime(3) --------- */
/*
 * A formatter is the strftime(3) format it stands for, kept in a
 * CFMutableString so that pmset's CFRelease() of it works. The ICU patterns
 * pmset sets are translated field by field (yyyy yy MM dd HH mm ss ZZZ);
 * without one, the date and time styles pick a fixed format. Time zone and
 * locale are the process's (TZ, /etc/localtime), as CFTimeZoneCopySystem()
 * would give.
 */
typedef CFMutableStringRef              __pmset_CFDateFormatterRef;
#define CFDateFormatterRef              __pmset_CFDateFormatterRef

static __inline void
__pmset_DateFormatterSetStyles(CFMutableStringRef f,
    CFDateFormatterStyle dateStyle, CFDateFormatterStyle timeStyle)
{
    const char *d = "", *t = "";

    switch (dateStyle) {
    case kCFDateFormatterNoStyle:       d = ""; break;
    case kCFDateFormatterShortStyle:    d = "%m/%d/%y"; break;
    default:                            d = "%m/%d/%Y"; break;
    }
    switch (timeStyle) {
    case kCFDateFormatterNoStyle:       t = ""; break;
    case kCFDateFormatterShortStyle:    t = "%H:%M"; break;
    case kCFDateFormatterMediumStyle:   t = "%H:%M:%S"; break;
    default:                            t = "%H:%M:%S %Z"; break;
    }
    CFStringReplaceAll(f, CFSTR(""));
    CFStringAppendCString(f, d, kCFStringEncodingUTF8);
    if (*d && *t) {
        CFStringAppendCString(f, " ", kCFStringEncodingUTF8);
    }
    CFStringAppendCString(f, t, kCFStringEncodingUTF8);
}

static __inline __pmset_CFDateFormatterRef
__pmset_DateFormatterCreate(CFAllocatorRef alloc __unused, CFLocaleRef locale __unused,
    CFDateFormatterStyle dateStyle, CFDateFormatterStyle timeStyle)
{
    CFMutableStringRef f = CFStringCreateMutable(NULL, 0);

    if (f) {
        __pmset_DateFormatterSetStyles(f, dateStyle, timeStyle);
    }
    return f;
}
#define CFDateFormatterCreate           __pmset_DateFormatterCreate

/* An ICU date pattern as a strftime(3) format. */
static __inline void
__pmset_DateFormatterSetFormat(__pmset_CFDateFormatterRef f, CFStringRef pattern)
{
    static const struct { const char *icu; const char *tm; } fields[] = {
        { "yyyy", "%Y" }, { "yy", "%y" }, { "MM", "%m" }, { "dd", "%d" },
        { "HH", "%H" }, { "mm", "%M" }, { "ss", "%S" }, { "ZZZ", "%z" },
    };
    char        in[64], out[128];
    size_t      i, j, o = 0;

    if (!f || !pattern ||
        !CFStringGetCString(pattern, in, sizeof(in), kCFStringEncodingUTF8)) {
        return;
    }
    for (i = 0; in[i] && o < sizeof(out) - 3; ) {
        for (j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) {
            size_t n = strlen(fields[j].icu);
            if (strncmp(&in[i], fields[j].icu, n) == 0) {
                out[o++] = fields[j].tm[0];
                out[o++] = fields[j].tm[1];
                i += n;
                break;
            }
        }
        if (j < sizeof(fields) / sizeof(fields[0])) {
            continue;
        }
        if (in[i] == '%') {
            out[o++] = '%';
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    CFStringReplaceAll(f, CFSTR(""));
    CFStringAppendCString(f, out, kCFStringEncodingUTF8);
}
#define CFDateFormatterSetFormat        __pmset_DateFormatterSetFormat

/* The time zone is the process's own; nothing else is set. */
#define CFDateFormatterSetProperty(f, key, value) \
        do { (void)(f); (void)(value); } while (0)

static __inline CFStringRef
__pmset_DateFormatterCreateStringWithAbsoluteTime(CFAllocatorRef alloc __unused,
    __pmset_CFDateFormatterRef f, CFAbsoluteTime at)
{
    char        fmt[128], buf[128];
    time_t      t;
    struct tm   tm;

    if (!f || !CFStringGetCString(f, fmt, sizeof(fmt), kCFStringEncodingUTF8)) {
        return NULL;
    }
    t = (time_t)floor(at + kCFAbsoluteTimeIntervalSince1970);
    if (!localtime_r(&t, &tm) || strftime(buf, sizeof(buf), fmt, &tm) == 0) {
        return NULL;
    }
    return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
}
#define CFDateFormatterCreateStringWithAbsoluteTime \
        __pmset_DateFormatterCreateStringWithAbsoluteTime

static __inline CFStringRef
__pmset_DateFormatterCreateStringWithDate(CFAllocatorRef alloc,
    __pmset_CFDateFormatterRef f, CFDateRef date)
{
    if (!date || CFGetTypeID(date) != CFDateGetTypeID()) {
        return NULL;
    }
    return __pmset_DateFormatterCreateStringWithAbsoluteTime(alloc, f,
        CFDateGetAbsoluteTime(date));
}
#define CFDateFormatterCreateStringWithDate \
        __pmset_DateFormatterCreateStringWithDate

/*
 * strptime(3) the whole string with fmt; fields it does not set (the date,
 * for a time of day) come from 1/1/2001. ICU's two-digit "yy" also takes a
 * four-digit year, so "%y" is retried as "%Y".
 */
static __inline bool
__pmset_strptime_all(const char *str, const char *fmt, struct tm *tm)
{
    const char *end;

    memset(tm, 0, sizeof(*tm));
    tm->tm_year = 101;
    tm->tm_mday = 1;
    end = strptime(str, fmt, tm);
    if (end == NULL) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    return (*end == '\0');
}

static __inline CFDateRef
__pmset_DateFormatterCreateDateFromString(CFAllocatorRef alloc __unused,
    __pmset_CFDateFormatterRef f, CFStringRef string, CFRange *rangep __unused)
{
    char        fmt[128], str[128];
    char        *yy;
    struct tm   tm;
    time_t      t;

    if (!f || !string ||
        !CFStringGetCString(f, fmt, sizeof(fmt), kCFStringEncodingUTF8) ||
        !CFStringGetCString(string, str, sizeof(str), kCFStringEncodingUTF8)) {
        return NULL;
    }
    if (!__pmset_strptime_all(str, fmt, &tm)) {
        if ((yy = strstr(fmt, "%y")) == NULL) {
            return NULL;
        }
        yy[1] = 'Y';
        if (!__pmset_strptime_all(str, fmt, &tm)) {
            return NULL;
        }
    }
    tm.tm_isdst = -1;
    t = mktime(&tm);
    if (t == (time_t)-1) {
        return NULL;
    }
    return CFDateCreate(NULL, (CFAbsoluteTime)t - kCFAbsoluteTimeIntervalSince1970);
}
#define CFDateFormatterCreateDateFromString \
        __pmset_DateFormatterCreateDateFromString

/*
 * CFCalendarDecomposeAbsoluteTime on the Gregorian calendar (_gregorian()),
 * in the local time zone: the units pmset asks for are y M d H m s.
 */
static __inline Boolean
__pmset_DecomposeAbsoluteTime(CFAbsoluteTime at, const char *desc, ...)
{
    va_list     ap;
    time_t      t;
    struct tm   tm;
    int         *v;

    t = (time_t)floor(at + kCFAbsoluteTimeIntervalSince1970);
    if (!desc || !localtime_r(&t, &tm)) {
        return false;
    }
    va_start(ap, desc);
    for (; *desc; desc++) {
        v = va_arg(ap, int *);
        switch (*desc) {
        case 'y': *v = tm.tm_year + 1900; break;
        case 'M': *v = tm.tm_mon + 1; break;
        case 'd': *v = tm.tm_mday; break;
        case 'H': *v = tm.tm_hour; break;
        case 'm': *v = tm.tm_min; break;
        case 's': *v = tm.tm_sec; break;
        default:  *v = 0; break;
        }
    }
    va_end(ap);
    return true;
}
#define CFCalendarDecomposeAbsoluteTime(calendar, at, desc, ...) \
        __pmset_DecomposeAbsoluteTime((at), (desc), __VA_ARGS__)

/* ---- IOKitLib.h: registry paths --------------------------------------- */
#ifndef kIOPowerPlane
#define kIOPowerPlane                   "IOPower"
#endif
#ifndef kIOServicePlane
#define kIOServicePlane                 "IOService"
#endif

static __inline io_registry_entry_t
__pmset_IORegistryEntryFromPath(mach_port_t mainPort, const char *path)
{
    if (path == NULL) {
        return IO_OBJECT_NULL;
    }
    if (strcmp(path, kIOPowerPlane ":/IOPowerConnection/IOPMrootDomain") == 0) {
        return IOServiceGetMatchingService(mainPort,
            IOServiceMatching("IOPMrootDomain"));
    }
    if (strcmp(path, kIOServicePlane ":/IOResources/IODisplayWrangler") == 0) {
        return IOServiceGetMatchingService(mainPort,
            IOServiceMatching("IODisplayWrangler"));
    }
    return IO_OBJECT_NULL;
}
#define IORegistryEntryFromPath         __pmset_IORegistryEntryFromPath

/*
 * The registry calls libIOKit does not have. Name matching is
 * IONameMatch, which libIOKit's matching understands. NextBSD's registry
 * has the IOService plane only, so an entry's name in it is its name.
 * There is no matching by registry entry id (an assertion's owning driver,
 * -g assertions), and no IOServiceOpen or IOConnectCallMethod: the root
 * domain's methods are libIOKit's IOPM* calls, and the one pmset makes
 * itself (kPMSleepWakeDebugTrig, debugTrig) has no NextBSD kernel side.
 */
static __inline CFMutableDictionaryRef
IOServiceNameMatching(const char *name)
{
    CFMutableDictionaryRef  d;
    CFStringRef             s;

    d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (d && s) {
        CFDictionarySetValue(d, CFSTR(kIONameMatchKey), s);
    }
    if (s) {
        CFRelease(s);
    }
    return d;
}

static __inline CFMutableDictionaryRef
IORegistryEntryIDMatching(uint64_t entryID __unused)
{
    return NULL;
}

static __inline kern_return_t
IORegistryEntryGetNameInPlane(io_registry_entry_t entry,
    const io_name_t plane __unused, io_name_t name)
{
    return IORegistryEntryGetName(entry, name);
}

static __inline kern_return_t
IOServiceOpen(io_service_t service __unused, mach_port_t owningTask __unused,
    uint32_t type __unused, io_connect_t *connect)
{
    if (connect) {
        *connect = IO_OBJECT_NULL;
    }
    return kIOReturnUnsupported;
}

static __inline kern_return_t
IOConnectCallMethod(io_connect_t connection __unused, uint32_t selector __unused,
    const uint64_t *input __unused, uint32_t inputCnt __unused,
    const void *inputStruct __unused, size_t inputStructCnt __unused,
    uint64_t *output __unused, uint32_t *outputCnt __unused,
    void *outputStruct __unused, size_t *outputStructCnt __unused)
{
    return kIOReturnUnsupported;
}

#endif /* !_PMSET_FREEBSD_H_ */
