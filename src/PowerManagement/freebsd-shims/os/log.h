/*
 * os/log.h — NextBSD shim for Apple's <os/log.h>, for pmconfigd.
 *
 * Apple's os_log writes to the unified log through libsystem_trace, which is
 * closed source and not ported. pmconfigd has ~70 os_log sites, and their
 * format strings use os_log's extensions to printf:
 *
 *   %{public}s, %{private}d, %{bool}d, %{errno}d ...  privacy / type
 *                                                      annotations
 *   %@                                                 a CFTypeRef
 *
 * Neither is valid printf, so the macros cannot simply expand to fprintf or
 * syslog (a %@ there reads a pointer as a string). They expand to
 * _pm_os_log(), in os_log_shim.c, which drops the {annotation}, prints %@
 * through CFCopyDescription, and hands every other conversion to vsnprintf.
 *
 * Output goes to stderr, which launchd sends to /var/log/powerd.stderr
 * (StandardErrorPath in org.nextbsd.powerd.plist), never the console. That
 * is the same sink every NextBSD daemon uses. Moving it to ASL later is a
 * change to os_log_shim.c alone; no caller changes.
 *
 * os_log_debug() is compiled out, as on macOS where debug messages are not
 * persisted by default.
 */
#ifndef _PM_SHIM_OS_LOG_H_
#define _PM_SHIM_OS_LOG_H_

#include <stdint.h>

typedef struct pm_os_log_s *os_log_t;

typedef uint8_t os_log_type_t;
#define OS_LOG_TYPE_DEFAULT	0x00
#define OS_LOG_TYPE_INFO	0x01
#define OS_LOG_TYPE_DEBUG	0x02
#define OS_LOG_TYPE_ERROR	0x10
#define OS_LOG_TYPE_FAULT	0x11

#define OS_LOG_DEFAULT		((os_log_t)0)
#define OS_LOG_DISABLED		((os_log_t)-1)

#ifdef __cplusplus
extern "C" {
#endif

void	_pm_os_log(os_log_t log, os_log_type_t type, const char *fmt, ...)
	    __attribute__((__visibility__("hidden")));
os_log_t _pm_os_log_create(const char *subsystem, const char *category)
	    __attribute__((__visibility__("hidden")));

#ifdef __cplusplus
}
#endif

#define os_log_create(subsystem, category) \
	_pm_os_log_create((subsystem), (category))

#define os_log_with_type(log, type, fmt, ...) \
	_pm_os_log((log), (type), (fmt), ##__VA_ARGS__)
#define os_log(log, fmt, ...) \
	_pm_os_log((log), OS_LOG_TYPE_DEFAULT, (fmt), ##__VA_ARGS__)
#define os_log_info(log, fmt, ...) \
	_pm_os_log((log), OS_LOG_TYPE_INFO, (fmt), ##__VA_ARGS__)
#define os_log_error(log, fmt, ...) \
	_pm_os_log((log), OS_LOG_TYPE_ERROR, (fmt), ##__VA_ARGS__)
#define os_log_fault(log, fmt, ...) \
	_pm_os_log((log), OS_LOG_TYPE_FAULT, (fmt), ##__VA_ARGS__)
#define os_log_debug(log, fmt, ...)	((void)(log))

#define os_log_type_enabled(log, type) \
	((log) != OS_LOG_DISABLED && (type) != OS_LOG_TYPE_DEBUG)
#define os_log_debug_enabled(log)	0
#define os_log_info_enabled(log)	((log) != OS_LOG_DISABLED)

#endif /* _PM_SHIM_OS_LOG_H_ */
