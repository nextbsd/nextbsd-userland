/*
 * nextbsd_verbose_boot.h — NextBSD's stand-in for Darwin's "-v" / "-s"
 * kern.bootargs checks, shared by launchd (PID 1, runtime.c do_file_init)
 * and launchctl (launchctl.c do_file_init).
 *
 * NextBSD has no kern.bootargs, so those checks never matched and every boot
 * was treated as non-verbose. A boot is verbose here if the loader set
 * RB_VERBOSE (boot -v, debug.bootverbose) or RB_SINGLE (boot -s,
 * debug.boothowto). That is launchctl-842's rule ("-v" or "-s"); on a Mac,
 * single-user also shows the full text console.
 *
 * On a non-verbose boot launchd keeps the console closed and its bootstrap
 * job's stdio on /dev/null, so nothing reaches the screen before the login
 * prompt (nextbsd/nextbsd#540). Messages still reach syslog through the
 * launchd log drain (nextbsd/nextbsd#541).
 *
 * Header-only and libc-only (static inline): PID 1 must not pick up another
 * shared library for this.
 */
#ifndef _NEXTBSD_VERBOSE_BOOT_H_
#define _NEXTBSD_VERBOSE_BOOT_H_

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <stdbool.h>

static inline bool
nextbsd_verbose_boot(void)
{
	int val = 0;
	size_t len = sizeof(val);

	if (sysctlbyname("debug.bootverbose", &val, &len, NULL, 0) == 0 &&
	    val != 0) {
		return true;
	}

	val = 0;
	len = sizeof(val);
	if (sysctlbyname("debug.boothowto", &val, &len, NULL, 0) == 0 &&
	    (val & RB_SINGLE) != 0) {
		return true;
	}

	return false;
}
#endif /* __FreeBSD__ */

#endif /* _NEXTBSD_VERBOSE_BOOT_H_ */
