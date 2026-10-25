/*
 * IOKit/IOKitLib.h — public header for the freebsd-launchd-mach
 * IOKit userland facade.
 *
 * This is the Apple IOKit C API as a thin client layered over
 * hwregd's MIG hwreg.defs Mach RPC. Per the plan
 * (pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html §5),
 * the facade is NOT a port of Apple's IOKitUser source (which
 * decomposes to a kernel device.defs surface this repo has no kernel
 * for) — it is a fresh, thin CF wrapper over hwregd. Apple's
 * io_object_t is a mach_port_t naming a kernel-resident IOService;
 * the facade has no such kernel, so io_object_t here is an opaque
 * pointer to a client-side struct holding a hwregd node id (or a
 * captured id array for iterators). Each routine that returns one
 * allocates it; the caller releases with IOObjectRelease.
 *
 * iter 1 — read-only registry walk:
 *   IORegistryGetRootEntry, IORegistryEntryGetChildIterator,
 *   IOIteratorNext, IORegistryEntryGetName, IORegistryEntryGetPath,
 *   IOObjectRetain, IOObjectRelease.
 *
 * iter 2 — properties + matching (CF-typed):
 *   IORegistryEntryCreateCFProperties, IORegistryEntryCreateCFProperty,
 *   IOObjectGetClass, IOServiceMatching, IOServiceGetMatchingService,
 *   IOServiceGetMatchingServices.
 *
 * iter 3 — `ioreg(8)` introspection tool (the K1 success marker in
 * the plan). Implemented entirely on top of iter 1 + iter 2; no new
 * facade API.
 *
 * iter 4 — K2 notifications (device-arrival / departure callbacks):
 *   IONotificationPortCreate, IONotificationPortDestroy,
 *   IONotificationPortGetMachPort, IONotificationPortSetDispatchQueue,
 *   IOServiceAddMatchingNotification.
 *
 * Interest notifications (nextbsd-userland#456, kernel nextbsd-kernel#338):
 *   IOServiceAddInterestNotification (kIOGeneralInterest), with the
 *   kIOMessage* codes in <IOKit/IOMessage.h>.
 *
 * NOTE: there is also a separate Apple-API stub at
 * src/launchd/freebsd-shims/IOKit/IOKitLib.h covering the four IOKit
 * calls launchctl makes (IOKitWaitQuiet etc.). That shim aliases
 * io_object_t to mach_port_t and is unrelated to this facade; they
 * sit on different include paths and are not used in the same TU.
 */
#ifndef _IOKIT_IOKITLIB_H_
#define _IOKIT_IOKITLIB_H_

#include <sys/cdefs.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_port.h>
#include <mach/kern_return.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque client-side handle. See the header comment.
 */
typedef struct __IOObject *io_object_t;
typedef io_object_t io_registry_entry_t;
typedef io_object_t io_service_t;
typedef io_object_t io_iterator_t;

#define IO_OBJECT_NULL ((io_object_t)0)

/*
 * Apple's IOKit name + path buffer typedefs. io_name_t is a 128-byte
 * fixed array used for class / registry-entry names; io_string_t is
 * 512 bytes and carries plane-qualified paths.
 */
typedef char io_name_t[128];
typedef char io_string_t[512];

/*
 * IOOptionBits — Apple's IOKit options-mask typedef. Accepted by
 * several routines (CreateCFProperties etc.) for source
 * compatibility; iter 2 ignores it.
 */
typedef uint32_t IOOptionBits;

/*
 * IOReturn — Apple's IOKit error code space. kern_return_t fits the
 * same range. iter 1 only needs a small subset (the rest exist on
 * macOS); add more as later iters grow the API.
 */
typedef int IOReturn;
#define kIOReturnSuccess     0
#define kIOReturnError       0x2bc
#define kIOReturnNoDevice    0x2c0
#define kIOReturnUnsupported 0x2c7
#define kIOReturnBadArgument 0x2c9
#define kIOReturnNotFound    0x2f0

/*
 * "main port" sentinels. On macOS these select the IOKit master
 * server. The facade resolves org.freebsd.hwregd lazily via
 * bootstrap_look_up on first use; this argument is ignored, but
 * declared so the Apple signatures match.
 */
#define kIOMainPortDefault   ((mach_port_t)0)
#define kIOMasterPortDefault ((mach_port_t)0)	/* deprecated alias */

/*
 * IORegistryGetRootEntry — handle to the registry root (the newbus
 * root device hwregd snapshots at startup). Returns IO_OBJECT_NULL
 * if hwregd is unreachable or the registry is empty. The returned
 * handle must be released with IOObjectRelease.
 */
io_registry_entry_t	IORegistryGetRootEntry(mach_port_t mainPort);

/*
 * IORegistryEntryGetChildIterator — iterator over `entry`'s direct
 * children. `plane` is accepted for source compatibility and
 * ignored (this facade has one plane, IOService). The iterator
 * must be released with IOObjectRelease.
 */
kern_return_t	IORegistryEntryGetChildIterator(io_registry_entry_t entry,
		    const io_name_t plane, io_iterator_t *iterator);

/*
 * IOIteratorNext — next entry from `iterator`, or IO_OBJECT_NULL
 * when exhausted. Each returned entry must be released with
 * IOObjectRelease.
 */
io_object_t	IOIteratorNext(io_iterator_t iterator);

/*
 * IORegistryEntryGetName — `entry`'s short name (e.g. "em0") into
 * the caller's io_name_t buffer.
 */
kern_return_t	IORegistryEntryGetName(io_registry_entry_t entry,
		    io_name_t name);

/*
 * IORegistryEntryGetPath — `entry`'s plane-qualified path into the
 * caller's io_string_t buffer. The result is "IOService:" followed
 * by the entry's newbus-tree path (e.g. "IOService:/pci0/em0").
 * `plane` is ignored.
 */
kern_return_t	IORegistryEntryGetPath(io_registry_entry_t entry,
		    const io_name_t plane, io_string_t path);

/*
 * IOObjectRetain — gain an additional reference to a handle.
 * IOObjectRelease — drop a reference and free the handle at zero.
 * Releasing IO_OBJECT_NULL is a no-op (matches macOS).
 */
kern_return_t	IOObjectRetain(io_object_t object);
kern_return_t	IOObjectRelease(io_object_t object);

/*
 * iter 2 ----------------------------------------------------------
 *
 * Well-known IOService matching dictionary keys (the small subset
 * the facade understands). The full Apple set is much larger; the
 * facade translates IOProviderClass / IOClass to hwregd's `class`
 * criterion and IONameMatch to `name`.
 */
#define kIOProviderClassKey	"IOProviderClass"
#define kIOClassKey		"IOClass"
#define kIONameMatchKey		"IONameMatch"

/*
 * IORegistryEntryCreateCFProperties — `entry`'s full property bag
 * as a freshly-allocated CFMutableDictionary (keys = CFString,
 * values = CFString or CFNumber — the only types hwregd's bag
 * carries). The caller releases the dictionary with CFRelease.
 * `options` is ignored.
 */
kern_return_t	IORegistryEntryCreateCFProperties(io_registry_entry_t entry,
		    CFMutableDictionaryRef *properties,
		    CFAllocatorRef allocator, IOOptionBits options);

/*
 * IORegistryEntryCreateCFProperty — a single property's value, +1
 * retain. NULL if `entry` has no such property. `options` is
 * ignored.
 */
CFTypeRef	IORegistryEntryCreateCFProperty(io_registry_entry_t entry,
		    CFStringRef key, CFAllocatorRef allocator,
		    IOOptionBits options);

/*
 * IOObjectGetClass — the entry's class name (hwregd's `class`
 * field — "CPU" / "PCIDevice" / "USBDevice" / etc.) into the
 * caller's io_name_t buffer.
 */
kern_return_t	IOObjectGetClass(io_object_t object, io_name_t className);

/*
 * IOServiceMatching — build a fresh matching dictionary against a
 * provider class. Equivalent to:
 *   { kIOProviderClassKey: CFSTR(name) }
 * Caller owns the returned dictionary; IOServiceGetMatchingService*
 * consume one reference.
 */
CFMutableDictionaryRef	IOServiceMatching(const char *name);

/*
 * IOServiceGetMatchingService(s) — look up service(s) whose hwregd
 * node matches `matching`. The single-result form returns the first
 * match (or IO_OBJECT_NULL); the iterator form returns an iterator
 * over all matches (always non-NULL on success — an empty iterator
 * is valid and yields IO_OBJECT_NULL on first IOIteratorNext).
 *
 * BOTH forms consume one reference of `matching` (Apple contract:
 * "one reference is always consumed by this function") so the
 * idiom `IOServiceGetMatchingService(mp, IOServiceMatching("X"))`
 * does not leak.
 */
io_service_t	IOServiceGetMatchingService(mach_port_t mainPort,
		    CFDictionaryRef matching);

kern_return_t	IOServiceGetMatchingServices(mach_port_t mainPort,
		    CFDictionaryRef matching, io_iterator_t *iterator);

/*
 * iter 4 ----------------------------------------------------------
 *
 * IONotificationPort — a Mach receive port plus a private receive
 * thread that demuxes hwregd watch-event messages and fires the
 * IOServiceMatchingCallback registered for each watcher. Each call
 * to IOServiceAddMatchingNotification owns one hwreg_watch
 * registration on the daemon side.
 *
 * Delivery: SetDispatchQueue routes callbacks via dispatch_async
 * onto the caller's queue. If no queue is set, callbacks run on
 * the internal receive thread — usable for simple consumers,
 * fine for the iokitnotifytest test client. A CFRunLoopSource
 * delivery option (Apple's IONotificationPortGetRunLoopSource) is
 * deferred — SCDynamicStore's runloop-source path covers the same
 * pattern when needed.
 *
 * NOTE: this comment used to say the facade delivers via a raw
 * mach_msg(MACH_RCV_MSG|MACH_RCV_TIMEOUT,500ms) loop in a pthread
 * and NOT a libdispatch source. That is no longer true and has not
 * been since commit 05a674f, which replaced the pthread poll loop
 * with a DISPATCH_SOURCE_TYPE_MACH_RECV source (see IOKitNotify.c:337).
 * The header was never updated. Corrected in nextbsd-userland#154.
 */
#include <dispatch/dispatch.h>

typedef struct IONotificationPort *IONotificationPortRef;

/*
 * Apple's well-known notification-type strings. The facade maps
 * the three "device arrived" flavours to HWREG_EVT_ARRIVED and
 * Terminate to HWREG_EVT_DEPARTED. They're aliases here because
 * hwregd has no internal IOService-lifecycle distinction between
 * Publish / FirstMatch / Matched.
 */
#define kIOPublishNotification		"IOServicePublish"
#define kIOFirstMatchNotification	"IOServiceFirstMatch"
#define kIOMatchedNotification		"IOServiceMatched"
#define kIOTerminatedNotification	"IOServiceTerminate"

/*
 * IOServiceMatchingCallback — fired when a service matching the
 * criteria arrives or departs. The iterator yields the new
 * service(s); the callback is expected to drain it (each
 * IOIteratorNext returns IO_OBJECT_NULL when done). The facade
 * tears the iterator down when the callback returns; the caller
 * MUST NOT IOObjectRelease it.
 */
typedef void (*IOServiceMatchingCallback)(void *refcon,
		    io_iterator_t iterator);

IONotificationPortRef	IONotificationPortCreate(mach_port_t mainPort);
void			IONotificationPortDestroy(IONotificationPortRef
			    notify);
void			IONotificationPortSetDispatchQueue(
			    IONotificationPortRef notify,
			    dispatch_queue_t queue);
mach_port_t		IONotificationPortGetMachPort(
			    IONotificationPortRef notify);

/*
 * IOServiceAddMatchingNotification — register a callback that
 * fires whenever a service matching `matching` arrives (Publish /
 * FirstMatch / Matched) or departs (Terminate). On return the
 * `notification` iterator carries the services that ALREADY
 * match — Apple's "initial arming"; consumers iterate it to find
 * existing devices and to arm the notification for future
 * arrivals.
 *
 * One reference of `matching` is consumed (Apple contract).
 *
 * The returned iterator survives until released with
 * IOObjectRelease — but releasing it does NOT cancel the watch;
 * use IONotificationPortDestroy to tear the notification down
 * (the facade has no IOObjectRelease-on-iterator → cancel path
 * in iter 4; if a consumer needs per-notification cancellation
 * later, an explicit IOServiceRemoveMatchingNotification will land
 * in a follow-up iter).
 */
kern_return_t	IOServiceAddMatchingNotification(
		    IONotificationPortRef notifyPort,
		    const io_name_t notification_type,
		    CFDictionaryRef matching,
		    IOServiceMatchingCallback callback,
		    void *refCon,
		    io_iterator_t *notification);

/*
 * Interest notifications ------------------------------------------
 *
 * IOServiceAddInterestNotification — Darwin's per-service message channel
 * (IOService::messageClients). Register `callback` for the messages the
 * provider of ONE existing service sends, delivered through `notifyPort`
 * with the same rules as matching notifications (on the queue set by
 * IONotificationPortSetDispatchQueue, or on the port's receive thread when
 * none is set). `service` is retained for as long as the registration lives
 * and is the object handed to the callback.
 *
 * Only kIOGeneralInterest is supported; any other interest type returns
 * kIOReturnUnsupported. Other errors: KERN_INVALID_ARGUMENT for a NULL or
 * non-entry argument, kIOReturnNotFound if the service has already departed,
 * kIOReturnNoDevice without /dev/ioregistry, kIOReturnUnsupported on a kernel
 * without interest support.
 *
 * Messages (kIOMessage*, <IOKit/IOMessage.h>) the NextBSD kernel sends:
 *   kIOMessageServicePropertyChange — e.g. a DRM connector hotplug on a drmn
 *       entry (see IOServiceInterestMessageArgument below);
 *   kIOMessageServiceIsTerminated — the service departed. This is the last
 *       message; the registration is inactive afterwards, and the caller
 *       still releases `*notification`.
 *
 * `*notification` is the registration. IOObjectRelease on it unregisters:
 * no callback for it starts after the release (one already handed to a
 * dispatch queue may still run). As with matching notifications the kernel
 * side of the watch lasts until the port is destroyed or the service
 * terminates; libIOKit drops messages for released registrations, and
 * registering the same service again on the same port reuses the kernel
 * watch, so each message reaches each registration exactly once.
 */
#define kIOGeneralInterest		"IOGeneralInterest"
#define kIOBusyInterest			"IOBusyInterest"
#define kIOAppPowerStateInterest	"IOAppPowerStateInterest"
#define kIOPriorityPowerStateInterest	"IOPriorityPowerStateInterest"

/*
 * IOServiceInterestCallback — Darwin's signature. `service` is the object
 * passed to IOServiceAddInterestNotification; do not release it.
 * `messageType` is a kIOMessage* code.
 *
 * `messageArgument` (NextBSD): Darwin documents it as message-specific and
 * the kernel's message carries more than one pointer's worth (two 64-bit
 * arguments and a device node), so libIOKit passes a pointer to an
 * IOServiceInterestMessageArgument. It is owned by libIOKit and valid only
 * until the callback returns; copy what you need. It is never NULL for
 * messages from the NextBSD kernel. Fields may be appended later; existing
 * ones keep their offsets.
 */
typedef void (*IOServiceInterestCallback)(void *refcon,
		    io_service_t service, uint32_t messageType,
		    void *messageArgument);

#define kIOServiceInterestDevnodeMax	32

typedef struct IOServiceInterestMessageArgument {
	/* Registry entry id of the service (IORegistryEntryGetRegistryEntryID). */
	uint64_t	registryEntryID;
	/*
	 * Message-specific arguments. kIOMessageServicePropertyChange from a
	 * DRM device (drmn): argument[0] is the DRM connector object id (0 for
	 * a change concerning the whole device), argument[1] the DRM property
	 * id (0 for a connection change). Zero for kIOMessageServiceIsTerminated.
	 */
	uint64_t	argument[2];
	/*
	 * Device node relative to /dev the message concerns, NUL-terminated,
	 * "" if none. DRM: "dri/cardN".
	 */
	char		devnode[kIOServiceInterestDevnodeMax];
} IOServiceInterestMessageArgument;

kern_return_t	IOServiceAddInterestNotification(
		    IONotificationPortRef notifyPort,
		    io_service_t service,
		    const io_name_t interestType,
		    IOServiceInterestCallback callback,
		    void *refCon,
		    io_object_t *notification);

/*
 * iter 5 — bus quiescence -----------------------------------------
 *
 * mach_timespec_t — Apple's IOKit wait-timeout struct (normally from
 * <mach/clock_types.h>). Declared inline here to avoid dragging in the
 * whole clock-types header (clock_res_t et al.) just for the two
 * fields IOServiceWaitQuiet / IOKitWaitQuiet consume. tv_sec/tv_nsec
 * are summed to a nanosecond budget and handed to the mach_wait_quiet
 * syscall.
 */
#ifndef _MACH_CLOCK_TYPES_H_
typedef struct mach_timespec {
	unsigned int	tv_sec;
	int		tv_nsec;
} mach_timespec_t;
#endif

/*
 * IORegistryEntryGetBusyState — *busyState = the number of in-flight
 * device probe->attach operations on the host.
 *
 * GLOBAL-APPROXIMATION DIVERGENCE FROM APPLE: on macOS this reports the
 * busy count of a SPECIFIC IORegistryEntry's subtree. This facade has
 * no kernel IOService tree to walk per-entry; it reports the GLOBAL
 * kernel bus-busy count from `sysctl mach.bus.busy` (the in-flight
 * device_probe_and_attach() depth maintained by mach.ko's
 * device_match_start/device_match_end eventhandler consumer). The
 * `entry` argument is therefore accepted for source compatibility and
 * IGNORED — busyState is host-wide, not subtree-scoped. Returns
 * kIOReturnSuccess (0) once the count was read; non-zero kern_return_t
 * if the sysctl is unavailable (mach.ko not loaded).
 */
kern_return_t	IORegistryEntryGetBusyState(mach_port_t mainPort,
		    io_registry_entry_t entry, uint32_t *busyState);

/*
 * IOServiceWaitQuiet / IOKitWaitQuiet — block until the host's device
 * tree quiesces (global mach.bus.busy reaches 0), or until `timeout`
 * elapses (NULL == wait indefinitely). Both resolve the mach_wait_quiet
 * syscall via `sysctl mach.syscall.mach_wait_quiet` and call it with the
 * timeout converted to nanoseconds. Like Apple's APIs they return
 * success on EITHER quiescence or the deadline elapsing
 * (kIOReturnSuccess); a resolution failure maps to kIOReturnError.
 *
 * Per the GLOBAL-APPROXIMATION note above, IOServiceWaitQuiet ignores
 * its `service` argument — the wait is host-wide.
 */
kern_return_t	IOServiceWaitQuiet(io_service_t service,
		    mach_timespec_t *timeout);
IOReturn	IOKitWaitQuiet(mach_port_t mainPort, mach_timespec_t *timeout);

#ifdef __cplusplus
}
#endif

#endif /* _IOKIT_IOKITLIB_H_ */
