/*
 * launchctl_freebsd_compat.h — freebsd-launchd-mach (2026-05-15)
 *
 * Compatibility defines for launchctl.c. Force-included via the
 * Makefile (-include) so launchctl.c sees these identifiers without
 * patching its source.
 *
 * Each define is paired with a brief justification. The shimmed
 * values let the affected functions compile; at runtime they either
 * return safely-failing values (sysctl ENOENT) or touch a harmless
 * file path.
 */
#ifndef _FREEBSD_LAUNCHD_MACH_LAUNCHCTL_COMPAT_H_
#define _FREEBSD_LAUNCHD_MACH_LAUNCHCTL_COMPAT_H_

/* sys/wait.h
 *   launchctl.c uses WIFEXITED, WIFSIGNALED, WEXITSTATUS, WTERMSIG,
 *   waitpid in 8+ sites without including <sys/wait.h>. On macOS the
 *   header gets pulled in transitively via <sys/types.h> or similar;
 *   FreeBSD requires explicit inclusion. Adding it here keeps
 *   launchctl.c source unchanged.
 */
#include <sys/wait.h>

/* _PATH_UTMPX (line 2441 in launchctl.c)
 *   touch_file(_PATH_UTMPX, DEFFILEMODE) — bumps the utmpx login
 *   record at boot. FreeBSD's <paths.h> doesn't define _PATH_UTMPX
 *   (utmpx file lives at /var/log/utx.* but the path macro is private
 *   to libulog). Pointing at /var/empty makes touch_file a no-op.
 */
#ifndef _PATH_UTMPX
#define _PATH_UTMPX     "/var/empty/utmpx"
#endif

/* KERN_SAFEBOOT / KERN_NETBOOT
 *   Not defined here on purpose. is_safeboot() and is_netboot() in
 *   launchctl.c return false under __FreeBSD__ without calling sysctl(2):
 *   the old approach (alias to a guaranteed-ENOENT MIB) made
 *   posix_assumes_zero log to the console on every boot (nextbsd#324).
 *   See docs/launchd-darwin-probes.md.
 */

/* KERN_OSVERSION (line 4364)
 *   do_sysversion_sysctl() reads kern.osversion to print the macOS
 *   build number ("24A335" etc.). FreeBSD's analogue is
 *   KERN_OSRELEASE (returns "15.0-RELEASE"); not byte-identical but
 *   same protocol (string sysctl). Alias.
 */
#ifndef KERN_OSVERSION
#define KERN_OSVERSION  KERN_OSRELEASE
#endif

/* SO_EXECPATH (line 4447)
 *   setsockopt(SOL_SOCKET, SO_EXECPATH, ...) — Apple-only socket
 *   option that tags the socket with the binary path allowed to
 *   inherit it. The launchctl call site explicitly tolerates
 *   ENOPROTOOPT (errno-check below the call). Use an unused
 *   socket-option value so the kernel returns ENOPROTOOPT.
 */
#ifndef SO_EXECPATH
#define SO_EXECPATH     0xfffe
#endif

#endif /* _FREEBSD_LAUNCHD_MACH_LAUNCHCTL_COMPAT_H_ */
