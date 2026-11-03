/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * HISTORY
 *
 */

/*
 * NextBSD: vendored from apple-oss-distributions/IOKitUser @
 * IOKitUser-100231.120.3 (ps.subproj/IOPowerSources.c) for libIOKit
 * (nextbsd-userland#232). pmconfigd serves the other end: the
 * io_ps_copy_powersources_info MIG routine and the
 * com.apple.system.powersources.* notify(3) state (nextbsd-userland#227).
 *
 * The __FreeBSD__ changes:
 *  - The Darwin-only headers are replaced by ps_freebsd.h (os_log,
 *    SystemConfiguration's isA_* helpers); the ps headers come from
 *    <IOKit/ps/>, where libIOKit installs them.
 *  - IOPSCopyExternalPowerAdapterDetails returns NULL, Apple's answer when
 *    there are no adapter details: the kernel's IOPMPowerSource entries carry
 *    no AdapterDetails, pmconfigd does not answer the kPSAdapterDetails XPC
 *    request, and NextBSD's CF has no CF <-> XPC bridge to decode a reply.
 *  - IOPSGetPercentRemaining is defined here. IOKitUser declares it in
 *    IOPowerSourcesPrivate.h, but this drop of the source no longer carries
 *    its body; it decodes kIOPSNotifyPercentChange's state the way
 *    pmconfigd's publish_IOPSGetPercentRemaining() packs it.
 *  - The run loop sources: Apple's are CFMachPort sources on a
 *    notify_register_mach_port() port. NextBSD's CoreFoundation has no
 *    CFMachPort, so the notification arrives through
 *    notify_register_dispatch() and signals a version 0 source, as
 *    IONotificationPortGetRunLoopSource, libDiskArbitration and SCDynamicStore
 *    do. The callback still runs on the run loop's thread.
 */

#include <sys/cdefs.h>
#include <notify.h>
#include <mach/vm_map.h>

#ifdef __FreeBSD__
#include "ps_freebsd.h"
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPowerSourcesPrivate.h>
#include "powermanagement.h"
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <notify.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFXPCBridge.h>
#include "IOSystemConfiguration.h"
#include "IOPSKeys.h"
#include "IOPowerSources.h"
#include "IOPowerSourcesPrivate.h"
#include "powermanagement.h"
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <notify.h>
#include <xpc/xpc.h>
#include <os/log.h>
#endif


#ifndef kIOPSDynamicStoreLowBattPathKey
#define kIOPSDynamicStoreLowBattPathKey "/IOKit/LowBatteryWarning"
#endif

#ifndef __FreeBSD__
dispatch_queue_t  getPMQueue();
#define POWERD_XPC_ID   "com.apple.iokit.powerdxpc"
#endif

IOPSLowBatteryWarningLevel IOPSGetBatteryWarningLevel(void)
{
    SCDynamicStoreRef   store = NULL;
    CFStringRef         key = NULL;
    CFNumberRef         scWarnValue = NULL;
    int                 return_level = kIOPSLowBatteryWarningNone;

    store = SCDynamicStoreCreate(kCFAllocatorDefault, 
                            CFSTR("IOKit Power Source Copy"), NULL, NULL);
    if (!store) 
        goto SAD_EXIT;

    key = SCDynamicStoreKeyCreate(
                            kCFAllocatorDefault, 
                            CFSTR("%@%@"),
                            _io_kSCDynamicStoreDomainState, 
                            CFSTR(kIOPSDynamicStoreLowBattPathKey));
    if (!key)
        goto SAD_EXIT;

    scWarnValue = SCDynamicStoreCopyValue(store, key);
    if (scWarnValue) {

        if(isA_CFType(scWarnValue, CFNumberGetTypeID()))
        {
            CFNumberGetValue(scWarnValue, kCFNumberIntType, &return_level);
        }
        CFRelease(scWarnValue);
        scWarnValue = NULL;
    }

SAD_EXIT:
    if (store) CFRelease(store);
    if (key) CFRelease(key);
    return return_level;
}


// powerd uses these same constants to define the bitfields in the
// kIOPSTimeRemainingNotificationKey key
#define     _kSecondsPerMinute                       ((CFTimeInterval)60.0)
CFTimeInterval IOPSGetTimeRemainingEstimate(void)
{
    int                 token = 0;
    uint64_t            packedBatteryData = 0;
    int                 status = 0;

    status = notify_register_check(kIOPSTimeRemainingNotificationKey, &token);

    if (NOTIFY_STATUS_OK != status) {
        // FAILURE: We return an optimistic unlimited time remaining estimate 
        // if we don't know the truth.
        return kIOPSTimeRemainingUnlimited;
    }

    notify_get_state(token, &packedBatteryData);

    notify_cancel(token);

    if (!(packedBatteryData & kPSTimeRemainingNotifyValidBit)
        || (packedBatteryData & kPSTimeRemainingNotifyExternalBit)) {
        return kIOPSTimeRemainingUnlimited;
    }

    if (packedBatteryData & kPSTimeRemainingNotifyUnknownBit) {
        return kIOPSTimeRemainingUnknown;
    }
    
    return (_kSecondsPerMinute * (CFTimeInterval)(packedBatteryData & 0xFFFF));
}

#ifdef __FreeBSD__
/*
 * NextBSD: not in this drop of IOKitUser (see the top of the file). The
 * state is pmconfigd's: the percent (0-100) in the low bits, plus the
 * kPSTimeRemainingNotify* bits. As the header says, a failure also reports
 * 100%.
 */
IOReturn IOPSGetPercentRemaining(int *percent, bool *isCharging, bool *isFullyCharged)
{
    int                 token = 0;
    uint64_t            packedBatteryBits = 0;
    int                 status = 0;
    IOReturn            ret = kIOReturnSuccess;

    if (!percent) {
        return kIOReturnBadArgument;
    }

    *percent = 100;
    if (isCharging) {
        *isCharging = false;
    }
    if (isFullyCharged) {
        *isFullyCharged = false;
    }

    status = notify_register_check(kIOPSNotifyPercentChange, &token);
    if (NOTIFY_STATUS_OK != status) {
        return kIOReturnInternalError;
    }
    notify_get_state(token, &packedBatteryBits);
    notify_cancel(token);

    if (!(packedBatteryBits & kPSTimeRemainingNotifyValidBit)) {
        return kIOReturnNotReady;
    }

    *percent = (int)(packedBatteryBits & 0xFF);
    if (*percent > 100) {
        *percent = 100;
    }
    if (isCharging) {
        *isCharging = (packedBatteryBits & kPSTimeRemainingNotifyChargingBit) ? true : false;
    }
    if (isFullyCharged) {
        *isFullyCharged = (packedBatteryBits & kPSTimeRemainingNotifyFullyChargedBit) ? true : false;
    }

    return ret;
}
#endif /* __FreeBSD__ */

bool IOPSDrawingUnlimitedPower(void)
{
    int                 token = 0;
    uint64_t            packedBatteryBits = 0;
    int                 status = 0;
    bool                unlimitedPower = true;
    const int           kUnlimitedBits = (kPSTimeRemainingNotifyValidBit |
                                          kPSTimeRemainingNotifyExternalBit);

    status = notify_register_check(kIOPSNotifyPercentChange, &token);
    if (NOTIFY_STATUS_OK == status) {
        notify_get_state(token, &packedBatteryBits);
        notify_cancel(token);
        if ((packedBatteryBits & kUnlimitedBits) == kPSTimeRemainingNotifyValidBit)
            unlimitedPower = false;
    }

    return unlimitedPower;
}

IOReturn IOPSGetSupportedPowerSources(IOPSPowerSourceIndex *active,
                                      bool *batterySupport,
                                      bool *externalBatteryAttached)
{
    int                 token = 0;
    int                 status = 0;
    uint64_t            packedBatteryBits = 0;

    status = notify_register_check(kIOPSTimeRemainingNotificationKey,
                                           &token);
    if (NOTIFY_STATUS_OK != status) {
        return kIOReturnError;
    }

    notify_get_state(token, &packedBatteryBits);
    notify_cancel(token);

    if (batterySupport) {
        *batterySupport =
            (packedBatteryBits & kPSTimeRemainingNotifyBattSupportBit) ? true:false;
    }

    if (externalBatteryAttached) {
        *externalBatteryAttached =
            (packedBatteryBits & kPSTimeRemainingNotifyUPSSupportBit) ? true:false;
    }

    uint8_t activeInt = 0xFF & (packedBatteryBits >> kPSTimeRemainingNotifyActivePS8BitsStarts);
    if (active) {
        *active = (int)activeInt;
    }

    return kIOReturnSuccess;
}

CFDictionaryRef IOPSCopyExternalPowerAdapterDetails(void)
{
#ifdef __FreeBSD__
    /* NextBSD: no adapter details to ask for (see the top of the file). */
    return NULL;
#else
    xpc_object_t        connection = NULL;
    xpc_object_t        msg = NULL;
    xpc_object_t        respData = NULL;
    xpc_object_t        reply = NULL;
    CFDictionaryRef     ret_dict = NULL;
    dispatch_queue_t    pmQueue = getPMQueue();

    if (!pmQueue) {
        return NULL;
    }
    connection = xpc_connection_create_mach_service(POWERD_XPC_ID, pmQueue, 0);
    if (!connection) {
        os_log_error(OS_LOG_DEFAULT, "Failed to create connection\n");
        goto exit;
    }
    xpc_connection_set_target_queue(connection, pmQueue);
    xpc_connection_set_event_handler(connection,
            ^(xpc_object_t e ) {
            os_log_error(OS_LOG_DEFAULT, "Event handler is called %@\n", e);
            });
    xpc_connection_resume(connection);
    msg = xpc_dictionary_create(NULL, NULL, 0);
    if (!msg) {
        os_log_error(OS_LOG_DEFAULT, "Failed to create message\n");
        goto exit;
    }

    xpc_dictionary_set_string(msg, kPSAdapterDetails, "true");
    reply = xpc_connection_send_message_with_reply_sync(connection, msg);

    if ((xpc_get_type(reply) == XPC_TYPE_DICTIONARY) &&
            (respData = xpc_dictionary_get_value(reply, kPSAdapterDetails))) {
        ret_dict = _CFXPCCreateCFObjectFromXPCObject(respData);
    }

exit:
    if (reply) {
        xpc_release(reply);
    }
    if (msg) {
        xpc_release(msg);
    }
    if (connection) {
        xpc_release(connection);
    }


    return ret_dict;
#endif /* !__FreeBSD__ */
}

__private_extern__ IOReturn _pm_connect(mach_port_t *newConnection);
__private_extern__ IOReturn _pm_disconnect(mach_port_t connection);

CFTypeRef IOPSCopyPowerSourcesByType(int type) {
    CFArrayRef          ps_arr = NULL;

    mach_port_t pm_server = MACH_PORT_NULL;

    if (kIOReturnSuccess == _pm_connect(&pm_server))
    {
        vm_address_t            buffer = 0;
        vm_size_t               size = 0;
        CFDataRef               d = NULL;
        int                     return_code;

        io_ps_copy_powersources_info(pm_server,
                                      type,
                                      &buffer,
                                      (mach_msg_type_number_t *) &size,
                                     &return_code);

        d = CFDataCreate(0, (const UInt8 *)buffer, size);
        if (d) {
            ps_arr = (CFArrayRef)CFPropertyListCreateWithData(0, d, 0, NULL, NULL);
            CFRelease(d);
        }

        vm_deallocate(mach_task_self(), buffer, size);
    }

    if(!ps_arr) {
        // On failure, we return an empty array instead of NULL
        ps_arr = (CFTypeRef)CFArrayCreate(kCFAllocatorDefault,
                                          NULL, 0,
                                          &kCFTypeArrayCallBacks);
    }

    // Return CFDictionary as opaque CFTypeRef
    return (CFTypeRef)ps_arr;
}

/***
 Returns a blob of Power Source information in an opaque CFTypeRef. Clients should
 not actually look directly at data in the CFTypeRef - they should use the accessor
 functions IOPSCopyPowerSourcesList and IOPSGetPowerSourceDescription, instead.
 Returns NULL if errors were encountered.
 Return: Caller must CFRelease() the return value when done.
***/
CFTypeRef IOPSCopyPowerSourcesInfo(void) {

    return IOPSCopyPowerSourcesByType(kIOPSSourceInternalAndUPS);
}


/***
 Arguments - Takes the CFTypeRef returned by IOPSCopyPowerSourcesInfo()
 Returns a CFArray of Power Source handles, each of type CFTypeRef.
 The caller shouldn't look directly at the CFTypeRefs, but should use
 IOPSGetPowerSourceDescription on each member of the CFArray.
 Returns NULL if errors were encountered.
 Return: Caller must CFRelease() the returned CFArray.
***/
CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef blob) {
    if (isA_CFArray(blob)) {
        return CFArrayCreateCopy(0, (CFArrayRef)blob);
    }

    return NULL;
}

/***
 Arguments - Takes one of the CFTypeRefs in the CFArray returned by
 IOPSCopyPowerSourcesList
 Returns a CFDictionary with specific information about the power source.
 See IOKit.framework/Headers/ups/IOUPSKeys.h for specific fields.
 Return: Caller should not CFRelease the returned CFArray
***/
CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef blob, CFTypeRef ps) {
    if (blob) {
        return (CFDictionaryRef)ps;
    }
    
    return NULL;
}


/* IOPSGetProvidingPowerSourceType
 * Argument: 
 *  ps_blob: as returned from IOPSCopyPowerSourcesInfo()
 * Returns: 
 *  The current system power source.
 *  CFSTR("AC Power"), CFSTR("Battery Power"), CFSTR("UPS Power")
 */
enum {
    kProvidedByAC = 1,
    kProvidedByBattery,
    kProvidedByUPS
};

CFStringRef IOPSGetProvidingPowerSourceType(CFTypeRef ps_blob __unused)
{
    IOPSPowerSourceIndex activeps;
    IOReturn ret;
    CFStringRef returnValue;

    ret = IOPSGetSupportedPowerSources(&activeps, NULL, NULL);

    if (kIOReturnSuccess != ret)
    {
        return CFSTR(kIOPMACPowerKey);
    }

    if (kIOPSProvidedByExternalBattery == activeps) {
        returnValue = CFSTR(kIOPMUPSPowerKey);
    } else if (kIOPSProvidedByBattery == activeps) {
        returnValue = CFSTR(kIOPMBatteryPowerKey);
    } else {
        returnValue = CFSTR(kIOPMACPowerKey);
    }

    return returnValue;
}


/***
 Support structures and functions for IOPSNotificationCreateRunLoopSource
***/
#ifdef __FreeBSD__
/*
 * NextBSD: a version 0 run loop source, signalled from a notify(3) dispatch
 * handler (see the top of the file). The context is the source's info; the
 * source's release callback cancels the registration and frees the context
 * on the handler's serial queue, behind any handler already queued.
 */
typedef struct {
    IOPowerSourceCallbackType       callback;
    void                            *context;
    int                             token;
    pthread_mutex_t                 lock;
    CFRunLoopSourceRef              rls;        /* not retained; NULL once released */
    CFMutableArrayRef               runloops;   /* where rls is scheduled */
} IOPSNotifyCallbackContext;

static dispatch_queue_t IOPSNotifyQueue(void)
{
    static dispatch_once_t  once;
    static dispatch_queue_t q;

    dispatch_once(&once, ^{
        q = dispatch_queue_create("com.apple.iokit.powersources.notify", NULL);
    });
    return q;
}

static void IOPSRLSPerform(void *info)
{
    IOPSNotifyCallbackContext	*c = (IOPSNotifyCallbackContext *)info;
    IOPowerSourceCallbackType cb;
    
    if (c && (cb = c->callback)) {
        (*cb)(c->context);
    }
}

static void IOPSRLSSchedule(void *info, CFRunLoopRef rl, CFStringRef mode __unused)
{
    IOPSNotifyCallbackContext	*c = (IOPSNotifyCallbackContext *)info;

    pthread_mutex_lock(&c->lock);
    CFArrayAppendValue(c->runloops, rl);
    pthread_mutex_unlock(&c->lock);
}

static void IOPSRLSCancel(void *info, CFRunLoopRef rl, CFStringRef mode __unused)
{
    IOPSNotifyCallbackContext	*c = (IOPSNotifyCallbackContext *)info;
    CFIndex                     idx;

    pthread_mutex_lock(&c->lock);
    idx = CFArrayGetFirstIndexOfValue(c->runloops,
            CFRangeMake(0, CFArrayGetCount(c->runloops)), rl);
    if (idx != kCFNotFound) {
        CFArrayRemoveValueAtIndex(c->runloops, idx);
    }
    pthread_mutex_unlock(&c->lock);
}

static void IOPSRLSFree(void *info)
{
    IOPSNotifyCallbackContext	*c = (IOPSNotifyCallbackContext *)info;

    CFRelease(c->runloops);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

static void IOPSRLSRelease(const void *info)
{
    IOPSNotifyCallbackContext	*c = (IOPSNotifyCallbackContext *)info;
    
    if (c) {
        pthread_mutex_lock(&c->lock);
        c->rls = NULL;
        pthread_mutex_unlock(&c->lock);
        if (0 != c->token) {
            notify_cancel(c->token);
        }
        dispatch_async_f(IOPSNotifyQueue(), c, IOPSRLSFree);
    }
}

static void IOPSRLSNotify(IOPSNotifyCallbackContext *c)
{
    CFIndex     i, n;

    pthread_mutex_lock(&c->lock);
    if (c->rls) {
        CFRunLoopSourceSignal(c->rls);
        n = CFArrayGetCount(c->runloops);
        for (i = 0; i < n; i++) {
            CFRunLoopWakeUp((CFRunLoopRef)CFArrayGetValueAtIndex(c->runloops, i));
        }
    }
    pthread_mutex_unlock(&c->lock);
}

static CFRunLoopSourceRef doCreatePSRLS(const char *notify_type, IOPowerSourceCallbackType callback, void *context)
{
    int                             status = 0;
    int                             token = 0;
    CFRunLoopSourceContext          rlsContext;
    CFRunLoopSourceRef              rls = NULL;
    IOPSNotifyCallbackContext       *ioContext;

    ioContext = calloc(1, sizeof(IOPSNotifyCallbackContext));
    if (!ioContext) {
        return NULL;
    }
    ioContext->callback = callback;
    ioContext->context = context;
    pthread_mutex_init(&ioContext->lock, NULL);
    ioContext->runloops = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);
    if (!ioContext->runloops) {
        pthread_mutex_destroy(&ioContext->lock);
        free(ioContext);
        return NULL;
    }

    bzero(&rlsContext, sizeof(rlsContext));
    rlsContext.version = 0;
    rlsContext.info = (void *)ioContext;
    rlsContext.release = IOPSRLSRelease;
    rlsContext.schedule = IOPSRLSSchedule;
    rlsContext.cancel = IOPSRLSCancel;
    rlsContext.perform = IOPSRLSPerform;

    rls = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &rlsContext);
    if (!rls) {
        IOPSRLSFree(ioContext);
        return NULL;
    }
    /* From here the source owns ioContext: releasing it frees the context. */
    ioContext->rls = rls;

    status = notify_register_dispatch(notify_type, &token, IOPSNotifyQueue(),
                                      ^(int t __unused) {
                                          IOPSRLSNotify(ioContext);
                                      });
    if (NOTIFY_STATUS_OK != status) {
        CFRelease(rls);
        return NULL;
    }
    ioContext->token = token;

    return rls;
}
#else /* !__FreeBSD__ */
typedef struct {
    IOPowerSourceCallbackType       callback;
    void                            *context;
    int                             token;
    CFMachPortRef                   mpRef;
} IOPSNotifyCallbackContext;

static void IOPSRLSMachPortCallback (CFMachPortRef port __unused, void *msg __unused, CFIndex size __unused, void *info)
{
    IOPSNotifyCallbackContext	*c = (IOPSNotifyCallbackContext *)info;
    IOPowerSourceCallbackType cb;
    
    if (c && (cb = c->callback)) {
        (*cb)(c->context);
    }
}

static void IOPSRLSMachPortRelease(const void *info)
{
    IOPSNotifyCallbackContext	*c = (IOPSNotifyCallbackContext *)info;
    
    if (c) {
        if (0 != c->token) {
            notify_cancel(c->token);
        }
        if (c->mpRef) {
            CFMachPortInvalidate(c->mpRef);
            CFRelease(c->mpRef);
        }
        free(c);
    }
}


static CFRunLoopSourceRef doCreatePSRLS(const char *notify_type, IOPowerSourceCallbackType callback, void *context)
{
    int                             status = 0;
    int                             token = 0;
    mach_port_t                     mp = MACH_PORT_NULL;
    CFMachPortRef                   mpRef = NULL;
    CFMachPortContext               mpContext;
    CFRunLoopSourceRef              mpRLS = NULL;
    IOPSNotifyCallbackContext       *ioContext;
    Boolean                         isReused = false;
    int                             giveUpRetryCount = 5;

    status = notify_register_mach_port(notify_type, &mp, 0, &token);
    if (NOTIFY_STATUS_OK != status) {
        return NULL;
    }
    
    ioContext = calloc(1, sizeof(IOPSNotifyCallbackContext));
    ioContext->callback = callback;
    ioContext->context = context;
    ioContext->token = token;

    bzero(&mpContext, sizeof(mpContext));
    mpContext.info = (void *)ioContext;
    mpContext.release = IOPSRLSMachPortRelease;
    
    do {
        if (mpRef) {
            // CFMachPorts may be reused. We don't want to get a reused mach port; so if we're unlucky enough
            // to get one, we'll pre-emptively invalidate it, throw them back in the pool, and retry.
            CFMachPortInvalidate(mpRef);
            CFRelease(mpRef);
        }
        
        mpRef = CFMachPortCreateWithPort(0, mp, IOPSRLSMachPortCallback, &mpContext, &isReused);
    } while (!mpRef && isReused && (--giveUpRetryCount > 0));

    if (mpRef) {
        if (!isReused) {
            // A reused mach port is a failure; it'll have an invalid callback pointer associated with it.
            ioContext->mpRef = mpRef;
            mpRLS = CFMachPortCreateRunLoopSource(0, mpRef, 0);
        }
        CFRelease(mpRef);
    }
    
    return mpRLS;
}
#endif /* !__FreeBSD__ */

CFRunLoopSourceRef IOPSNotificationCreateRunLoopSource(IOPowerSourceCallbackType callback, void *context) {
    return doCreatePSRLS(kIOPSNotifyTimeRemaining, callback, context);
}

CFRunLoopSourceRef IOPSCreateLimitedPowerNotification(IOPowerSourceCallbackType callback, void *context) {
    return doCreatePSRLS(kIOPSNotifyPowerSource, callback, context);
}

CFRunLoopSourceRef IOPSAccNotificationCreateRunLoopSource(IOPowerSourceCallbackType callback, void *context) {
    return doCreatePSRLS(kIOPSAccNotifyTimeRemaining, callback, context);
}

CFRunLoopSourceRef IOPSAccCreateLimitedPowerNotification(IOPowerSourceCallbackType callback, void *context) {
    return doCreatePSRLS(kIOPSAccNotifyPowerSource, callback, context);
}

CFRunLoopSourceRef IOPSAccCreateAttachNotification(IOPowerSourceCallbackType callback, void *context) {
    return doCreatePSRLS(kIOPSAccNotifyAttach, callback, context);
}
