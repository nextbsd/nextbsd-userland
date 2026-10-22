/*
 * launchd_early.c — freebsd-launchd-mach PID-1 early-init helpers.
 *
 * Boot-readiness work PID 1 does BEFORE any plist is dispatched,
 * and one thing it deliberately does not do:
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
 *  2. No /dev/klog reader. PID 1 used to open /dev/klog here and leak
 *     the fd so the kernel's log() went TOLOG only and stopped
 *     bleeding onto /dev/console while syslogd's klog_in was off.
 *     As on Darwin, syslogd owns /dev/klog: klog_in is enabled again
 *     (nextbsd/nextbsd-userland#383), and kern/subr_log.c logopen()
 *     allows a single reader, so a PID-1 reader made klog_in's open
 *     fail with EBUSY and kernel messages never reached system.log
 *     (#169). A normal boot keeps the console quiet through the
 *     kernel's cn_mute instead (nextbsd/nextbsd-kernel#291); on a
 *     verbose boot kernel output on the console is expected.
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
 * The helper is best-effort: failures log to launchd_console and
 * boot continues. It is not the sole or final hostname surface —
 * it's a quality-of-life floor.
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
