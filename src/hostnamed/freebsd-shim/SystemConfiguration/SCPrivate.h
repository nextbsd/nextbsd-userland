/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * Re-export SCPrivate shim under the <SystemConfiguration/...> path,
 * layered over libSystemConfiguration's installed SCPrivate.h (the
 * _SC_NOTIFY_* names). The shim adds the SPI subset the library does
 * not export yet.
 */
#ifndef _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_INDIRECT_H_
#define _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_INDIRECT_H_
#include_next <SystemConfiguration/SCPrivate.h>
#include "../SCPrivate.h"
#endif
