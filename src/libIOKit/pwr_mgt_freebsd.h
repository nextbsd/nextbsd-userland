/*
 * pwr_mgt_freebsd.h — private to libIOKit: what IOKitUser's pwr_mgt.subproj
 * sources (IOPMAssertions.c, IOPMLibPrivate.c, IOPMEnergyPrefs.c) take from
 * Darwin frameworks and private headers that NextBSD's base system does not
 * have. Each of those files includes this header under __FreeBSD__ in place
 * of the Darwin-only includes; the code that uses the names below is Apple's,
 * unchanged.
 *
 *  - os_log: NextBSD has no unified log, and libIOKit must not write to a
 *    client's stdout/stderr or the console, so the library's log calls
 *    compile to nothing (pmconfigd logs the server side of every call).
 *  - os_unfair_lock: a pthread mutex.
 *  - energytrace: no energy tracing; the calls do nothing.
 *  - __CFRunLoopSetOptionsReason: CoreFoundation's App Nap hook. NextBSD has
 *    no App Nap, so a process that holds an assertion has nothing to opt
 *    out of.
 *  - IOSystemConfiguration.h: IOKitUser's wrapper that loads
 *    SystemConfiguration lazily on embedded platforms; on macOS it is the
 *    SystemConfiguration headers plus SCValidation.h's isA_* helpers, which
 *    NextBSD's libSystemConfiguration does not install.
 *  - CFXPCBridge.h: NextBSD's CoreFoundation has no CF <-> XPC object
 *    bridge. The few calls that carry a CF object over XPC are left out
 *    where they appear.
 *  - IORegistryEntryFromPath: IOKitUser finds the root domain by its
 *    IOPower plane path. NextBSD's registry has the IOService plane only,
 *    where the root domain is the one IOPMrootDomain entry, so that path is
 *    looked up by class, as IOPMLib.c does; no other path is known.
 */
#ifndef _LIBIOKIT_PWR_MGT_FREEBSD_H_
#define _LIBIOKIT_PWR_MGT_FREEBSD_H_

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <IOKit/IOKitLib.h>
#include <Block.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---- os/log.h ----------------------------------------------------------- */
typedef void *os_log_t;
#define OS_LOG_DEFAULT				((os_log_t)0)
#define os_log_create(subsystem, category)	OS_LOG_DEFAULT
#define os_log(log, fmt, ...)			((void)(log))
#define os_log_info(log, fmt, ...)		((void)(log))
#define os_log_debug(log, fmt, ...)		((void)(log))
#define os_log_error(log, fmt, ...)		((void)(log))
#define os_log_fault(log, fmt, ...)		((void)(log))

/* ---- os/lock_private.h -------------------------------------------------- */
typedef pthread_mutex_t os_unfair_lock;
#define OS_UNFAIR_LOCK_INIT			PTHREAD_MUTEX_INITIALIZER
#define os_unfair_lock_lock(l)			((void)pthread_mutex_lock(l))
#define os_unfair_lock_unlock(l)		((void)pthread_mutex_unlock(l))

/* ---- energytrace.h ------------------------------------------------------ */
enum {
	kEnTrCompSysPower = 0,
	kEnTrActSPPMAssertion = 0,
	kEnTrModSPRetain = 0,
	kEnTrModSPRelease = 0,
	kEnTrQualNone = 0,
	kEnTrQualSPKeepSystemAwake = 0,
	kEnTrValNone = 0
};
#define entr_act_begin(comp, act, id, qual, val)	((void)(id))
#define entr_act_modify(comp, act, id, mod, val)	((void)(id))
#define entr_act_end(comp, act, id, qual, val)		((void)(id))

/* ---- CoreFoundation/CFPriv.h: App Nap ----------------------------------- */
enum {
	__CFRunLoopOptionsTakeAssertion = 1,
	__CFRunLoopOptionsDropAssertion = 2
};
#define __CFRunLoopSetOptionsReason(opts, reason)	((void)(reason))

/* ---- SystemConfiguration/SCValidation.h --------------------------------- */
#ifndef isA_CFType
static __inline CFTypeRef
isA_CFType(CFTypeRef obj, CFTypeID type)
{
	if (obj == NULL || CFGetTypeID(obj) != type)
		return (NULL);
	return (obj);
}
#define isA_CFArray(obj)	((CFArrayRef)isA_CFType((obj), CFArrayGetTypeID()))
#define isA_CFBoolean(obj)	((CFBooleanRef)isA_CFType((obj), CFBooleanGetTypeID()))
#define isA_CFData(obj)		((CFDataRef)isA_CFType((obj), CFDataGetTypeID()))
#define isA_CFDate(obj)		((CFDateRef)isA_CFType((obj), CFDateGetTypeID()))
#define isA_CFDictionary(obj)	((CFDictionaryRef)isA_CFType((obj), CFDictionaryGetTypeID()))
#define isA_CFNumber(obj)	((CFNumberRef)isA_CFType((obj), CFNumberGetTypeID()))
#define isA_CFString(obj)	((CFStringRef)isA_CFType((obj), CFStringGetTypeID()))
#endif

/* ---- CoreFoundation/CFMachPort.h ---------------------------------------- */
/*
 * NextBSD's CoreFoundation has no CFMachPort. IOPMConnection keeps one for
 * run-loop delivery; IOPMLibPrivate.c never creates it on NextBSD (it offers
 * dispatch-queue delivery only), so the type is opaque and has no port.
 */
typedef struct __CFMachPort *CFMachPortRef;
static __inline mach_port_t
CFMachPortGetPort(CFMachPortRef port __unused)
{
	return (MACH_PORT_NULL);
}

/* ---- IOKit/IOTypes.h, which libIOKit does not have (see IOPM.h) --------- */
enum {
	kNilOptions = 0
};

/* ---- IOKitLib.h: the root domain's IOPower plane path ------------------- */
#define kIOPowerPlane		"IOPower"
#define kIOPMRootDomainPowerPlanePath \
	kIOPowerPlane ":/IOPowerConnection/IOPMrootDomain"

static __inline io_registry_entry_t
__pm_IORegistryEntryFromPath(mach_port_t mainPort, const char *path)
{
	if (path == NULL || strcmp(path, kIOPMRootDomainPowerPlanePath) != 0)
		return (IO_OBJECT_NULL);
	return (IOServiceGetMatchingService(mainPort,
	    IOServiceMatching("IOPMrootDomain")));
}
#define IORegistryEntryFromPath		__pm_IORegistryEntryFromPath

#endif /* !_LIBIOKIT_PWR_MGT_FREEBSD_H_ */

