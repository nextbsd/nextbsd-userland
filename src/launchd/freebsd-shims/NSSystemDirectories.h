/*
 * freebsd-launchd-mach (2026-05-15)
 *
 * Stub NSSystemDirectories for launchctl. Apple's launchctl uses
 * NSStartSearchPathEnumeration / NSGetNextSearchPathEnumeration to
 * walk the Library directory hierarchy (~/Library, /Local/Library,
 * /Network/Library, /System/Library) looking for LaunchAgents and
 * LaunchDaemons subdirs.
 *
 * Stub returns ~/Library, /Local/Library and /System/Library, in
 * Darwin's order (user, local, network, system), for the domains set in
 * the mask. /Local/Library is NextBSD's local domain. /Network/Library
 * is skipped (no NetInfo / OD).
 *
 * The enumeration state is a bitmask of the steps still to produce (see
 * NSD_STEP_* below); each call emits the first remaining step and clears it.
 * The call after the last step returns 0 WITHOUT writing a path.
 *
 * The terminating call must not write a path. Apple's callers test the
 * return value before touching the buffer:
 *
 *     while ((es = NSGetNextSearchPathEnumeration(es, nspath))) { ... }
 *
 * so a call that fills 'path' and returns 0 has its path silently
 * dropped -- the loop exits before the body runs. Returning 0 on the
 * /System/Library step is what made `launchctl load -D all` search only
 * /Local/Library and never see the OS daemons.
 *
 * ~/Library is returned literally. Apple's launchctl globs the result
 * with GLOB_TILDE, which expands it to the caller's home directory.
 */

#ifndef _FREEBSD_LAUNCHD_MACH_NSSYSTEMDIRECTORIES_H_
#define _FREEBSD_LAUNCHD_MACH_NSSYSTEMDIRECTORIES_H_

#include <string.h>
#include <sys/param.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Apple's enum values. We only honor NSLibraryDirectory; the others
 * exist so the enum compiles, but launchctl doesn't pass them. */
typedef enum {
        NSApplicationDirectory          = 1,
        NSDemoApplicationDirectory      = 2,
        NSDeveloperApplicationDirectory = 3,
        NSAdminApplicationDirectory     = 4,
        NSLibraryDirectory              = 5,
        NSDeveloperDirectory            = 6,
        NSUserDirectory                 = 7,
        NSDocumentationDirectory        = 8,
        NSDocumentDirectory             = 9,
        NSAllApplicationsDirectory      = 100,
        NSAllLibrariesDirectory         = 101,
} NSSearchPathDirectory;

typedef enum {
        NSUserDomainMask        = 1,
        NSLocalDomainMask       = 2,
        NSNetworkDomainMask     = 4,
        NSSystemDomainMask      = 8,
        NSAllDomainsMask        = 0x0ffff,
} NSSearchPathDomainMask;

/*
 * Apple makes this opaque. We use it as a bitmask of remaining steps, so an
 * empty mask is representable as 0 -- which is what terminates the walk.
 */
typedef unsigned int NSSearchPathEnumerationState;

#define NSD_STEP_LOCAL          0x1     /* /Local/Library  */
#define NSD_STEP_SYSTEM         0x2     /* /System/Library */
#define NSD_STEP_USER           0x8     /* ~/Library       */
/*
 * Sentinel meaning "a path was just written, but nothing remains".
 *
 * Apple's callers loop as `while ((es = Next(es, path)))`, so the state a
 * call RETURNS gates whether the path it just wrote gets used. Returning 0
 * on the final directory would silently discard it. The last real step
 * therefore returns this, and the following call returns 0 writing nothing.
 */
#define NSD_STEP_DONE           0x4

/*
 * Initialize enumeration.
 *
 * The MASK IS LOAD-BEARING and must be honoured. Apple's callers rely on an
 * empty mask producing an empty enumeration:
 *
 *     es = 0;                          // no -D given
 *     ... getopt sets bits only for -D ...
 *     es = NSStartSearchPathEnumeration(NSLibraryDirectory, es);
 *     while ((es = NSGetNextSearchPathEnumeration(es, nspath))) {
 *             strcat(nspath, "/LaunchDaemons");
 *             glob(nspath, ...);       // readpath() every plist found
 *     }
 *     for (i = 0; i < argc; i++) readpath(argv[i], &lus);
 *
 * So for `launchctl unload /path/to/one.plist` with no -D, mask is 0, the
 * loop body never runs, and only the named file is acted on.
 *
 * This stub previously ignored the mask and returned 1 unconditionally, so
 * the loop ALWAYS ran: every `launchctl load` or `unload` globbed both
 * LaunchDaemons directories and added EVERY plist on the system to the set.
 * `launchctl unload <one job>` therefore unloaded everything -- syslogd and
 * sshd included -- and took the machine down. Reproduced twice on hardware
 * before the cause was found (#113).
 *
 * Domains we can serve, and the step each maps to, in walk order:
 *   NSUserDomainMask   (1) -> ~/Library
 *   NSLocalDomainMask  (2) -> /Local/Library
 *   NSSystemDomainMask (8) -> /System/Library
 *
 * NSNetworkDomainMask is not supported (no OD), and is simply absent from
 * the walk rather than silently substituted.
 *
 * launchctl load clears NSUserDomainMask unless a session type (-S) was
 * given, so the system bootstrap (`load -D all`, no -S) never walks
 * ~/Library, as on Darwin.
 *
 * The returned state encodes which steps remain, so an empty mask yields 0
 * and terminates immediately.
 */
static inline NSSearchPathEnumerationState
NSStartSearchPathEnumeration(NSSearchPathDirectory dir,
    NSSearchPathDomainMask mask)
{
        NSSearchPathEnumerationState st = 0;

        /* Only NSLibraryDirectory is meaningful here. */
        if (dir != NSLibraryDirectory && dir != NSAllLibrariesDirectory)
                return 0;

        if (mask & NSUserDomainMask)
                st |= NSD_STEP_USER;
        if (mask & NSLocalDomainMask)
                st |= NSD_STEP_LOCAL;
        if (mask & NSSystemDomainMask)
                st |= NSD_STEP_SYSTEM;

        return st;
}


/*
 * Advance enumeration. `state` is the set of steps still to produce; each bit
 * is cleared as its path is emitted.
 *
 * Callers test the RETURN before using the buffer:
 *
 *     while ((es = NSGetNextSearchPathEnumeration(es, path))) { use(path); }
 *
 * so the call that writes the final directory must still return non-zero.
 * NSD_STEP_DONE serves that purpose; the next call returns 0 and writes
 * nothing.
 */
static inline NSSearchPathEnumerationState
NSGetNextSearchPathEnumeration(NSSearchPathEnumerationState state,
    char path[/* PATH_MAX */])
{
        NSSearchPathEnumerationState rest;

        if (state & NSD_STEP_USER) {
                strlcpy(path, "~/Library", MAXPATHLEN);
                rest = state & ~NSD_STEP_USER;
                return rest ? rest : NSD_STEP_DONE;
        }
        if (state & NSD_STEP_LOCAL) {
                strlcpy(path, "/Local/Library", MAXPATHLEN);
                rest = state & ~NSD_STEP_LOCAL;
                return rest ? rest : NSD_STEP_DONE;
        }
        if (state & NSD_STEP_SYSTEM) {
                strlcpy(path, "/System/Library", MAXPATHLEN);
                rest = state & ~NSD_STEP_SYSTEM;
                return rest ? rest : NSD_STEP_DONE;
        }
        /* NSD_STEP_DONE, or an empty/exhausted state: write nothing. */
        return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _FREEBSD_LAUNCHD_MACH_NSSYSTEMDIRECTORIES_H_ */
