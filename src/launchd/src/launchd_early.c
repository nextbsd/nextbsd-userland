/*
 * launchd_early.c — freebsd-launchd-mach PID-1 early-init helpers.
 *
 * Two boot-readiness primitives the LaunchDaemon scan needs done
 * BEFORE it dispatches any plist:
 *
 *  1. launchd_early_sethostname() — synthesize a hostname from the
 *     SMBIOS model slug (e.g. "ThinkPad-T460s"), sethostname(2) it.
 *     getty (system_cmds/getty/main.c:202) reads gethostname() once
 *     at process start and caches it in a process-global; without
 *     this early set, getty's first login banner shows the FreeBSD
 *     default "Amnesiac" until the user presses return (which exits
 *     getty, launchd respawns, and the new getty reads the updated
 *     hostname). hostnamed's own boot-time sethostname runs too late:
 *     launchd dispatches hostnamed and getty in parallel, getty wins
 *     the race. PID 1 doing the synth+sethostname BEFORE plist
 *     dispatch is the cleanest fix — hostnamed then refines the
 *     value later through its full SCPrefs/DHCP/PTR/mDNS chain.
 *
 *  2. launchd_early_open_klog() — open(/dev/klog, O_RDONLY) and
 *     leak the fd. kern/subr_log.c:104-124 logopen() flips log_open
 *     to 1 on first open; once set, kern/subr_prf.c:321 vlog() routes
 *     to TOLOG only, NOT TOCONS|TOLOG. Without an early klog reader,
 *     kernel printf()s (e.g. nd6_dad_timer messages) bleed into
 *     /dev/console mid-getty-prompt. Stock FreeBSD's syslogd opens
 *     /dev/klog at startup and the kernel goes quiet on console;
 *     our syslogd has klog_in deactivated (syslog/syslogd.tproj/
 *     syslogd.c:90, task #41 libdispatch+Mach deadlock workaround),
 *     so PID 1 takes ownership of the log_open flip itself. The fd
 *     is intentionally leaked — we just need logopen() to fire once.
 *
 * Synthesis comes from the header-only nextbsd_hostname_synth.h, the
 * same code hostnamed's freebsd_synthesize_hostname (shim.c) uses, so
 * the early name getty caches equals the one hostnamed publishes. It is
 * included rather than linked because PID 1 is the foundation: every
 * transitive .so it pulls is one more thing that has to be present and
 * loadable on a degraded boot. Header-only C means no CoreFoundation, no
 * SCDS, no Libnotify; only libc + libthr. (nextbsd/nextbsd#325: the two
 * used to be separate copies and drifted.)
 *
 * Both helpers are best-effort: failures log to launchd_console and
 * boot continues. They are not the sole or final hostname / log
 * routing surface — they're a quality-of-life floor.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <kenv.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "launchd_early.h"
#include "nextbsd_hostname_synth.h"

int
launchd_early_sethostname(char *out, size_t outsz)
{
	char name[NBHS_NAME_MAX + 1];

	/* Must be byte-identical to hostnamed's freebsd_synthesize_hostname
	 * (both call nbhs_synthesize) so the kernel hostname set here, the
	 * name hostnamed publishes to SCDS, the DHCP host-name option
	 * ipconfigd sends, and mDNSResponder's .local label all agree.
	 * e.g. "ThinkPad-T460s", or "VirtualBox-68f9a871" for a VM. */
	nbhs_synthesize(name, sizeof(name));
	if (sethostname(name, (int)strlen(name)) != 0)
		return (-1);
	if (out != NULL && outsz > 0) {
		(void)strncpy(out, name, outsz - 1);
		out[outsz - 1] = '\0';
	}
	return (0);
}

int
launchd_early_open_klog(void)
{
	int fd;

	fd = open("/dev/klog", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	/*
	 * Deliberately leak the fd. kern.log_open is set by the first
	 * open(2) on /dev/klog (subr_log.c:104), and we want it to stay
	 * 1 for the lifetime of PID 1. We don't drain — kernel msgbuf
	 * sizes itself and rolls without back-pressure when nobody
	 * reads. Once syslogd's klog_in module is re-enabled (task #41),
	 * it will open a second reader; multi-reader /dev/klog is
	 * supported by FreeBSD's logread() (subr_log.c).
	 */
	return (fd);
}
