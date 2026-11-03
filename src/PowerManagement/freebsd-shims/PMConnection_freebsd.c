/*
 * PMConnection_freebsd.c — pmconfigd's side of system sleep and wake, and
 * the IOPMConnection server: a C port of PMConnection.m
 * (nextbsd-userland#229, E17 D5).
 *
 * As on macOS, the kernel's IOPMrootDomain (nextbsd-kernel#243) runs the
 * sleep/wake sequence and waits for every client's answer; pmconfigd is one
 * of those clients. This file is the part of PMConnection.m that does that
 * job on macOS, with Apple's names, structures and order:
 *
 *   PMConnection_prime()          opens the root domain's user client
 *                                 (gRootDomainConnect, getRootDomainConnect())
 *                                 and registers for its sleep/wake messages
 *   PMConnectionPowerCallBack()   the state machine: gPowerState and
 *                                 gCurrentCapabilityBits, the IOPMConnection
 *                                 notifications, kIOPMSystemPowerStateNotify,
 *                                 State:/IOKit/SystemPowerCapabilities, and
 *                                 the root domain's WillSleep held until the
 *                                 IOPMConnection clients answer (28 s at most)
 *   _io_pm_connection_*()         the IOPMConnection MIG routines
 *   _io_pm_get_capability_bits()
 *
 * One difference in shape. xnu tells pmconfigd about sleep and wake with
 * kIOMessageSystemCapabilityChange on "IOPMSystemCapabilityInterest": an
 * IOPMSystemCapabilityChangeParameters saying which capabilities (CPU,
 * graphics, audio, network) come and go. NextBSD's root domain has only the
 * app-power interest (kIOAppPowerStateInterest: CanSystemSleep, WillSleep,
 * WillNotSleep, WillPowerOn, HasPoweredOn) and no dark wake, so every sleep
 * takes the whole system down and every wake is a full wake.
 * PMConnectionAppPowerCallBack() turns each app-power message into the
 * capability change xnu sends for it and hands that to Apple's
 * PMConnectionPowerCallBack():
 *
 *   kIOMessageSystemWillSleep     WillChange  CPU|Graphics|Audio|Network -> 0,
 *                                 notifyRef = the message's notificationID
 *   kIOMessageSystemWillPowerOn   WillChange  0 -> CPU|Graphics|Audio|Network
 *   kIOMessageSystemHasPoweredOn  DidChange   0 -> CPU|Graphics|Audio|Network
 *   kIOMessageCanSystemSleep,     unchanged, as xnu sends them to the
 *   kIOMessageSystemWillNotSleep  capability interest too
 *
 * The registration has its own notification port, apart from pmconfigd.m's
 * IORegisterForSystemPower one (pmconfigd_freebsd.c), so the root domain
 * waits for each, as xnu waits for the capability client and the app client
 * separately.
 *
 * Left out with the dark wake: PowerNap and sleep services, background and
 * push-service tasks, silent running and perf restriction, TCP keep-alive,
 * auto power-off and standby, the dark-wake linger, the SMC, CoreAnalytics,
 * ASL and messagetracer logging, and wake-event claims over XPC. Their places
 * in the state machine are marked "Apple:". Delete this file when
 * PMConnection.m builds.
 */
#include "PrivateLib_freebsd.h"
#include "AutoWakeScheduler.h"

#include <IOKit/IOMessage.h>
#include <xpc/xpc.h>			/* PMConnection.h: getScheduledWake() */
#include "PMConnection.h"

#include "powermanagementServer.h"	/* MIG: the _io_* prototypes */

#include <bsm/libbsm.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Two pmconfigd subsystems PMConnection.m calls into that are not on main
 * yet: PMStore.c (the SCDynamicStore cache) and BatteryTimeRemaining.m's
 * wake hook, both from nextbsd-userland#227 (D3). They are weak, so each
 * call starts working when D3's files link and is skipped until then.
 */
__private_extern__ bool PMStoreSetValue(CFStringRef key, CFTypeRef value) __attribute__((weak));
__private_extern__ void BatteryTimeRemainingWakeNotification(void) __attribute__((weak));

os_log_t    sleepwake_log = NULL;
#undef   LOG_STREAM
#define  LOG_STREAM   sleepwake_log

#define CAPABILITY_BIT_CHANGED(x, y, b)     ((x ^ y) & b)
#define CHANGED_CAP_BITS(x, y)              ((x) ^ (y))

/* Bookkeeping data types */

enum {
    _kSleepStateBits = 0x0000,
    _kOnStateBits = 0xFFFF
};

/* Array indices & for PMChooseScheduledEvent */
typedef enum {
    kChooseFullWake         = 0,
    kChooseMaintenance      = 1,
    kChooseSleepServiceWake = 2,
    kChooseTimerPlugin      = 3,
    kChooseWakeTypeCount    = 4
} wakeType_e;

static int const kMaxConnectionIDCount = 1000*1000*1000;
static int const kConnectionOffset = 1000;
static double const  kPMConnectionNotifyTimeoutDefault = 28.0;

uint64_t lastcall_ts = 0;

bool gCapabilityChangeDone = false;
bool gEmergencySleep = false;
// gPowerState - Tracks various wake types the system is currently in
// by setting appropriate bits.
static uint32_t                 gPowerState;

static io_service_t             rootDomainService = IO_OBJECT_NULL;
static IOPMCapabilityBits       gCurrentCapabilityBits = kIOPMCapabilityCPU | kIOPMCapabilityDisk
                                    | kIOPMCapabilityNetwork | kIOPMCapabilityAudio | kIOPMCapabilityVideo;

/* The capabilities NextBSD's root domain gives a woken system. */
#define kNextBSDFullWakeCapabilities \
                    (kIOPMSystemCapabilityCPU | kIOPMSystemCapabilityGraphics \
                    | kIOPMSystemCapabilityAudio | kIOPMSystemCapabilityNetwork)

/************************************************************************************/

/* Bookkeeping structs */

/* PMResponseWrangler
 * While we have an outstanding notification, we have one of these guys sitting around
 *  waiting to handle the incoming responses.
 * We assert that only one PMResponseWrangler shall exist at a time - e.g. no more
 *  than one system state transition shall occur simultaneously.
 */
typedef struct {
    CFMutableArrayRef       awaitingResponses;
    dispatch_source_t       awaitingResponsesTimeout;
    CFAbsoluteTime          allRepliedTime;
    long                    kernelAcknowledgementID;
    int                     notificationType;
    int                     awaitingResponsesCount;
    int                     awaitResponsesTimeoutSeconds;
    uint16_t                generationCount;
} PMResponseWrangler;


/* PMConnection - one tracker corresponds to one PMConnection
 * in an application.
 *
 * responseHandler - Should be NULL unless this connection has outstanding
 *      notifications to reply to.
 */
typedef struct {
    mach_port_t             notifyPort;
    PMResponseWrangler      *responseHandler;
    CFStringRef             callerName;
    uint32_t                uniqueID;
    pid_t                   callerPID;
    IOPMCapabilityBits      interestsBits;
    bool                    notifyEnable;
    int                     timeoutCnt;
    dispatch_source_t       procExit;
} PMConnection;


/* PMResponse
 * represents one outstanding notification acknowledgement
 */
typedef struct {
    PMConnection            *connection;
    PMResponseWrangler      *myResponseWrangler;
    IOPMConnectionMessageToken  token;
    CFAbsoluteTime          repliedWhen;
    CFAbsoluteTime          notifiedWhen;
    CFAbsoluteTime          maintenanceRequested;
    CFStringRef             clientInfoString;
    int                     notificationType;
    bool                    replied;
    bool                    timedout;
} PMResponse;

/************************************************************************************/

/* Internal methods */

#define VALID_DATE(x) (x!=0.0)

static IOReturn createConnectionWithID(
                    PMConnection **);

static PMConnection *connectionForID(
                    uint32_t findMe);

static CFArrayRef createArrayOfConnectionsWithInterest(
                    int interestBits);

static PMResponseWrangler *connectionFireNotification(
                    int notificationType,
                    long kernelAcknowledgementID);

static void _sendMachMessage(
                    mach_port_t port,
                    mach_msg_id_t msg_id,
                    uint32_t payload_bits,
                    uint32_t payload_messagetoken);

static void checkResponses(PMResponseWrangler *wrangler);

static void PMScheduleWakeEventChooseBest(CFAbsoluteTime scheduleTime, wakeType_e type);

static void cleanClientResponses(PMResponseWrangler *wrangler, bool timeout);

static void cleanupConnection(PMConnection *reap);

static void cleanupResponseWrangler(PMResponseWrangler *reap);

static void setSystemSleepStateTracking(IOPMCapabilityBits);

static void PMConnectionPowerCallBack(
    void            *port,
    io_service_t    rootdomainservice,
    natural_t       messageType,
    void            *messageArgument);

static void sendNoRespNotification( int interestBitsNotify );
static bool PMConnectionHandleDeadName(uint32_t connection_id);
static bool checkResponses_ScheduleWakeEvents(PMResponseWrangler *wrangler);

/************************************************************************************/

/* CFArrayRef support structures */

static Boolean _CFArrayConnectionEquality(const void *value1, const void *value2)
{
    const PMConnection *v1 = (const PMConnection *)value1;
    const PMConnection *v2 = (const PMConnection *)value2;
    return (v1->uniqueID == v2->uniqueID);
}
static CFArrayCallBacks _CFArrayConnectionCallBacks =
                        { 0, NULL, NULL, NULL, _CFArrayConnectionEquality };
static CFArrayCallBacks _CFArrayVanillaCallBacks =
                        { 0, NULL, NULL, NULL, NULL };

/************************************************************************************/

/* Globals */

static CFMutableArrayRef        gConnections = NULL;

static uint32_t                 globalConnectionIDTally = 0;

static io_connect_t             gRootDomainConnect = IO_OBJECT_NULL;

static PMResponseWrangler *     gLastResponseWrangler = NULL;
static uint16_t                 gWranglerGenerationCount = 0;

bool                            gMachineStateRevertible = true;

/************************************************************************************/

#define IS_DARK_CAPABILITIES(x) \
                    (BIT_IS_SET(x, kIOPMSystemCapabilityCPU) \
                    && BIT_IS_NOT_SET(x, kIOPMSystemCapabilityGraphics|kIOPMSystemCapabilityAudio))

#define SYSTEM_WILL_WAKE(x) \
                    ( BIT_IS_SET(capArgs->changeFlags, kIOPMSystemCapabilityWillChange) \
                    && IS_CAP_GAIN(capArgs, kIOPMSystemCapabilityCPU) )

#define SYSTEM_DID_WAKE(x) \
                    ( BIT_IS_SET(capArgs->changeFlags, kIOPMSystemCapabilityDidChange)  \
                        && (IS_CAP_GAIN(capArgs, kIOPMSystemCapabilityCPU) \
                        || IS_CAP_GAIN(capArgs, kIOPMSystemCapabilityGraphics) ) )

#define SYSTEM_WILL_SLEEP_TO_S0(x) \
                    ((CAPABILITY_BIT_CHANGED(x->fromCapabilities, x->toCapabilities, kIOPMSystemCapabilityCPU)) \
                    && BIT_IS_SET(x->changeFlags, kIOPMSystemCapabilityWillChange) \
                    && BIT_IS_NOT_SET(x->toCapabilities, kIOPMSystemCapabilityCPU))

/*
 * PMConnection.h's IS_EMERGENCY_SLEEP() also counts a raised thermal level
 * (getSystemThermalState(), PrivateLib.m), which NextBSD does not track yet.
 */
#define IS_EMERGENCY_SLEEP_REASON(reason) \
                (CFEqual((reason), CFSTR(kIOPMLowPowerSleepKey)) ||  \
                CFEqual((reason), CFSTR(kIOPMThermalEmergencySleepKey)))

/************************************************************************************/

/* ---- PrivateLib.m: the sleep and wake reasons --------------------------- */

/*
 * Apple keeps these in PrivateLib.m's `reasons`; only PMConnection.m reads
 * them, so they live here until PrivateLib.m builds. The root domain sets
 * "Last Sleep Reason" before it sends kIOMessageSystemWillSleep, and
 * "Wake Reason" / "Wake Type" before kIOMessageSystemWillPowerOn.
 */
static struct {
    CFStringRef     sleepReason;
    CFStringRef     platformWakeReason;
    CFStringRef     platformWakeType;
} reasons = { NULL, NULL, NULL };

static CFStringRef _updateSleepReason(void)
{
    io_service_t    iopm_rootdomain_ref = getRootDomain();

    if (reasons.sleepReason) CFRelease(reasons.sleepReason);

    reasons.sleepReason = IORegistryEntryCreateCFProperty(
                            iopm_rootdomain_ref,
                            CFSTR("Last Sleep Reason"),
                            kCFAllocatorDefault, 0);

    if (!isA_CFString(reasons.sleepReason)) {
        if (reasons.sleepReason) CFRelease(reasons.sleepReason);
        reasons.sleepReason = CFSTR("");
    }

    return reasons.sleepReason;
}

static CFStringRef _getSleepReason(void)
{
    if (!reasons.sleepReason)
        reasons.sleepReason = CFSTR("");
    return (reasons.sleepReason);
}

static void _resetWakeReason(void)
{
    if (reasons.platformWakeReason)         CFRelease(reasons.platformWakeReason);
    if (reasons.platformWakeType)           CFRelease(reasons.platformWakeType);

    reasons.platformWakeReason              = CFSTR("");
    reasons.platformWakeType                = CFSTR("");
}

static void _updateWakeReason
    (CFStringRef *wakeReason, CFStringRef *wakeType)
{
    _resetWakeReason();

    // This property may not exist on all platforms.
    reasons.platformWakeReason = _copyRootDomainProperty(CFSTR(kIOPMRootDomainWakeReasonKey));
    if (!isA_CFString(reasons.platformWakeReason)) {
        if (reasons.platformWakeReason) CFRelease(reasons.platformWakeReason);
        reasons.platformWakeReason = CFSTR("");
    }

    reasons.platformWakeType = _copyRootDomainProperty(CFSTR(kIOPMRootDomainWakeTypeKey));
    if (!isA_CFString(reasons.platformWakeType)) {
        if (reasons.platformWakeType) CFRelease(reasons.platformWakeType);
        reasons.platformWakeType = CFSTR("");
    }

    /* Apple: claimed wake events (IOPMDriverWakeEvents) and messagetracer. */
    *wakeReason = reasons.platformWakeReason;
    *wakeType = reasons.platformWakeType;
}

/************************************************************************************/

/*
 * The root domain's app-power messages as the capability changes xnu sends
 * pmconfigd on "IOPMSystemCapabilityInterest" (see the top of this file).
 */
static void PMConnectionAppPowerCallBack(
    void            *port,
    io_service_t    rootdomainservice,
    natural_t       inMessageType,
    void            *messageData)
{
    struct IOPMSystemCapabilityChangeParameters capArgs;

    bzero(&capArgs, sizeof(capArgs));

    switch (inMessageType) {
    case kIOMessageSystemWillSleep:
        capArgs.notifyRef = (uint32_t)(uintptr_t)messageData;
        capArgs.changeFlags = kIOPMSystemCapabilityWillChange;
        capArgs.fromCapabilities = kNextBSDFullWakeCapabilities;
        capArgs.toCapabilities = 0;
        break;

    case kIOMessageSystemWillPowerOn:
        capArgs.changeFlags = kIOPMSystemCapabilityWillChange;
        capArgs.fromCapabilities = 0;
        capArgs.toCapabilities = kNextBSDFullWakeCapabilities;
        break;

    case kIOMessageSystemHasPoweredOn:
        capArgs.changeFlags = kIOPMSystemCapabilityDidChange;
        capArgs.fromCapabilities = 0;
        capArgs.toCapabilities = kNextBSDFullWakeCapabilities;
        break;

    default:
        /* kIOMessageCanSystemSleep, kIOMessageSystemWillNotSleep */
        PMConnectionPowerCallBack(port, rootdomainservice, inMessageType, messageData);
        return;
    }

    PMConnectionPowerCallBack(port, rootdomainservice,
                              kIOMessageSystemCapabilityChange, &capArgs);
}

/*
 * PMConnection_prime
 */
__private_extern__
void PMConnection_prime(void)
{
    io_object_t                 sleepWakeCallbackHandle = IO_OBJECT_NULL;
    IONotificationPortRef       notify = NULL;
    kern_return_t               kr = 0;
    char                        *errorString = NULL;
    CFMutableDictionaryRef      rdProps = NULL;
    CFNumberRef                 caps_cf = NULL;

    sleepwake_log = os_log_create(PM_LOG_SYSTEM, SLEEPWAKE_LOG);

    gConnections = CFArrayCreateMutable(kCFAllocatorDefault, 100, &_CFArrayConnectionCallBacks);

    // Find it
    rootDomainService = getRootDomain();
    if (IO_OBJECT_NULL == rootDomainService) {
        errorString = "Could not find IOPMrootDomain";
        goto error;
    }

    // Open it
    kr = IOServiceOpen(rootDomainService, mach_task_self(), 0, &gRootDomainConnect);
    if (KERN_SUCCESS != kr) {
        errorString = "Could not open IOPMrootDomain";
        goto error;
    }

    notify = IONotificationPortCreate(MACH_PORT_NULL);
    if (!notify) {
        errorString = "Could not create IONotificationPort";
        goto error;
    }

    // Register for sleep wake notifications
    kr = IOServiceAddInterestNotification(notify, rootDomainService, kIOAppPowerStateInterest,
                                          (IOServiceInterestCallback) PMConnectionAppPowerCallBack, NULL,
                                          &sleepWakeCallbackHandle);
    if (KERN_SUCCESS != kr) {
        IONotificationPortDestroy(notify);
        errorString = "Could not add interest notification kIOPMAppPowerStateInterest";
        goto error;
    }

    IONotificationPortSetDispatchQueue(notify, _getPMMainQueue());

    /* Apple: setInitialSleepPreventersCount() (PMAssertions.c). */

    IORegistryEntryCreateCFProperties(rootDomainService, &rdProps, 0, 0);
    if (isA_CFDictionary(rdProps)) {
        caps_cf = CFDictionaryGetValue(rdProps, CFSTR(kIOPMSystemCapabilitiesKey));
        if (isA_CFNumber(caps_cf)) {
            uint32_t caps;

            CFNumberGetValue(caps_cf, kCFNumberIntType, &caps);
            if (caps & (kIOPMSystemCapabilityGraphics|kIOPMSystemCapabilityAudio)) {
                gPowerState = kFullWakeState;
            }
            else if (caps & kIOPMSystemCapabilityCPU) {
                gPowerState = kDarkWakeState;
            }
            else {
                gPowerState = kSleepState;
            }
        }
    }
    if (rdProps) {
        CFRelease(rdProps);
    }

    return;

error:
    ERROR_LOG("PowerManagement: unable to register with kernel power management. %s %s\n",
                    errorString ? "Reason = : ":"", errorString ? errorString:"unknown");
    return;
}


/*****************************************************************************/

/* M I G */

/*
 * Create a PM Connection
 *   - assign a globally unique ID
 *   - find the remote mach port
 *   - get an invalidation notification on the remote callback
 *
 */
kern_return_t _io_pm_connection_create (
    mach_port_t     server __unused,
    audit_token_t   token,
    string_t        name,
    int             interests,
    uint32_t        *connection_id,
    int             *return_code
)
{
    PMConnection         *newConnection = NULL;
    *connection_id = 0;
    // Allocate: create a new PMConnection type
    createConnectionWithID(&newConnection);
    if (!newConnection) {
        *return_code = kIOReturnError;
        goto exit;
    }

    audit_token_to_au32(token, NULL, NULL, NULL, NULL, NULL, &newConnection->callerPID, NULL, NULL);

    // Save caller name for logging
    if (name && strlen(name)) {
        newConnection->callerName = CFStringCreateWithCString(0, name, kCFStringEncodingUTF8);
    }

    newConnection->interestsBits = interests;
    *connection_id = newConnection->uniqueID;
    *return_code = kIOReturnSuccess;

exit:

    return KERN_SUCCESS;
}


/*****************************************************************************/

 kern_return_t _io_pm_connection_schedule_notification(
    mach_port_t     server __unused,
    audit_token_t   token,
    uint32_t        connection_id,
    mach_port_t     notify_port_in,
    int             disable,
    int             *return_code)
{
    PMConnection        *connection = NULL;
    pid_t               callerPID = -1;

    if (MACH_PORT_NULL == notify_port_in || NULL == return_code) {
        if (return_code) *return_code = kIOReturnBadArgument;
        goto exit;
    }

    *return_code = kIOReturnError;

    audit_token_to_au32(token, NULL, NULL, NULL, NULL, NULL, &callerPID, NULL, NULL);
    connection = connectionForID(connection_id);
    if (!connection) {
        ERROR_LOG("Failed to find the connection for ID %d from pid %d\n", connection_id, callerPID);
        return kIOReturnNotFound;
    }
    if (connection->callerPID != callerPID) {
        ERROR_LOG("Notification schedule request from unexepcted pid %d for connection %d. Expected pid:%d\n",
                callerPID, connection_id, connection->callerPID);

        return kIOReturnNotFound;
    }

    connection->notifyEnable = (disable == 0);

    if (!disable && (MACH_PORT_NULL == connection->notifyPort)) {
        dispatch_source_t src;
        connection->notifyPort = notify_port_in;

        src = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, callerPID,
                                            DISPATCH_PROC_EXIT, _getPMMainQueue());
        if (src != NULL) {
            dispatch_source_set_event_handler(src, ^{
                    PMConnectionHandleDeadName(connection_id);
                    });
            dispatch_source_set_cancel_handler(src, ^{dispatch_release(src);});
            dispatch_resume(src);

            // Create dispatch source in local variable 'src' to force the cancel handler
            // block to make a copy of the address
            connection->procExit = src;
        }
        else {
            ERROR_LOG("Failed to create dispatch src to cleanup connection %d from pid %d\n", connection_id, callerPID);
        }


    } else {
        mach_port_deallocate(mach_task_self(), notify_port_in);
    }

    *return_code = kIOReturnSuccess;
exit:
    return KERN_SUCCESS;
}

/*****************************************************************************/

kern_return_t _io_pm_connection_release
(
    mach_port_t server __unused,
    uint32_t connection_id,
    int *return_code
)
{
    PMConnection        *cleanMeUp = NULL;

    cleanMeUp = connectionForID(connection_id);

    if (cleanMeUp) {
        cleanupConnection(cleanMeUp);
        if (return_code)
            *return_code = kIOReturnSuccess;
    } else {
        if (return_code)
            *return_code = kIOReturnNotFound;
    }

    return KERN_SUCCESS;
}

/*****************************************************************************/

/*
 * _io_pm_acknowledge_event_findOutstandingResponseForToken
 * Helper function to improve readability of connection_acknowledge_event
 */

static PMResponse *_io_pm_acknowledge_event_findOutstandingResponseForToken(PMConnection *connection, int token)
{
    CFMutableArrayRef   responsesTrackingList = NULL;
    long                responsesCount = 0;
    PMResponse          *checkResponse = NULL;
    PMResponse          *foundResponse = NULL;
    long                i;

    if (!connection
        || !connection->responseHandler
        || !(responsesTrackingList = connection->responseHandler->awaitingResponses))
    {
        return NULL;
    }

    responsesCount = CFArrayGetCount(responsesTrackingList);

    for (i=0; i<responsesCount; i++)
    {
        checkResponse = (PMResponse *)CFArrayGetValueAtIndex(responsesTrackingList, i);
        if (checkResponse && ((IOPMConnectionMessageToken)token == checkResponse->token)) {
            foundResponse = checkResponse;
            break;
        }
    }

    if (foundResponse && !foundResponse->connection)
    {
        ERROR_LOG("Trying to acknowledge event with a NULL connection. ClientInfo %@, Replied: %d\n", foundResponse->clientInfoString, foundResponse->replied);
        return NULL;
    }

    return foundResponse;
}

/*
 * _io_pm_connection_acknowledge_event_unpack_payload
 * Helper function to improve readability of connection_acknowledge_event
 */
static CFDictionaryRef _io_pm_connection_acknowledge_event_unpack_payload(
    vm_offset_t         ptr,
    mach_msg_type_number_t   len)
{
    CFDataRef           optionsAsData = NULL;
    CFPropertyListRef   optionsUnzipped = NULL;
    CFDictionaryRef     ackOptionsDict = NULL;

    if ( ptr != 0 && len != 0)
    {
        optionsAsData = CFDataCreateWithBytesNoCopy(0, (const uint8_t *)ptr, len, kCFAllocatorNull);
        if (optionsAsData)
        {
            optionsUnzipped = CFPropertyListCreateWithData(0, optionsAsData, kCFPropertyListImmutable, NULL, NULL);
            ackOptionsDict = (CFDictionaryRef) isA_CFDictionary(optionsUnzipped);
            if (optionsUnzipped && !ackOptionsDict) {
                CFRelease(optionsUnzipped);
            }
            CFRelease(optionsAsData);
        }
    }

    return ackOptionsDict;
}

/*
 * Whether the system runs on external power, from the power-source state
 * pmconfigd itself publishes on kIOPSNotifyPercentChange
 * (BatteryTimeRemaining, nextbsd-userland#227). Apple asks
 * _getPowerSource(); a system with no battery data is on AC there too.
 */
#define kPMPercentChangeNotify      "com.apple.system.powersources.percent"
#define kPMPercentExternalBit       (1 << 16)
#define kPMPercentValidBit          (1 << 19)

static bool _onACPower(void)
{
    static int  token = 0;
    uint64_t    state = 0;

    if (token == 0
        && notify_register_check(kPMPercentChangeNotify, &token) != NOTIFY_STATUS_OK)
    {
        token = 0;
        return true;
    }
    if (notify_get_state(token, &state) != NOTIFY_STATUS_OK
        || !(state & kPMPercentValidBit))
    {
        return true;
    }
    return (state & kPMPercentExternalBit) != 0;
}

kern_return_t _io_pm_connection_acknowledge_event
(
 mach_port_t server __unused,
 audit_token_t   token __unused,
 uint32_t connection_id,
 int messageToken,
 vm_offset_t options_ptr,
 mach_msg_type_number_t options_len,
 int *return_code
 )
{
    PMConnection        *connection = connectionForID(connection_id);
    PMResponse          *foundResponse = NULL;

    // Check options dictionary
    CFDictionaryRef     ackOptionsDict = NULL;
    CFDateRef           requestDate = NULL;

    if (messageToken == 0)
    {
       /*
        * This response is for a message for which response is not expected.
        * There is no record mainatained for these messages.
        */
       *return_code = kIOReturnSuccess;
       goto exit;
    }

    if (!(foundResponse = _io_pm_acknowledge_event_findOutstandingResponseForToken(connection, messageToken))) {
        *return_code = kIOReturnNotFound;
        goto exit;
    }

    *return_code = kIOReturnSuccess;
    foundResponse->repliedWhen = CFAbsoluteTimeGetCurrent();
    foundResponse->replied = true;

    // Unpack the passed-in options data structure
    if ((ackOptionsDict = _io_pm_connection_acknowledge_event_unpack_payload(options_ptr, options_len)))
    {
        if (!CFDictionaryContainsKey(ackOptionsDict, kIOPMAckClientInfoKey) &&
            !CFDictionaryContainsKey(ackOptionsDict, kIOPMAckClientInfoBGTaskKey) &&
            !CFDictionaryContainsKey(ackOptionsDict, kIOPMAckClientInfoAppRefreshKey)) {
            if (foundResponse->connection->callerPID && foundResponse->connection->callerName) {
                ERROR_LOG("IOPMConnectionAcknowledgeEventWithOptions: No client info key present for %d:%@\n",
                    foundResponse->connection->callerPID, foundResponse->connection->callerName);
            } else {
                ERROR_LOG("IOPMConnectionAcknowledgeEventWithOptions: No client info\n");
            }
            *return_code = kIOReturnBadArgument;
            goto skip;
        }

        foundResponse->clientInfoString = CFDictionaryGetValue(ackOptionsDict, kIOPMAckClientInfoKey);
        if (foundResponse->clientInfoString) {
            CFRetain(foundResponse->clientInfoString);
        }

        /*
         * mDNSResponder requests a maintenance wake for DHCP renewal.
         *  - Schedules on AC
         *  - Schedules on battery only if TCPKA is active
         *
         * kIOPMAckDHCPRenewWakeDate
         *
         * Apple: TCP keep-alive is not ported, so battery never qualifies.
         */
        requestDate = isA_CFDate(CFDictionaryGetValue(ackOptionsDict, kIOPMAckDHCPRenewWakeDate));

        if (requestDate) {
            if (_onACPower())
            {
                foundResponse->maintenanceRequested = CFDateGetAbsoluteTime(requestDate);
            }
        }
        else {
            /*
             * Caller requests a maintenance wake. These schedule on AC only.
             *
             * kIOPMAckWakeDate
             * kIOPMAckNetworkMaintenanceWakeDate
             */
            requestDate = isA_CFDate(CFDictionaryGetValue(ackOptionsDict, kIOPMAckWakeDate));
            if (!requestDate) {
                requestDate = isA_CFDate(CFDictionaryGetValue(ackOptionsDict, kIOPMAckNetworkMaintenanceWakeDate));
            }

            if (requestDate
                && _onACPower())
            {
                foundResponse->maintenanceRequested = CFDateGetAbsoluteTime(requestDate);
            }

            /*
             * Caller requests a maintenance wake. These schedule on AC and Battery.
             * kIOPMAckSUWakeDate
             */
            if (!requestDate) {
                requestDate = isA_CFDate(CFDictionaryGetValue(ackOptionsDict, kIOPMAckSUWakeDate));
                if (requestDate) {
                    foundResponse->maintenanceRequested = CFDateGetAbsoluteTime(requestDate);                }
            }

        }

        /*
         * Apple: kIOPMAckBackgroundTaskWakeDate and kIOPMAckAppRefreshWakeDate
         * schedule PowerNap work in a dark wake, which NextBSD does not have.
         */
skip:
        CFRelease(ackOptionsDict);
    }

    checkResponses(connection->responseHandler);


exit:
    vm_deallocate(mach_task_self(), options_ptr, options_len);

    return KERN_SUCCESS;
}

kern_return_t _io_pm_get_capability_bits(
        mach_port_t     server __unused,
        audit_token_t   token __unused,
        uint32_t    *capBits,
        int         *return_code )
{

    *capBits = gCurrentCapabilityBits;
    *return_code = kIOReturnSuccess;
    return KERN_SUCCESS;
}

/************************************************************************************/

#pragma mark -
#pragma mark Connection

static void cleanupConnection(PMConnection *reap)
{
    PMResponseWrangler      *responseWrangler = NULL;
    PMResponse              *openResponse = NULL;
    CFIndex                 i, allResponsesCount = 0;
    CFIndex                 index;
    CFRange                 connectionsRange =
                                CFRangeMake(0, CFArrayGetCount(gConnections));

    if (MACH_PORT_NULL != reap->notifyPort)
    {
        // Release the send right on reap->notifyPort that we obtained
        // when we received it as an argument to _io_pm_connection_schedule_notification.
        mach_port_deallocate(mach_task_self(), reap->notifyPort);
        reap->notifyPort = MACH_PORT_NULL;
    }

    if (reap->procExit) {
        dispatch_source_cancel(reap->procExit);
    }
    if (reap->callerName) {
        CFRelease(reap->callerName);
        reap->callerName = NULL;
    }

    responseWrangler = reap->responseHandler;
    if (responseWrangler && responseWrangler->awaitingResponses)
    {
        allResponsesCount = CFArrayGetCount(responseWrangler->awaitingResponses);

        for (i=0; i<allResponsesCount; i++)
        {
            openResponse = (PMResponse *)CFArrayGetValueAtIndex(
                                    responseWrangler->awaitingResponses, i);

            if (openResponse && (openResponse->connection == reap)) {
                openResponse->connection    = NULL;
                openResponse->replied       = true;
                openResponse->timedout      = true;
                break;
            }
        }

        // Let the response wrangler finish handling the outstanding event
        // now that we've zeroed out any of our pending responses for this dead client.
        checkResponses(responseWrangler);
    }

    // Remove our struct from gConnections
    index = CFArrayGetFirstIndexOfValue(gConnections, connectionsRange, (void *)reap);

    if (kCFNotFound != index) {
        CFArrayRemoveValueAtIndex(gConnections, index);
    }

    free(reap);

    return;
}

/*****************************************************************************/

static void cleanupResponseWrangler(PMResponseWrangler *reap)
{
    PMConnection    *one_connection;
    CFIndex         i,  connectionsCount;
    CFIndex         responseCount;

    if (!reap || !gConnections)
        return;

    // Loop through all connections. If any connections are referring to
    // responseWrangler in their tracking structs, zero that out.
    connectionsCount = CFArrayGetCount(gConnections);
    for (i=0; i<connectionsCount; i++)
    {
        one_connection = (PMConnection *)CFArrayGetValueAtIndex(gConnections, i);
        if (reap == one_connection->responseHandler)
        {
            // Zero out this reference before it points to a free'd pointer
            one_connection->responseHandler = NULL;
        }
    }


    // Loop responses, destroy responses
    if (reap->awaitingResponses)
    {
        responseCount = CFArrayGetCount(reap->awaitingResponses);
        if (responseCount > 0)
        {
            for (i=0; i<responseCount; i++)
            {
                PMResponse  *purgeMe = (PMResponse *)CFArrayGetValueAtIndex(reap->awaitingResponses, i);

                if (purgeMe->clientInfoString)
                    CFRelease(purgeMe->clientInfoString);

                free(purgeMe);
            }
        }
        CFRelease(reap->awaitingResponses);

        reap->awaitingResponses = NULL;
    }

    // Invalidate the pointer to the in-flight response wrangler.
    if (gLastResponseWrangler == reap) {
        gLastResponseWrangler = NULL;
    }

    free(reap);
}

/*****************************************************************************/

static bool PMConnectionHandleDeadName(uint32_t connection_id)
{
    PMConnection    *the_connection = NULL;

    the_connection = connectionForID(connection_id);

    if (the_connection) {
        cleanupConnection(the_connection);
        return true;
    } else {
        return false;
    }
}

/*****************************************************************************/

static void setSystemSleepStateTracking(IOPMCapabilityBits capables)
{
    CFStringRef         key = NULL;
    CFNumberRef         capablesNum = NULL;

    /* PMStore.c (D3); see the top of this file. */
    if (!PMStoreSetValue)
        return;

    /* NextBSD's SystemConfiguration has no SCDynamicStoreKeyCreate(). */
    key = CFStringCreateWithFormat(0, NULL, CFSTR("%@%@"),
                        kSCDynamicStoreDomainState,
                        CFSTR(kIOPMSystemPowerCapabilitiesKeySuffix));
    if (!key)
        return;

    capablesNum = CFNumberCreate(0, kCFNumberIntType, &capables);

    if (capablesNum) {
        PMStoreSetValue(key, capablesNum);
        CFRelease(capablesNum);
    }

    CFRelease(key);
}

/*****************************************************************************/

__private_extern__ bool isA_SleepState(void)
{
    return (gPowerState & kSleepState) ? true : false;
}

__private_extern__ bool isA_DarkWakeState(void)
{
    return (gPowerState & kDarkWakeState) ? true : false;
}

__private_extern__ bool isA_FullWake(void)
{
    return (gPowerState & kFullWakeState) ? true : false;
}

__private_extern__ bool isCapabilityChangeDone(void)
{
    return gCapabilityChangeDone;
}

/*****************************************************************************/

static void handleCanSleepMsg(void *messageData, CFStringRef sleepReason)
{
    bool allow_sleep = true;
    bool idle_sleep = false;

    idle_sleep = (CFStringCompare(sleepReason, CFSTR(kIOPMIdleSleepKey), 0) == kCFCompareEqualTo);
    /*
     * Apple: TTYKeepAwakeConsiderAssertion() (active ssh sessions) and
     * checkForActivesByType(kDeclareSystemActivityType) (PMAssertions.c),
     * nextbsd-userland#228.
     */
    if (allow_sleep && idle_sleep) {
        if (!isA_DarkWakeState()) {
            // If idle sleep and transition is from full wake -> darkwake
            // check if sleep has to be prevented for upcoming sleep request
            allow_sleep = (checkPendingWakeReqs(CHECK_UPCOMING|PREVENT_PURGING) ? false : true);
        }
    }

    if (allow_sleep) {
        lastcall_ts = mach_absolute_time();
        gMachineStateRevertible = false;
        IOAllowPowerChange(gRootDomainConnect, (long)messageData);
    }
    else
        IOCancelPowerChange(gRootDomainConnect, (long)messageData);
}

/*
 * NextBSD: the root domain only asks kIOMessageCanSystemSleep for an idle
 * sleep, and sets "Last Sleep Reason" only once the sleep goes ahead, so a
 * CanSystemSleep is always "Idle Sleep" (xnu's kIOPMSleepReasonIdle).
 */
static CFStringRef _canSleepReason(void)
{
    if (reasons.sleepReason) CFRelease(reasons.sleepReason);
    reasons.sleepReason = CFSTR(kIOPMIdleSleepKey);
    return reasons.sleepReason;
}

static void PMConnectionPowerCallBack(
    void            *port __unused,
    io_service_t    rootdomainservice __unused,
    natural_t       inMessageType,
    void            *messageData)
{
    PMResponseWrangler        *responseController = NULL;
    IOPMCapabilityBits        deliverCapabilityBits = 0;
    const struct        IOPMSystemCapabilityChangeParameters *capArgs;
    CFStringRef sleepReason = _getSleepReason();
    static bool slp_logd = false;
    CFStringRef wakeType = CFSTR("");
    CFStringRef wakeReason = CFSTR("");

    if (inMessageType == kIOMessageCanSystemSleep)
    {
        sleepReason = _canSleepReason();
        if (IS_EMERGENCY_SLEEP_REASON(sleepReason)) {
            gEmergencySleep = true;
        }
        handleCanSleepMsg(messageData, sleepReason);
        return;
    }
    else if (inMessageType == kIOMessageSystemWillNotSleep)
    {
        gMachineStateRevertible = true;
        checkPendingWakeReqs(ALLOW_PURGING);
        gEmergencySleep = false;
        INFO_LOG("Sleep cancelled\n");
        return;
    }
    else if (kIOMessageSystemCapabilityChange != inMessageType)
    {
        ERROR_LOG("Unhandled inMessageType (%x)\n", inMessageType);
        return;
    }

    capArgs = (typeof(capArgs)) messageData;

    AutoWakeCapabilitiesNotification(capArgs);
    /*
     * Apple: ClockSleepWakeNotification() (pmconfigd.m) and
     * PMSettingsCapabilityChangeNotification() (PMSettings.m).
     */

    if (SYSTEM_WILL_SLEEP_TO_S0(capArgs))
    {
        sleepReason = _updateSleepReason();
        if (IS_EMERGENCY_SLEEP_REASON(sleepReason)) {
            gEmergencySleep = true;
        }

        // clear all wake time bookkeeping
        gCapabilityChangeDone = false;

        // Send an async notify out - clients include SCNetworkReachability API's; no ack expected
        setSystemSleepStateTracking(0);
        notify_post(kIOPMSystemPowerStateNotify);

        _resetWakeReason();

        /*
         * Apple: PowerNap scheduling, TCP keep-alive, the standby timer,
         * flight data recorder, ADS and silent running.
         */

        // Clear gPowerState and set it to kSleepState
        gPowerState &= ~kPowerStateMask;
        gPowerState |= kSleepState;


        /* We must acknowledge this sleep event within 30 second timeout,
         *      once clients ack via IOPMConnectionAcknowledgeEvent().
         * Our processing will pick-up again in our handler
         *      _io_pm_connection_acknowledge_event
         */
        INFO_LOG("Sleep: %@\n", sleepReason);
        slp_logd = true;

        responseController = connectionFireNotification(_kSleepStateBits, (long)capArgs->notifyRef);

        if (!responseController) {
            // We have zero clients. Acknowledge immediately.

            checkResponses_ScheduleWakeEvents(NULL);
            IOAllowPowerChange(gRootDomainConnect, (long)capArgs->notifyRef);
        }

        return;
    } else if (SYSTEM_DID_WAKE(capArgs))
    {
        _updateWakeReason(&wakeReason, &wakeType);

        gMachineStateRevertible = true;

        if (BatteryTimeRemainingWakeNotification)
            BatteryTimeRemainingWakeNotification();

        if (IS_CAP_GAIN(capArgs, kIOPMSystemCapabilityGraphics))
        {
            // e.g. System has powered into full wake
            gPowerState &= ~kPowerStateMask;
            gPowerState |= kFullWakeState;

            deliverCapabilityBits |=
                    kIOPMCapabilityCPU | kIOPMCapabilityDisk
                    | kIOPMCapabilityNetwork | kIOPMCapabilityAudio | kIOPMCapabilityVideo;

            if (slp_logd) {
                INFO_LOG("Wake: %@ (%@)\n", wakeReason, wakeType);
            }
            slp_logd = false;

            gCapabilityChangeDone = true;
        }
        /*
         * Apple: a gain of CPU alone is a dark wake (PowerNap, maintenance,
         * sleep services), which NextBSD's root domain never reports.
         */

        // Send an async notify out - clients include SCNetworkReachability API's; no ack expected
        setSystemSleepStateTracking(deliverCapabilityBits);

        notify_post(kIOPMSystemPowerStateNotify);

        gCurrentCapabilityBits = deliverCapabilityBits;
        sendNoRespNotification(deliverCapabilityBits);

        if (capArgs->notifyRef)
            IOAllowPowerChange(gRootDomainConnect, (long)capArgs->notifyRef);

        /* Apple: SystemLoadSystemPowerStateHasChanged() and wake statistics. */
        return;
    }
    else if (SYSTEM_WILL_WAKE(capArgs) )
    {
        gEmergencySleep = false;
        gMachineStateRevertible = true;

        gPowerState = kDarkWakeState;

        deliverCapabilityBits = kIOPMEarlyWakeNotification |
              kIOPMCapabilityCPU | kIOPMCapabilityDisk | kIOPMCapabilityNetwork ;

        if (IS_CAP_GAIN(capArgs, kIOPMSystemCapabilityGraphics))
           deliverCapabilityBits |= kIOPMCapabilityVideo | kIOPMCapabilityAudio;

        sendNoRespNotification( deliverCapabilityBits );
    }

    if (capArgs->notifyRef)
        IOAllowPowerChange(gRootDomainConnect, capArgs->notifyRef);

}

__private_extern__ bool isEmergencySleep(void)
{
    return gEmergencySleep;
}

/************************************************************************************/

#pragma mark -
#pragma mark Responses

static PMResponseWrangler *connectionFireNotification(
    int interestBitsNotify,
    long kernelAcknowledgementID)
{
    int                     affectedBits = 0;
    CFArrayRef              interested = NULL;
    PMConnection            *connection = NULL;
    int                     interestedCount = 0;
    uint32_t                messageToken = 0;
    int                     calloutCount = 0;

    PMResponseWrangler      *responseWrangler = NULL;
    PMResponse              *awaitThis = NULL;

    /*
     * If a response wrangler is active, store the new notification on the
     * active wrangler. Then wait for its completion before firing the new
     * notification. Only the last notification is stored.
     */
    if (gLastResponseWrangler)
    {
        // rdar://problem/69725628
        // The previous response wrangler created by connectionFireNotification(),
        // due to an upcoming loss of CPU capability bit, is still waiting for
        // client responses. One scenario is when a dark->sleep transition is
        // aborted and there is a slow client that doesn't ack before the next
        // dark->sleep transition. When that happens, rather than stacking the
        // response wranglers which can lead to a delayed and out-of-order
        // notification that crosses those sent by sendNoRespNotification(),
        // just invalidate gLastResponseWrangler as though all clients have
        // already responded. Client responses for an invalidated wrangler are
        // rejected based on the rolling generation count in the message token.
        cleanClientResponses(gLastResponseWrangler, false);
    }

    gWranglerGenerationCount++;
    if (gWranglerGenerationCount == 0) {
        gWranglerGenerationCount = 1;
    }
    INFO_LOG("connectionFireNotification: 0x%x gen 0x%x\n", interestBitsNotify, gWranglerGenerationCount);
    // We only send state change notifications out to entities interested in the changing
    // bits, or interested in a subset of the changing bits.
    // Any client who is interested in a superset of the changing bits shall not receive
    // a notification.
    //      affectedBits & InterestedBits != 0
    //  and affectedBits & InterestedBits == InterestedBits

    affectedBits = interestBitsNotify ^ gCurrentCapabilityBits;

    gCurrentCapabilityBits = interestBitsNotify;

    interested = createArrayOfConnectionsWithInterest(affectedBits);
    if (!interested) {
        goto exit;
    }
    interestedCount = (int)CFArrayGetCount(interested);
    if (0 == interestedCount) {
        goto exit;
    }

    /* Allocate the response wrangler
     *
     * This object will be allocated & valid for the duration of
     *   sending out notifications & awaiting responses.
     */
    responseWrangler = calloc(1, sizeof(PMResponseWrangler));
    if (!responseWrangler) {
        goto exit;
    }
    responseWrangler->notificationType = interestBitsNotify;
    responseWrangler->awaitResponsesTimeoutSeconds = (int)kPMConnectionNotifyTimeoutDefault;
    responseWrangler->kernelAcknowledgementID = kernelAcknowledgementID;
    responseWrangler->generationCount = gWranglerGenerationCount;

    /*
     * We will track each notification we're sending out with an individual response.
     * Record that response in the "active response array" so we can group them
     * all later when they acknowledge, or fail to acknowledge.
     */
    responseWrangler->awaitingResponses =
                    CFArrayCreateMutable(kCFAllocatorDefault, interestedCount, &_CFArrayVanillaCallBacks);
    responseWrangler->awaitingResponsesCount = interestedCount;

    for (calloutCount=0; calloutCount<interestedCount; calloutCount++)
    {
        connection = (PMConnection *)CFArrayGetValueAtIndex(interested, calloutCount);

        if ((MACH_PORT_NULL == connection->notifyPort) ||
            (false == connection->notifyEnable)) {
            continue;
        }

        /* We generate a messagetoken here, which the notifiee must pass
         * back into us when the client acknowledges.
         * We note the token in the PMResponse struct.
         *
         * callCount is incremented by 1 to make sure messageToken is  not NULL
         */
        messageToken = (responseWrangler->generationCount << 16)
                            | ((calloutCount+1) & 0xFFFF);

        // Mark this connection with the responseWrangler that's awaiting its responses
        connection->responseHandler = responseWrangler;

        _sendMachMessage(connection->notifyPort,
                            0,
                            interestBitsNotify,
                            messageToken);

        /*
         * Track the response!
         */
        awaitThis = calloc(1, sizeof(PMResponse));
        if (!awaitThis) {
            goto exit;
        }

        awaitThis->token = messageToken;
        awaitThis->connection = connection;
        awaitThis->notificationType = interestBitsNotify;
        awaitThis->myResponseWrangler = responseWrangler;
        awaitThis->notifiedWhen = CFAbsoluteTimeGetCurrent();

        CFArrayAppendValue(responseWrangler->awaitingResponses, awaitThis);
    }

    // TODO: Set off a timer to fire in xx30xx seconds
    dispatch_source_t   timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _getPMMainQueue());
    dispatch_time_t     when = dispatch_time(DISPATCH_TIME_NOW, responseWrangler->awaitResponsesTimeoutSeconds * NSEC_PER_SEC);
    dispatch_source_set_timer(timer, when, DISPATCH_TIME_FOREVER, 0);
    dispatch_source_set_event_handler(timer, ^{
        cleanClientResponses(responseWrangler, true);
    });
    responseWrangler->awaitingResponsesTimeout = timer;
    dispatch_activate(timer);

exit:
    if (interested)
        CFRelease(interested);

    // Record the active wrangler in a global, then clear when reaped.
    if (responseWrangler)
        gLastResponseWrangler = responseWrangler;

    return responseWrangler;
}

/*****************************************************************************/
static void sendNoRespNotification( int interestBitsNotify )
{

    CFIndex                 i, count = 0;
    // Set messageToken to 0, to indicate that we are not interested in response
    uint32_t                messageToken = 0;
    PMConnection            *connection = NULL;

    INFO_LOG("sendNoRespNotification: 0x%x\n", interestBitsNotify);
    count = CFArrayGetCount(gConnections);

    for (i=0; i<count; i++)
    {
        connection = (PMConnection *)CFArrayGetValueAtIndex(gConnections, i);

        if ((connection->interestsBits & interestBitsNotify) == 0)
           continue;

        if ((MACH_PORT_NULL == connection->notifyPort) ||
            (false == connection->notifyEnable)) {
            continue;
        }

        _sendMachMessage(connection->notifyPort,
                            0,
                            interestBitsNotify,
                            messageToken);
    }


}

/*****************************************************************************/

static void cleanClientResponses(PMResponseWrangler *responseWrangler, bool timeout)
{
    PMResponse      *one_response = NULL;
    CFIndex         i, responsesCount = 0;

    if (!responseWrangler)
        return;

    INFO_LOG("cleanClientResponses: gen 0x%x timeout %d\n",
        responseWrangler->generationCount, timeout);
    dispatch_source_cancel(responseWrangler->awaitingResponsesTimeout);
    dispatch_release(responseWrangler->awaitingResponsesTimeout);
    responseWrangler->awaitingResponsesTimeout = NULL;

    // Iterate list of awaiting responses, and tattle on anyone who hasn't
    // acknowledged yet.
    // Artificially mark them as "replied", with their reason being "timed out"
    responsesCount = CFArrayGetCount(responseWrangler->awaitingResponses);
    for (i=0; i<responsesCount; i++)
    {
        one_response = (PMResponse *)CFArrayGetValueAtIndex(
                                        responseWrangler->awaitingResponses, i);
        if (!one_response)
            continue;
        if (one_response->replied)
            continue;

        // Caught a tardy reply
        one_response->replied = true;
        one_response->timedout = timeout;
        one_response->repliedWhen = CFAbsoluteTimeGetCurrent();

        if (timeout && one_response->connection) {
            ERROR_LOG("PMConnection client %@(%d) did not acknowledge within %d s\n",
                      one_response->connection->callerName ? one_response->connection->callerName : CFSTR(""),
                      one_response->connection->callerPID,
                      responseWrangler->awaitResponsesTimeoutSeconds);
        }
    }

    checkResponses(responseWrangler);
}

/*****************************************************************************/

#define kMsgPayloadCount    2

typedef struct {
    mach_msg_header_t   header;
    mach_msg_body_t     body;
    uint32_t            payload[kMsgPayloadCount];
} IOPMMessageStructure;

static void _sendMachMessage(
    mach_port_t         port,
    mach_msg_id_t       msg_id,
    uint32_t            payload_bits,
    uint32_t            payload_messagetoken)
{
    kern_return_t        status;
    IOPMMessageStructure msg;

    bzero(&msg, sizeof(msg));

    msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_size = sizeof(msg);
    msg.header.msgh_remote_port = port;
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id = msg_id;

    msg.body.msgh_descriptor_count = 0;

    msg.payload[0] = payload_bits;
    msg.payload[1] = payload_messagetoken;

    status = mach_msg(&msg.header,                  /* msg */
              MACH_SEND_MSG | MACH_SEND_TIMEOUT,    /* options */
              msg.header.msgh_size,                 /* send_size */
              0,                                    /* rcv_size */
              MACH_PORT_NULL,                       /* rcv_name */
              MACH_MSG_TIMEOUT_NONE,                /* timeout */
              MACH_PORT_NULL);                      /* notify */

    if (status == MACH_SEND_TIMED_OUT) {
        mach_msg_destroy(&msg.header);
    }

    if (status != MACH_MSG_SUCCESS)
    {
        // Pray for the lost message.
    }

    return;
}

static bool validScheduledDate(CFAbsoluteTime event)
{
    return VALID_DATE(event) && (event > CFAbsoluteTimeGetCurrent());
}

/*
 * Every client has answered (or the wrangler is NULL, no clients): pick the
 * earliest wake anyone asked for and hand it to the RTC, as Apple does at
 * every sleep. Apple also weighs PowerNap, sleep-service and timer-plugin
 * requests and auto power-off, none of which NextBSD has.
 */
static bool checkResponses_ScheduleWakeEvents(PMResponseWrangler *wrangler)
{
    CFIndex                 i = 0;
    CFIndex                 responsesCount          = 0;
    bool                    complete                = true;
    PMResponse              *oneResponse            = NULL;
    CFAbsoluteTime          userWake = 0.0;
    CFAbsoluteTime          earliestWake = kCFAbsoluteTimeIntervalSince1904; // Invalid value
    wakeType_e              type = kChooseWakeTypeCount; // Invalid value

    if (wrangler) {
        responsesCount = CFArrayGetCount(wrangler->awaitingResponses);
    }

    for (i=0; i<responsesCount; i++)
    {
        oneResponse = (PMResponse *)CFArrayGetValueAtIndex(wrangler->awaitingResponses, i);

        if (!oneResponse->replied) {
            complete = false;
            break;
        }

        if (BIT_IS_SET(wrangler->notificationType, kIOPMSystemCapabilityCPU)) {
            // Don't need to look at wake requests if system is not going to sleep
            continue;
        }

        if (oneResponse->connection == NULL) {
            // Requesting process has died. Ignore its request
            continue;
        }

        if (validScheduledDate(oneResponse->maintenanceRequested))
        {
            if (oneResponse->maintenanceRequested < earliestWake)
            {
                earliestWake = oneResponse->maintenanceRequested;
                type = kChooseMaintenance;
            }
        }
    }

    if (!complete) {
        goto exit;
    }

    if (wrangler && BIT_IS_SET(wrangler->notificationType, kIOPMSystemCapabilityCPU)) {
        // Not a sleep: nothing to schedule.
        goto exit;
    }

    /* Check for user wake requests in the end to give highest priority, in case of conflict */
    CFDictionaryRef event = copyEarliestRequestAutoWakeEvent();
    if (event)
    {
        userWake = getWakeScheduleTime(event);

        if (validScheduledDate(userWake)) {
            if (userWake <= earliestWake) {
                earliestWake = userWake;
                type = kChooseFullWake;
            }
        }
        CFRelease(event);
    }

    /* check if there is any shutdown or restart request */
    event = copyEarliestShutdownRestartEvent();
    if (event)
    {
        //wake up a minute before shutdown
        userWake = getWakeScheduleTime(event);

        if (validScheduledDate(userWake)) {
            //schedule a wake for shutdown/restart event
            if (userWake <= earliestWake) {
                earliestWake = userWake;
                type = kChooseMaintenance;
            }
        }
        CFRelease(event);
    }

    PMScheduleWakeEventChooseBest(earliestWake, type);

exit:
    return complete;
}

static void checkResponses(PMResponseWrangler *wrangler)
{


    if (!checkResponses_ScheduleWakeEvents(wrangler)) {
        // Not all clients acknowledged.
        return;
    }

    // Completion: all clients have acknowledged.
    if (wrangler->awaitingResponsesTimeout) {
        dispatch_source_cancel(wrangler->awaitingResponsesTimeout);
        dispatch_release(wrangler->awaitingResponsesTimeout);
        wrangler->awaitingResponsesTimeout = NULL;
    }

    // Handle PowerManagement acknowledgements
    if (wrangler->kernelAcknowledgementID)
    {
        IOAllowPowerChange(gRootDomainConnect, wrangler->kernelAcknowledgementID);
    }

    cleanupResponseWrangler(wrangler);

    return;
}


static void PMScheduleWakeEventChooseBest(CFAbsoluteTime scheduleTime, wakeType_e type)
{
    CFStringRef     scheduleWakeType                    = NULL;

    if (!VALID_DATE(scheduleTime))
    {
        // Set a huge number for following comaprisions with power off timer
        scheduleTime = kCFAbsoluteTimeIntervalSince1904;
    }

    /* Apple: the auto power-off timer. */

    if ( scheduleTime == kCFAbsoluteTimeIntervalSince1904)
    {
       // Nothing to schedule. bail..
       return;
    }

    // INVARIANT: At least one of the WakeTimes we're evaluating has a valid date
    if ((kChooseMaintenance == type) || (kChooseTimerPlugin == type))
    {
        scheduleWakeType = CFSTR(kIOPMMaintenanceScheduleImmediate);
    } else if (kChooseFullWake == type)
    {
        scheduleWakeType = CFSTR(kIOPMAutoWakeScheduleImmediate);
    }

    if (VALID_DATE(scheduleTime) && scheduleWakeType)
    {
        CFDateRef theChosenDate = NULL;

        if ((theChosenDate = CFDateCreate(0, scheduleTime)))
        {
            /* Tell the RTC when PM wants to be woken up */
            IOPMSchedulePowerEvent(theChosenDate, NULL, scheduleWakeType);
            CFRelease(theChosenDate);

        }
    }

    return ;
}


/*****************************************************************************/

static CFArrayRef createArrayOfConnectionsWithInterest(
    int interestBits)
{
    CFMutableArrayRef       arrayFoundInterests = NULL;
    PMConnection            *lookee;
    CFIndex                 connectionsCount;
    CFIndex                 i;


    if (0 == interestBits)
        return NULL;

    // *********

    arrayFoundInterests = CFArrayCreateMutable(kCFAllocatorDefault, 0, &_CFArrayConnectionCallBacks);

    connectionsCount = CFArrayGetCount(gConnections);

    for (i=0; i<connectionsCount; i++)
    {
        lookee = (PMConnection *)CFArrayGetValueAtIndex(gConnections, i);

        // Matching interest in this connection
        if (interestBits & lookee->interestsBits) {
            CFArrayAppendValue(arrayFoundInterests, lookee);
        }
    }

    return arrayFoundInterests;

}

/*****************************************************************************/

static IOReturn createConnectionWithID(PMConnection **out)
{
    if (globalConnectionIDTally > (uint32_t)kMaxConnectionIDCount)
    {
        return kIOReturnNoSpace;
    }

    *out = (PMConnection *)calloc(1, sizeof(PMConnection));

    if (!*out)
        return kIOReturnNoMemory;

    ((PMConnection *)*out)->uniqueID = kConnectionOffset + globalConnectionIDTally++;

    // Add new connection to the global tracking array
    CFArrayAppendValue(gConnections, *out);

    return kIOReturnSuccess;
}

/*****************************************************************************/

static PMConnection *connectionForID(uint32_t findMe)
{
    CFIndex     where = 0;
    CFRange     theRange = CFRangeMake(0, CFArrayGetCount(gConnections));
    PMConnection     dummy;

    // Our gConnections CFArray equality callback only checks the "uniqueID" field
    // of the passed in PMConnection type. Searching for the value "dummy"
    // will return the first instance with connection ID "findMe".

    dummy.uniqueID = findMe;
    where = CFArrayGetFirstIndexOfValue(gConnections, theRange, &dummy);

    if (kCFNotFound == where) {
        return NULL;
    } else {
        return (PMConnection *)CFArrayGetValueAtIndex(gConnections, where);
    }
}

/*****************************************************************************/

__private_extern__ io_connect_t getRootDomainConnect(void)
{
    return gRootDomainConnect;
}
