/*
 * RootDomain_freebsd.c — pmconfigd's one kernel shim: the IOPMrootDomain
 * user client over /dev/iopmroot (nextbsd-userland#226, E17 D2).
 *
 * As on macOS, the power-management root domain lives in the kernel
 * (nextbsd-kernel#243): it sends the system sleep/wake messages to apps,
 * collects their answers and sleeps the machine, and pmconfigd is one of
 * its clients. On macOS pmconfigd reaches it three ways:
 *
 *   - IOServiceOpen() on the IOPMrootDomain service, then
 *     IOConnectCallMethod() with the kPM* selectors of xnu's IOPMLibDefs.h,
 *     which xnu's RootDomainUserClient serves;
 *   - IORegistryEntrySetCFProperty() on the root domain;
 *   - IOServiceAddInterestNotification(kIOGeneralInterest) on it, for the
 *     clamshell and property-change messages.
 *
 * NextBSD's user client is /dev/iopmroot. The second and third already
 * work through libIOKit (L4, nextbsd-userland#234). The first is this file:
 * PrivateLib_freebsd.h routes IOServiceOpen and IOConnectCallMethod here, and
 * each selector becomes the matching /dev/iopmroot ioctl (<sys/iopmroot.h>,
 * vendored as libIOKit's iopmroot.h). No selector is emulated in pmconfigd;
 * where the kernel has nothing behind one, the answer is
 * kIOReturnUnsupported.
 *
 *   kPMSetAggressiveness (0)          IOPMSetAggressiveness  (IOPMROOT_SETAGGR)
 *   kPMGetAggressiveness (1)          IOPMGetAggressiveness  (IOPMROOT_GETAGGR)
 *   kPMSleepSystem (2)                IOPMSleepSystem        (IOPMROOT_SLEEP)
 *   kPMAllowPowerChange (3)           IOAllowPowerChange     (IOPMROOT_ALLOW)
 *   kPMCancelPowerChange (4)          IOCancelPowerChange    (IOPMROOT_CANCEL)
 *   kPMShutdownSystem (5), kPMRestartSystem (6)
 *                                     kIOReturnUnsupported, as on xnu
 *   kPMSleepSystemOptions (7)         IOPMSleepSystemWithOptions (IOPMROOT_SLEEP)
 *   kPMSetMaintenanceWakeCalendar (8) kIOReturnUnsupported (no RTC alarm)
 *   kPMSetUserAssertionLevels (9)     IOPMROOT_SETASSERTIONS
 *   kPMActivityTickle (10)            IOPMROOT_TICKLE
 *   kPMGetSystemSleepType (11)        kIOPMSleepTypeNormalSleep, no timer
 *   kPMSetClamshellSleepState (12)    IOPMROOT_SETCLAMSHELL
 *   kPMSleepWakeWatchdogEnable (13), kPMSleepWakeDebugTrig (14),
 *   kPMSetDisplayPowerOn (15), kPMSetDisplayState (16)
 *                                     kIOReturnUnsupported
 *   kPMRequestIdleSleepRevert (17)    IOPMROOT_IDLEREVERT
 *   kPMSetLDMHibernationDisable (18), kPMGetAssertionLog (19),
 *   kPMSetAssertionLogThreshold (20)  kIOReturnUnsupported
 *
 * The argument counts are checked as xnu's dispatch table checks them, and
 * results come back where RootDomainUserClient puts them: the sleep and
 * set-aggressiveness calls succeed and carry their result in the first
 * output scalar (kPMSleepSystemOptions: in the output structure), the
 * others return it.
 *
 * One descriptor. The kernel lets one /dev/iopmroot descriptor own the lid
 * and power-button policy: the first to call IOPMROOT_SETCLAMSHELL or to set
 * "DisablePowerButtonSleep" / "Sleep On Power Button" (others get EBUSY).
 * pmconfigd sets those properties with IORegistryEntrySetCFProperty, which
 * libIOKit sends through its process-wide descriptor, so the ioctls this file
 * makes itself go through that same descriptor (__io_pmroot_fd()). The
 * connection IOServiceOpen returns is libIOKit's (IOPMFindPowerManagement)
 * and carries the calls libIOKit has an API for; none of those claims the
 * policy.
 *
 * Also here, because they are the rest of pmconfigd's kernel-facing code:
 * getRootDomain() (common/CommonLib.c), _setRootDomainProperty() and
 * _copyRootDomainProperty() (PrivateLib.m), getRootDomainConnect()
 * (PMConnection.m), and the root domain's general-interest listener,
 * initializeRootDomainInterestNotifications() and RootDomainInterest()
 * (pmconfigd.m). When an Apple file that owns one of these builds, its copy
 * here is deleted (the linker reports any left behind as a duplicate).
 */
#include "PrivateLib_freebsd.h"

#include "iopmroot.h"		/* libIOKit's vendored <sys/iopmroot.h> */

#include <IOKit/IOMessage.h>

#include <sys/ioctl.h>

#include <errno.h>
#include <string.h>

/*
 * libIOKit's process-wide /dev/iopmroot descriptor (IOPMLib.c), the one
 * IORegistryEntrySetCFProperty on the root domain uses; -1 when the kernel
 * has no root domain. See "One descriptor" above.
 */
extern int __io_pmroot_fd(void);

/* ---- common/CommonLib.c ------------------------------------------------ */

/*
 * Apple finds the root domain by its IOPower plane path,
 * IOPower:/IOPowerConnection/IOPMrootDomain. NextBSD's registry has the one
 * IOService plane, where the root domain is the one entry of its class.
 */
__private_extern__ io_registry_entry_t getRootDomain(void)
{
    static io_registry_entry_t gRoot = IO_OBJECT_NULL;

    if (IO_OBJECT_NULL == gRoot)
        gRoot = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(IOPMROOT_CLASS));

    return gRoot;
}

/* ---- PrivateLib.m ------------------------------------------------------ */

__private_extern__ IOReturn
_setRootDomainProperty(
    CFStringRef                 key,
    CFTypeRef                   val)
{
    return IORegistryEntrySetCFProperty(getRootDomain(), key, val);
}

__private_extern__ CFTypeRef
_copyRootDomainProperty(
    CFStringRef                 key)
{
    return IORegistryEntryCreateCFProperty(getRootDomain(), key, kCFAllocatorDefault, 0);
}

/* ---- IOServiceOpen: the root domain's user client ---------------------- */

/*
 * The root domain is the one service pmconfigd opens. As each IOServiceOpen
 * makes its own user client, each call returns a new connection;
 * IOServiceClose() closes it.
 */
__private_extern__ kern_return_t
pmRootDomainServiceOpen(io_service_t service, mach_port_t owningTask __unused,
                        uint32_t type __unused, io_connect_t *connect)
{
    io_name_t       klass;

    if (connect == NULL) {
        return kIOReturnBadArgument;
    }
    *connect = IO_OBJECT_NULL;

    if (service == IO_OBJECT_NULL
        || IOObjectGetClass(service, klass) != KERN_SUCCESS)
    {
        return kIOReturnBadArgument;
    }
    if (strcmp(klass, IOPMROOT_CLASS) != 0) {
        return kIOReturnUnsupported;
    }

    *connect = IOPMFindPowerManagement(MACH_PORT_NULL);
    return (*connect != IO_OBJECT_NULL) ? kIOReturnSuccess : kIOReturnNoDevice;
}

/* ---- PMConnection.m ---------------------------------------------------- */

/*
 * PMConnection_prime() opens gRootDomainConnect with IOServiceOpen on
 * getRootDomain(). Until PMConnection.m builds (nextbsd-userland#229) the
 * connection is opened here on first use, on pmconfigd's main queue.
 */
static io_connect_t     gRootDomainConnect = IO_OBJECT_NULL;

__private_extern__ io_connect_t getRootDomainConnect(void)
{
    static bool         tried = false;
    io_service_t        rootDomainService;
    kern_return_t       kr;

    if (gRootDomainConnect == IO_OBJECT_NULL && !tried) {
        tried = true;

        rootDomainService = getRootDomain();
        if (IO_OBJECT_NULL == rootDomainService) {
            ERROR_LOG("Could not find IOPMrootDomain\n");
            return IO_OBJECT_NULL;
        }

        kr = IOServiceOpen(rootDomainService, mach_task_self(), 0, &gRootDomainConnect);
        if (KERN_SUCCESS != kr) {
            ERROR_LOG("Could not open IOPMrootDomain (0x%x)\n", kr);
            gRootDomainConnect = IO_OBJECT_NULL;
        }
    }

    return gRootDomainConnect;
}

/* ---- IOConnectCallMethod: the kPM* selectors --------------------------- */

/* The errno of a /dev/iopmroot ioctl as RootDomainUserClient's IOReturn
 * (the same mapping as libIOKit's IOPMLib.c). */
static IOReturn
pmRootDomainIOReturn(int error)
{
    switch (error) {
    case 0:
        return kIOReturnSuccess;
    case EPERM:
        return kIOReturnNotPrivileged;
    case EINVAL:
        return kIOReturnBadArgument;
    case EOPNOTSUPP:
        return kIOReturnUnsupported;
    case EBUSY:
        return kIOReturnNotPermitted;
    case ENOENT:
    case ENXIO:
        return kIOReturnNoDevice;
    default:
        return kIOReturnError;
    }
}

static IOReturn
pmRootDomainIoctl(unsigned long cmd, void *arg)
{
    int             fd = __io_pmroot_fd();

    if (fd < 0) {
        return kIOReturnNoDevice;
    }
    if (ioctl(fd, cmd, arg) != 0) {
        return pmRootDomainIOReturn(errno);
    }
    return kIOReturnSuccess;
}

/*
 * xnu's RootDomainUserClient::externalMethod dispatch table: how many
 * scalars each selector takes and gives, and whether it takes a structure.
 * A call that does not match is refused with kIOReturnBadArgument before the
 * selector runs, as dispatchExternalMethod() does.
 */
#define kStructNone     0
#define kStructAny      1       /* kIOUCVariableStructureSize */

static const struct {
    uint8_t     scalarIn;
    uint8_t     scalarOut;
    uint8_t     structIn;
    uint8_t     structOut;      /* bytes */
} pmMethods[] = {
    [kPMSetAggressiveness]          = { 2, 1, kStructNone, 0 },
    [kPMGetAggressiveness]          = { 1, 1, kStructNone, 0 },
    [kPMSleepSystem]                = { 0, 1, kStructNone, 0 },
    [kPMAllowPowerChange]           = { 1, 0, kStructNone, 0 },
    [kPMCancelPowerChange]          = { 1, 0, kStructNone, 0 },
    [kPMShutdownSystem]             = { 0, 0, kStructNone, 0 },
    [kPMRestartSystem]              = { 0, 0, kStructNone, 0 },
    [kPMSleepSystemOptions]         = { 0, 0, kStructAny,  sizeof(uint32_t) },
    [kPMSetMaintenanceWakeCalendar] = { 0, 0, kStructAny,  sizeof(uint32_t) },
    [kPMSetUserAssertionLevels]     = { 1, 0, kStructNone, 0 },
    [kPMActivityTickle]             = { 0, 0, kStructNone, 0 },
    [kPMGetSystemSleepType]         = { 0, 2, kStructNone, 0 },
    [kPMSetClamshellSleepState]     = { 1, 0, kStructNone, 0 },
    [kPMSleepWakeWatchdogEnable]    = { 0, 0, kStructNone, 0 },
    [kPMSleepWakeDebugTrig]         = { 0, 0, kStructNone, 0 },
    [kPMSetDisplayPowerOn]          = { 1, 0, kStructNone, 0 },
    [kPMSetDisplayState]            = { 0, 0, kStructNone, 0 },
    [kPMRequestIdleSleepRevert]     = { 0, 1, kStructNone, 0 },
    [kPMSetLDMHibernationDisable]   = { 1, 0, kStructNone, 0 },
    [kPMGetAssertionLog]            = { 0, 0, kStructNone, 0 },
    [kPMSetAssertionLogThreshold]   = { 1, 0, kStructNone, 0 },
};

/* kPMSleepSystemOptions: the options dictionary, as IOCFSerialize() or
 * CFPropertyListCreateData() wrote it. NULL if it does not parse, which, as
 * on xnu, makes the call a sleep without options. */
static CFDictionaryRef
pmCopySleepOptions(const void *bytes, size_t len)
{
    CFDataRef           data;
    CFPropertyListRef   plist = NULL;

    if (bytes == NULL || len == 0) {
        return NULL;
    }
    data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes, (CFIndex)len, kCFAllocatorNull);
    if (data) {
        plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
        CFRelease(data);
    }
    if (plist && !isA_CFDictionary(plist)) {
        ERROR_LOG("SleepSystem options are not a dictionary\n");
        CFRelease(plist);
        plist = NULL;
    }
    return (CFDictionaryRef)plist;
}

__private_extern__ kern_return_t
pmRootDomainCallMethod(
    io_connect_t        connection,
    uint32_t            selector,
    const uint64_t      *input,
    uint32_t            inputCnt,
    const void          *inputStruct,
    size_t              inputStructCnt,
    uint64_t            *output,
    uint32_t            *outputCnt,
    void                *outputStruct,
    size_t              *outputStructCnt)
{
    uint32_t            outCnt = outputCnt ? *outputCnt : 0;
    size_t              outStructCnt = outputStructCnt ? *outputStructCnt : 0;
    IOReturn            ret;

    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }
    if (selector >= sizeof(pmMethods) / sizeof(pmMethods[0])
        || inputCnt != pmMethods[selector].scalarIn
        || outCnt != pmMethods[selector].scalarOut
        || (pmMethods[selector].scalarIn && input == NULL)
        || (pmMethods[selector].scalarOut && output == NULL)
        || (pmMethods[selector].structIn == kStructNone && inputStructCnt != 0)
        || outStructCnt != pmMethods[selector].structOut
        || (pmMethods[selector].structOut && outputStruct == NULL))
    {
        return kIOReturnBadArgument;
    }

    switch (selector) {
    case kPMSetAggressiveness:
        output[0] = (uint64_t)IOPMSetAggressiveness(connection,
                                (unsigned long)input[0], (unsigned long)input[1]);
        return kIOReturnSuccess;

    case kPMGetAggressiveness:
    {
        unsigned long   value = 0;

        ret = IOPMGetAggressiveness(connection, (unsigned long)input[0], &value);
        if (ret == kIOReturnSuccess) {
            output[0] = value;
        }
        return ret;
    }

    case kPMSleepSystem:
        output[0] = (uint64_t)IOPMSleepSystem(connection);
        return kIOReturnSuccess;

    case kPMAllowPowerChange:
        return IOAllowPowerChange(connection, (intptr_t)input[0]);

    case kPMCancelPowerChange:
        return IOCancelPowerChange(connection, (intptr_t)input[0]);

    case kPMSleepSystemOptions:
    {
        CFDictionaryRef options = pmCopySleepOptions(inputStruct, inputStructCnt);
        uint32_t        rc;

        rc = (uint32_t)IOPMSleepSystemWithOptions(connection, options);
        memcpy(outputStruct, &rc, sizeof(rc));
        if (options) {
            CFRelease(options);
        }
        return kIOReturnSuccess;
    }

    case kPMSetUserAssertionLevels:
    {
        uint64_t        levels = (uint32_t)input[0];

        return pmRootDomainIoctl(IOPMROOT_SETASSERTIONS, &levels);
    }

    case kPMActivityTickle:
        return pmRootDomainIoctl(IOPMROOT_TICKLE, NULL);

    case kPMGetSystemSleepType:
        /* S3 is the one sleep type; no standby or hibernate timer. */
        output[0] = kIOPMSleepTypeNormalSleep;
        output[1] = 0;
        return kIOReturnSuccess;

    case kPMSetClamshellSleepState:
    {
        uint32_t        disable = input[0] ? 1 : 0;

        ret = pmRootDomainIoctl(IOPMROOT_SETCLAMSHELL, &disable);
        /* Another descriptor owns the lid and button policy. */
        return (ret == kIOReturnNotPermitted) ? kIOReturnExclusiveAccess : ret;
    }

    case kPMRequestIdleSleepRevert:
    {
        /*
         * The kernel can revert an idle sleep only while it is still asking
         * kIOMessageCanSystemSleep; once it has said kIOMessageSystemWillSleep
         * the sleep goes ahead, as on xnu.
         */
        struct iopmroot_state   st;

        memset(&st, 0, sizeof(st));
        ret = pmRootDomainIoctl(IOPMROOT_GETSTATE, &st);
        if (ret == kIOReturnSuccess) {
            ret = pmRootDomainIoctl(IOPMROOT_IDLEREVERT, NULL);
        }
        if (ret == kIOReturnSuccess) {
            output[0] = (st.phase == IOPMROOT_PHASE_CAN_SLEEP) ? 1 : 0;
        }
        return ret;
    }

    case kPMShutdownSystem:
    case kPMRestartSystem:
        /* Deprecated on xnu too. */
    case kPMSetMaintenanceWakeCalendar:
        /* No RTC alarm behind the root domain (nextbsd-kernel#245). */
    case kPMSleepWakeWatchdogEnable:
    case kPMSleepWakeDebugTrig:
    case kPMSetDisplayPowerOn:
    case kPMSetDisplayState:
    case kPMSetLDMHibernationDisable:
    case kPMGetAssertionLog:
    case kPMSetAssertionLogThreshold:
    default:
        return kIOReturnUnsupported;
    }
}

/* ---- pmconfigd.m: the root domain's general interest ------------------- */

/*
 * The root domain's own state, logged once at start: what PMDisplay.m's
 * initializeClamshellState() reads (kAppleClamshellStateKey), and whether
 * this machine can sleep at all.
 */
static void
logRootDomainState(void)
{
    CFTypeRef       clamshell = _copyRootDomainProperty(CFSTR(kAppleClamshellStateKey));
    CFTypeRef       sleepSupported = _copyRootDomainProperty(CFSTR("IOSleepSupported"));

    INFO_LOG("rootDomain clamshell state %s, sleep %ssupported\n",
             clamshell == NULL ? "none" : (clamshell == kCFBooleanTrue ? "closed" : "open"),
             sleepSupported == kCFBooleanTrue ? "" : "not ");

    if (clamshell) {
        CFRelease(clamshell);
    }
    if (sleepSupported) {
        CFRelease(sleepSupported);
    }
}

/*
 * Apple's handler acts on the driver-assertion, system-power-event,
 * dark-wake-thermal, spindump, feature-change and sleep/wake-UUID messages,
 * none of which NextBSD's root domain sends yet, and (on Apple silicon) on
 * kIOPMMessageClamshellStateChange, which it does send. The clamshell state
 * is logged here; acting on it (PMDisplay.m's updateClamshellState) is the
 * sleep/wake policy's (nextbsd-userland#229). kIOMessageServicePropertyChange,
 * which follows every property update, is ignored, as on macOS.
 *
 * libIOKit hands a general-interest callback an
 * IOServiceInterestMessageArgument; Apple's kernel passes the clamshell bits
 * as the argument pointer itself. They are argument[0] here.
 */
static void
RootDomainInterest(
    void *refcon __unused,
    io_service_t root_domain __unused,
    natural_t messageType,
    void *messageArgument)
{
    const IOServiceInterestMessageArgument *arg = messageArgument;

    if (messageType == kIOPMMessageClamshellStateChange && arg != NULL)
    {
        uint32_t    bits = (uint32_t)arg->argument[0];

        INFO_LOG("Clamshell state changed: %s, closing %s\n",
                 (bits & kClamshellStateBit) ? "closed" : "open",
                 (bits & kClamshellSleepBit) ? "sleeps" : "does not sleep");
    }
}

__private_extern__ void
initializeRootDomainInterestNotifications(void)
{
    IONotificationPortRef       note_port = NULL;
    io_service_t                root_domain = IO_OBJECT_NULL;
    io_object_t                 notification_object = IO_OBJECT_NULL;
    IOReturn                    ret;

    root_domain = getRootDomain();

    if(!root_domain) return;

    note_port = IONotificationPortCreate(MACH_PORT_NULL);
    if(!note_port) goto exit;

    ret = IOServiceAddInterestNotification(note_port, root_domain,
                kIOGeneralInterest, RootDomainInterest,
                NULL, &notification_object);
    if (ret != kIOReturnSuccess) {
        ERROR_LOG("IOPMrootDomain interest notification failed: 0x%x\n", ret);
        IONotificationPortDestroy(note_port);
        goto exit;
    }

    IONotificationPortSetDispatchQueue(note_port, _getPMMainQueue());
    logRootDomainState();

exit:
    // Do not release notification_object, would uninstall notification
    // Do not release note_port: both must be kept around to receive this
    // notification. root_domain is getRootDomain()'s cached reference.
    return;
}
