/*
 * ps_freebsd.h — private to libIOKit: what IOKitUser's ps.subproj sources
 * (IOPowerSources.c, IOPowerSourcesPrivate.c) take from Darwin frameworks
 * and private headers that NextBSD's base system does not have. Both files
 * include this header under __FreeBSD__ in place of the Darwin-only
 * includes; the code that uses the names below is Apple's.
 *
 *  - os_log and asl_log: NextBSD has no unified log, and libIOKit must not
 *    write to a client's stdout/stderr or the console (libsystem_asl is
 *    built after libIOKit), so the library's log calls compile to nothing.
 *    pmconfigd logs the server side of every call.
 *  - IOSystemConfiguration.h: IOKitUser's wrapper that loads
 *    SystemConfiguration lazily on embedded platforms; on macOS it is the
 *    SystemConfiguration headers plus SCValidation.h's isA_* helpers, which
 *    NextBSD's libSystemConfiguration does not install.
 *    SCDynamicStoreKeyCreate, which NextBSD's SystemConfiguration lacks,
 *    is the CFStringCreateWithFormat it amounts to.
 *  - IOPMLibPrivate.h keys the ps sources use (kIOUserAssertionReSync,
 *    kIOPMServerBootstrapName), guarded, until libIOKit carries IOKitUser's
 *    full IOPMLibPrivate.h (nextbsd-userland#233).
 */
#ifndef _LIBIOKIT_PS_FREEBSD_H_
#define _LIBIOKIT_PS_FREEBSD_H_

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <Block.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- os/log.h ----------------------------------------------------------- */
#ifndef OS_LOG_DEFAULT
typedef void *os_log_t;
#define OS_LOG_DEFAULT				((os_log_t)0)
#define os_log(log, fmt, ...)			((void)(log))
#define os_log_error(log, fmt, ...)		((void)(log))
#endif

/* ---- asl.h -------------------------------------------------------------- */
#define asl_log(client, msg, level, fmt, ...)	((void)0)

/* ---- IOSystemConfiguration.h + SystemConfiguration/SCValidation.h ------- */
#define _io_kSCDynamicStoreDomainState		kSCDynamicStoreDomainState
#define SCDynamicStoreKeyCreate(allocator, fmt, ...) \
	CFStringCreateWithFormat((allocator), NULL, (fmt), __VA_ARGS__)

#ifndef isA_CFType
static __inline CFTypeRef
isA_CFType(CFTypeRef obj, CFTypeID type)
{
	if (obj == NULL || CFGetTypeID(obj) != type)
		return (NULL);
	return (obj);
}
#define isA_CFType		isA_CFType
#endif
#ifndef isA_CFArray
#define isA_CFArray(obj)	isA_CFType((obj), CFArrayGetTypeID())
#endif
#ifndef isA_CFDictionary
#define isA_CFDictionary(obj)	isA_CFType((obj), CFDictionaryGetTypeID())
#endif
#ifndef isA_CFString
#define isA_CFString(obj)	isA_CFType((obj), CFStringGetTypeID())
#endif

/* ---- IOKit/pwr_mgt/IOPMLibPrivate.h ------------------------------------- */
#ifndef kIOUserAssertionReSync
#define kIOUserAssertionReSync	"com.apple.system.powermanagement.assertionresync"
#endif
#ifndef kIOPMServerBootstrapName
#define kIOPMServerBootstrapName	"com.apple.PowerManagement.control"
#endif

/* IOPMLibPrivate.c: the shared connection to pmconfigd. */
IOReturn _pm_connect(mach_port_t *newConnection);
IOReturn _pm_disconnect(mach_port_t connection);

#endif /* _LIBIOKIT_PS_FREEBSD_H_ */
