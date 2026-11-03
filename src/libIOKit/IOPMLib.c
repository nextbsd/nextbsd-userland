/*
 * IOPMLib.c — libIOKit: system power against the in-kernel IOPMrootDomain
 * (nextbsd-userland#234, E17 L4; kernel nextbsd-kernel#243, K4).
 *
 * As on Darwin, the root domain lives in the kernel. Apple's IOPMLib.c finds
 * it in the registry, opens its user client with IOServiceOpen and calls
 * RootDomainUserClient methods with IOConnectCall*Method. NextBSD has neither
 * call, so the io_connect_t here (IOOBJ_KIND_CONNECT) holds a descriptor on
 * /dev/iopmroot, the user client's stand-in, and each method is an ioctl
 * (<sys/iopmroot.h>, vendored as iopmroot.h):
 *
 *   IORegisterForSystemPower   IOServiceAddInterestNotification
 *                              (kIOAppPowerStateInterest) on the IOPMrootDomain
 *                              entry, plus the connection
 *   IOAllowPowerChange         kPMAllowPowerChange  -> IOPMROOT_ALLOW
 *   IOCancelPowerChange        kPMCancelPowerChange -> IOPMROOT_CANCEL
 *   IOPMSleepSystem            kPMSleepSystem       -> IOPMROOT_SLEEP
 *   IOPMSet/GetAggressiveness  kPMSet/GetAggressiveness -> IOPMROOT_SET/GETAGGR
 *   IORegistryEntrySetCFProperty on the root domain   -> IOPMROOT_SETPROP
 *
 * The order of the steps and what is returned on failure follow Apple's
 * IOPMLib.c (IOKitUser-100231.120.3, pwr_mgt.subproj). In particular a kernel
 * without the root domain (older than #243) has no IOPMrootDomain entry, so
 * IORegisterForSystemPower and IOPMFindPowerManagement return IO_OBJECT_NULL,
 * as Apple's do when the service is missing.
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>	/* kIOReturn*: Apple's values */
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include "IOKitInternal.h"

#include "iopmroot.h"		/* vendored: /dev/iopmroot ioctls */

#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int		pmroot_fd = -1;
static pthread_once_t	pmroot_once = PTHREAD_ONCE_INIT;

static void
pmroot_open_once(void)
{
	pmroot_fd = open(IOPMROOT_DEVICE, O_RDWR | O_CLOEXEC);
}

int
__io_pmroot_fd(void)
{
	(void)pthread_once(&pmroot_once, pmroot_open_once);
	return (pmroot_fd);
}

/* The errno of a /dev/iopmroot ioctl as the IOReturn the root domain's user
 * client gives on Darwin. */
static IOReturn
pmroot_ioreturn(int error)
{
	switch (error) {
	case 0:
		return (kIOReturnSuccess);
	case EPERM:
		return (kIOReturnNotPrivileged);
	case EINVAL:
		return (kIOReturnBadArgument);
	case EOPNOTSUPP:
		return (kIOReturnUnsupported);
	case EBUSY:		/* "SleepDisabled", "System Shutdown" */
		return (kIOReturnNotPermitted);
	case ENOENT:
	case ENXIO:
		return (kIOReturnNoDevice);
	default:
		return (kIOReturnError);
	}
}

static int
connect_fd(io_connect_t connect)
{
	if (connect == IO_OBJECT_NULL || connect->kind != IOOBJ_KIND_CONNECT)
		return (-1);
	return (atomic_load(&connect->fd));
}

static IOReturn
connect_ioctl(io_connect_t connect, unsigned long cmd, void *arg)
{
	int fd = connect_fd(connect);

	if (fd < 0)
		return (kIOReturnBadArgument);
	if (ioctl(fd, cmd, arg) != 0)
		return (pmroot_ioreturn(errno));
	return (kIOReturnSuccess);
}

/* Apple looks the root domain up by its IOPower plane path; NextBSD's
 * registry has the one IOService plane, where it is the one entry of its
 * class. */
static io_service_t
pmroot_service(void)
{
	return (IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching(IOPMROOT_CLASS)));
}

static bool
is_pmroot(io_registry_entry_t entry)
{
	io_name_t klass;

	if (entry == IO_OBJECT_NULL || entry->kind != IOOBJ_KIND_ENTRY)
		return (false);
	return (IOObjectGetClass(entry, klass) == KERN_SUCCESS &&
	    strcmp(klass, IOPMROOT_CLASS) == 0);
}

/*
 * IOServiceOpen on the root domain: a connection with its own descriptor, as
 * each IOServiceOpen makes its own user client.
 */
static io_connect_t
pmroot_connect(io_service_t service)
{
	io_connect_t conn;
	int fd;

	fd = open(IOPMROOT_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return (IO_OBJECT_NULL);
	conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		(void)close(fd);
		return (IO_OBJECT_NULL);
	}
	conn->kind = IOOBJ_KIND_CONNECT;
	atomic_store(&conn->refcnt, 1);
	conn->node_id = service->node_id;
	atomic_store(&conn->fd, fd);
	return (conn);
}

void
__io_connect_release(io_connect_t connect)
{
	int fd = atomic_exchange(&connect->fd, -1);

	if (fd >= 0)
		(void)close(fd);
}

/* Apple's IOServiceClose closes the user client and releases the handle. */
kern_return_t
IOServiceClose(io_connect_t connect)
{
	if (connect == IO_OBJECT_NULL || connect->kind != IOOBJ_KIND_CONNECT)
		return (kIOReturnBadArgument);
	__io_connect_release(connect);
	return (IOObjectRelease(connect));
}

io_connect_t
IOPMFindPowerManagement(mach_port_t master_device_port __unused)
{
	io_connect_t fb = IO_OBJECT_NULL;
	io_service_t obj;

	obj = pmroot_service();
	if (obj != IO_OBJECT_NULL) {
		fb = pmroot_connect(obj);
		IOObjectRelease(obj);
	}
	return (fb);
}

io_connect_t
IORegisterForSystemPower(void *refcon, IONotificationPortRef *thePortRef,
    IOServiceInterestCallback callback, io_object_t *root_notifier)
{
	io_connect_t fb = IO_OBJECT_NULL;
	IONotificationPortRef notify = NULL;
	io_service_t obj = IO_OBJECT_NULL;
	kern_return_t kr;

	if (thePortRef == NULL || root_notifier == NULL)
		return (IO_OBJECT_NULL);
	*root_notifier = IO_OBJECT_NULL;

	notify = IONotificationPortCreate(MACH_PORT_NULL);
	if (notify == NULL)
		goto failure_exit;

	obj = pmroot_service();
	if (obj == IO_OBJECT_NULL)
		goto failure_exit;

	fb = pmroot_connect(obj);
	if (fb == IO_OBJECT_NULL)
		goto failure_exit;

	kr = IOServiceAddInterestNotification(notify, obj,
	    kIOAppPowerStateInterest, callback, refcon, root_notifier);

	IOObjectRelease(obj);
	obj = IO_OBJECT_NULL;

	if (kr == KERN_SUCCESS) {
		/* Successful exit case */
		*thePortRef = notify;
		return (fb);
	}

failure_exit:
	if (obj != IO_OBJECT_NULL)
		IOObjectRelease(obj);
	if (notify != NULL)
		IONotificationPortDestroy(notify);
	if (fb != IO_OBJECT_NULL)
		IOServiceClose(fb);
	if (*root_notifier != IO_OBJECT_NULL) {
		IOObjectRelease(*root_notifier);
		*root_notifier = IO_OBJECT_NULL;
	}
	return (IO_OBJECT_NULL);
}

/*
 * As on Darwin, this releases the notifier only; the caller still closes the
 * connection (IOServiceClose) and destroys the port. Until the port goes, a
 * sleep/wake message that still reaches it is acknowledged by libIOKit
 * (handle_interest, IOKitNotify.c), so the root domain does not wait for it.
 */
IOReturn
IODeregisterForSystemPower(io_object_t *root_notifier)
{
	if (root_notifier == NULL)
		return (kIOReturnBadArgument);
	if (*root_notifier != IO_OBJECT_NULL) {
		IOObjectRelease(*root_notifier);
		*root_notifier = IO_OBJECT_NULL;
	}
	return (kIOReturnSuccess);
}

/*
 * The acknowledgement for kIOMessageCanSystemSleep and
 * kIOMessageSystemWillSleep; notificationID is the callback's
 * messageArgument. A stale or unknown id is ignored by the kernel. Apple
 * turns any failure of the call into kIOReturnError.
 */
IOReturn
IOAllowPowerChange(io_connect_t kernelPort, intptr_t notificationID)
{
	int64_t nid = (int64_t)notificationID;

	if (connect_ioctl(kernelPort, IOPMROOT_ALLOW, &nid) != kIOReturnSuccess)
		return (kIOReturnError);
	return (kIOReturnSuccess);
}

/* Vetoes a kIOMessageCanSystemSleep (idle sleep); for
 * kIOMessageSystemWillSleep it counts as an acknowledgement, as on Darwin. */
IOReturn
IOCancelPowerChange(io_connect_t kernelPort, intptr_t notificationID)
{
	int64_t nid = (int64_t)notificationID;

	if (connect_ioctl(kernelPort, IOPMROOT_CANCEL, &nid) !=
	    kIOReturnSuccess)
		return (kIOReturnError);
	return (kIOReturnSuccess);
}

static IOReturn
pmroot_sleep(io_connect_t fb, uint32_t reason)
{
	struct iopmroot_sleep rq;

	memset(&rq, 0, sizeof(rq));
	rq.reason = reason;
	return (connect_ioctl(fb, IOPMROOT_SLEEP, &rq));
}

/*
 * "Software Sleep". Returns at once; the fan-out and the sleep follow.
 * kIOReturnNotPrivileged unless root, kIOReturnUnsupported where the machine
 * cannot sleep, kIOReturnNotPermitted while sleep is disabled.
 */
IOReturn
IOPMSleepSystem(io_connect_t fb)
{
	return (pmroot_sleep(fb, IOPMROOT_REASON_SOFTWARE_SLEEP));
}

/* kIOPMSleepReasonKey values (xnu IOPMPrivate.h) and the root domain's
 * reason codes. */
static const struct {
	const char	*name;
	uint32_t	 reason;
} sleep_reasons[] = {
	{ kIOPMClamshellSleepKey,	IOPMROOT_REASON_CLAMSHELL },
	{ kIOPMPowerButtonSleepKey,	IOPMROOT_REASON_POWER_BUTTON },
	{ kIOPMSoftwareSleepKey,	IOPMROOT_REASON_SOFTWARE_SLEEP },
	{ kIOPMIdleSleepKey,		IOPMROOT_REASON_IDLE },
	{ kIOPMLowPowerSleepKey,	IOPMROOT_REASON_LOW_POWER },
	{ kIOPMThermalEmergencySleepKey, IOPMROOT_REASON_THERMAL },
	{ kIOPMMaintenanceSleepKey,	IOPMROOT_REASON_MAINTENANCE },
};

/*
 * IOPMSleepSystemWithOptions: kIOPMSleepReasonKey in `options` picks the
 * reason; without it (or with an unknown one) this is IOPMSleepSystem, as
 * xnu's sleepSystemOptions falls back to a software sleep. "Idle Sleep"
 * asks kIOMessageCanSystemSleep first, so a client can veto it.
 */
IOReturn
IOPMSleepSystemWithOptions(io_connect_t fb, CFDictionaryRef options)
{
	uint32_t reason = IOPMROOT_REASON_SOFTWARE_SLEEP;
	char buf[64];
	CFTypeRef v;
	size_t i;

	if (options != NULL) {
		v = CFDictionaryGetValue(options, CFSTR(kIOPMSleepReasonKey));
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID() &&
		    CFStringGetCString(v, buf, sizeof(buf),
		    kCFStringEncodingUTF8)) {
			for (i = 0; i < sizeof(sleep_reasons) /
			    sizeof(sleep_reasons[0]); i++)
				if (strcmp(buf, sleep_reasons[i].name) == 0)
					reason = sleep_reasons[i].reason;
		}
	}
	return (pmroot_sleep(fb, reason));
}

IOReturn
IOPMSetAggressiveness(io_connect_t fb, unsigned long type,
    unsigned long aggressiveness)
{
	struct iopmroot_aggr ag;

	memset(&ag, 0, sizeof(ag));
	ag.type = (uint32_t)type;
	ag.value = (uint32_t)aggressiveness;
	if (type >= IOPMROOT_AGGR_MAX)
		return (kIOReturnBadArgument);
	return (connect_ioctl(fb, IOPMROOT_SETAGGR, &ag));
}

IOReturn
IOPMGetAggressiveness(io_connect_t fb, unsigned long type,
    unsigned long *aggressiveness)
{
	struct iopmroot_aggr ag;
	IOReturn ret;

	if (aggressiveness == NULL || type >= IOPMROOT_AGGR_MAX)
		return (kIOReturnBadArgument);
	memset(&ag, 0, sizeof(ag));
	ag.type = (uint32_t)type;
	ret = connect_ioctl(fb, IOPMROOT_GETAGGR, &ag);
	if (ret == kIOReturnSuccess)
		*aggressiveness = ag.value;
	return (ret);
}

/* Apple reads the root domain's "IOSleepSupported"; "SleepDisabled" also
 * refuses every sleep here, so it counts too. */
boolean_t
IOPMSleepEnabled(void)
{
	struct iopmroot_state st;
	int fd = __io_pmroot_fd();

	if (fd < 0 || ioctl(fd, IOPMROOT_GETSTATE, &st) != 0)
		return (false);
	return ((st.flags & IOPMROOT_F_SLEEP_SUPPORTED) != 0 &&
	    (st.flags & IOPMROOT_F_SLEEP_DISABLED) == 0);
}

/*
 * The calendar time of the last wake, from kern.waketime as on Darwin.
 * kIOReturnNotReady before the first sleep. adjustedForPhysicalWake is
 * always 0: there is no dark wake to adjust for.
 */
IOReturn
IOPMGetLastWakeTime(CFAbsoluteTime *lastWakeTime,
    CFTimeInterval *adjustedForPhysicalWake)
{
	struct timeval tv;
	size_t len = sizeof(tv);

	if (lastWakeTime == NULL || adjustedForPhysicalWake == NULL)
		return (kIOReturnBadArgument);
	if (sysctlbyname("kern.waketime", &tv, &len, NULL, 0) != 0 ||
	    len != sizeof(tv) || tv.tv_sec == 0)
		return (kIOReturnNotReady);
	*lastWakeTime = (CFAbsoluteTime)tv.tv_sec -
	    kCFAbsoluteTimeIntervalSince1970 + (CFAbsoluteTime)tv.tv_usec / 1e6;
	*adjustedForPhysicalWake = 0;
	return (kIOReturnSuccess);
}

/*
 * IORegistryEntrySetCFProperty. The root domain is the one entry that takes
 * properties (xnu's IOPMrootDomain::setProperties); the others answer
 * kIOReturnUnsupported, IORegistryEntry::setProperties' default. Values a
 * root domain key can hold: CFBoolean, CFNumber (as a 64-bit integer),
 * CFString (up to 63 bytes of UTF-8) and CFData (up to 64 bytes, e.g. the
 * IOPMCalendarStruct of "WakeByCalendarDate"; a kernel older than
 * nextbsd-kernel#245 refuses it with kIOReturnBadArgument). Root only. The request goes through the
 * process-wide descriptor, so the power-button policy a key like
 * "DisablePowerButtonSleep" claims stays with this process until it exits.
 */
static IOReturn
pmroot_setprop(CFStringRef key, CFTypeRef value)
{
	struct iopmroot_prop pr;
	int fd;

	if (key == NULL || value == NULL ||
	    CFGetTypeID(key) != CFStringGetTypeID())
		return (kIOReturnBadArgument);
	memset(&pr, 0, sizeof(pr));
	if (!CFStringGetCString(key, pr.key, sizeof(pr.key),
	    kCFStringEncodingUTF8))
		return (kIOReturnBadArgument);
	if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		pr.type = IOPMROOT_PROP_BOOL;
		pr.number = CFBooleanGetValue(value) ? 1 : 0;
	} else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		pr.type = IOPMROOT_PROP_NUMBER;
		if (!CFNumberGetValue(value, kCFNumberSInt64Type, &pr.number))
			return (kIOReturnBadArgument);
	} else if (CFGetTypeID(value) == CFStringGetTypeID()) {
		pr.type = IOPMROOT_PROP_STRING;
		if (!CFStringGetCString(value, pr.string, sizeof(pr.string),
		    kCFStringEncodingUTF8))
			return (kIOReturnBadArgument);
	} else if (CFGetTypeID(value) == CFDataGetTypeID()) {
		pr.type = IOPMROOT_PROP_DATA;
		pr.number = CFDataGetLength(value);
		if (pr.number > (int64_t)sizeof(pr.string))
			return (kIOReturnBadArgument);
		CFDataGetBytes(value, CFRangeMake(0, pr.number),
		    (UInt8 *)pr.string);
	} else
		return (kIOReturnUnsupported);

	fd = __io_pmroot_fd();
	if (fd < 0)
		return (kIOReturnNoDevice);
	if (ioctl(fd, IOPMROOT_SETPROP, &pr) != 0)
		return (errno == EBUSY ? kIOReturnExclusiveAccess :
		    pmroot_ioreturn(errno));
	return (kIOReturnSuccess);
}

kern_return_t
IORegistryEntrySetCFProperty(io_registry_entry_t entry, CFStringRef
    propertyName, CFTypeRef property)
{
	if (entry == IO_OBJECT_NULL || entry->kind != IOOBJ_KIND_ENTRY)
		return (kIOReturnBadArgument);
	if (!is_pmroot(entry))
		return (kIOReturnUnsupported);
	return (pmroot_setprop(propertyName, property));
}

struct setprops_ctx {
	IOReturn	ret;
};

static void
setprops_one(const void *key, const void *value, void *context)
{
	struct setprops_ctx *ctx = context;
	IOReturn ret;

	ret = pmroot_setprop((CFStringRef)key, (CFTypeRef)value);
	if (ret != kIOReturnSuccess && ctx->ret == kIOReturnSuccess)
		ctx->ret = ret;
}

/* A dictionary of properties, each set as IORegistryEntrySetCFProperty
 * would; the first failure is returned, the other keys are still set. */
kern_return_t
IORegistryEntrySetCFProperties(io_registry_entry_t entry, CFTypeRef properties)
{
	struct setprops_ctx ctx;

	if (entry == IO_OBJECT_NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    properties == NULL ||
	    CFGetTypeID(properties) != CFDictionaryGetTypeID())
		return (kIOReturnBadArgument);
	if (!is_pmroot(entry))
		return (kIOReturnUnsupported);
	ctx.ret = kIOReturnSuccess;
	CFDictionaryApplyFunction((CFDictionaryRef)properties, setprops_one,
	    &ctx);
	return (ctx.ret);
}
