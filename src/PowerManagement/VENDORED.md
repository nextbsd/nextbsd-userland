# Vendored: Apple PowerManagement (pmconfigd)

**Upstream:** https://github.com/apple-oss-distributions/PowerManagement
**Upstream tag:** `PowerManagement-1846.120.8.0.1`
**Upstream commit:** `dc081c6cca8184bd1f49987ac70a800f2b19b0de`
**Imported:** 2026-11-02 (nextbsd-userland#224, E17 D1)
**License:** APSL 2.0 (see `APPLE_LICENSE`)

The whole tree is imported as Apple ships it, minus `.git`. Apple's files are
unmodified except `pmconfigd/Makefile` (below) and the files listed under
"Apple files changed"; a diff against a checkout of the upstream tag shows
exactly those plus the NextBSD files listed at the end.

Also vendored, into `freebsd-shims/IOKit/pwr_mgt/`:

| File | Upstream |
|---|---|
| `powermanagement.defs` | apple-oss-distributions/IOKitUser @ `IOKitUser-100231.120.3` (commit `23a611d9d194b63465f6df2abfdda63d71c87e97`), `pwr_mgt.subproj/powermanagement.defs` |
| `powermanagement_mig.h` | same tag, `pwr_mgt.subproj/powermanagement_mig.h`, unmodified |
| `IOPMLibDefs.h` | apple-oss-distributions/xnu @ `xnu-12377.121.6` (commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`), `iokit/IOKit/pwr_mgt/IOPMLibDefs.h`, unmodified: the root domain's user-client selectors (`kPM*`) |

`pmconfigd/powermanagement.defs` is a one-line
`#include <IOKit/pwr_mgt/powermanagement.defs>`: Apple builds the daemon
against the copy in the IOKit SDK. Here that copy sits at the same include
path under `freebsd-shims/`, and it is the one description of the wire for
both ends, pmconfigd's server and libIOKit's IOPMLib client. It drops
`UseSpecialReplyPort 1;`, as configd's `config.defs` and Libnotify's
`notify_ipc.defs` do; the routine order (the msgh_ids from 73000) is unchanged.

## What is built

The shipping macOS target is `powerd` in `PowerManagement.xcodeproj`, and the
Xcode project, not Apple's `pmconfigd/Makefile`, is the source of truth for
what it contains. That Makefile predates the Xcode project (i386/ppc, files
that no longer exist), so it has been **replaced** by NextBSD's bsd.prog.mk
Makefile, which `build-userland.sh` drives. It is the one Apple file
overwritten in place.

The daemon is ported in stages (epic nextbsd/nextbsd#490). The first stage,
D1, builds a pmconfigd that runs under launchd as `org.nextbsd.powerd`, checks
in Apple's two MachServices and stays up:

- `com.apple.PowerManagement.control`: the MIG server, generated from
  `powermanagement.defs`. Every routine answers `kIOReturnUnsupported` until
  the Apple file that implements it is built.
- `com.apple.iokit.powerdxpc`: the XPC listener. Peers are accepted and
  their messages dropped until the subsystems they address are built.

Later stages add Apple's files to `SRCS` in `pmconfigd/Makefile` and delete
the matching NextBSD stand-in from `freebsd-shims/`.

S2 (nextbsd-userland#238) adds `AutoWakeScheduler.c` and
`RepeatingAutoWake.c`: `pmset schedule` / `pmset repeat`, persisted through
SCPreferences to `com.apple.AutoWake.plist` in
`/Local/Library/Preferences/SystemConfiguration/`.

D2 (nextbsd-userland#226) adds the one kernel shim, `RootDomain_freebsd.c`.
pmconfigd reaches the in-kernel IOPMrootDomain (nextbsd-kernel#243) as it
does on macOS: Apple's `IOServiceOpen` / `IOConnectCallMethod` calls on the
root domain are routed there by `PrivateLib_freebsd.h`, and each `kPM*`
selector becomes a `/dev/iopmroot` ioctl or the libIOKit IOPMLib call over
it. Root-domain properties go through libIOKit's
`IORegistryEntrySetCFProperty`, and the root domain's general interest
(clamshell changes) through `IOServiceAddInterestNotification`.

## Apple files changed

Each change is under `#ifdef __FreeBSD__`, with Apple's code kept in the
`#else`:

| File | Change |
|---|---|
| `pmconfigd/AutoWakeScheduler.c` | includes `PrivateLib_freebsd.h` in place of `PrivateLib.h`, `PMAssertions.h` and `PMSettings.h` |
| `pmconfigd/RepeatingAutoWake.h` | the same, for `PrivateLib.h` |
| `pmconfigd/RepeatingAutoWake.c` | the same; the Gregorian `CFCalendar` calls become `localtime_r(3)` / `mktime(3)` in the same local time zone |

The Xcode project also builds `pmset`, `caffeinate`, `pmtool`, `ioupsd` and
the Low Power Mode and battery-health pieces. None is built from this tree
yet. `src/caffeinate` is a separate, older import (PowerManagement-1049).

## NextBSD files

All in `freebsd-shims/` unless noted (the Libnotify pattern):

| File | Purpose |
|---|---|
| `pmconfigd/Makefile` | the build (replaces Apple's stale Makefile, see above) |
| `pmconfigd/pmconfigd.8` | manual page; Apple ships none for powerd |
| `pmconfigd_freebsd.c` | `main()`, `powerd_init()` and the two listeners, in Apple's shape, until `pmconfigd.m` and `PrivateLib.m` build |
| `powermanagement_stubs.c` | the `_io_*` MIG routines whose Apple files are not built yet; `io_pm_assertion_copy_details` serves only the two scheduled-event selectors |
| `PrivateLib_freebsd.h` | the share of `PrivateLib.h` (and the IOKit private headers it includes) that the ported Apple files use, until `PrivateLib.h` builds |
| `PrivateLib_freebsd.c` | `PrivateLib.m` / `PMAssertions.c` functions those files call: scheduled shutdown/restart over launchd's init signals, scheduled sleep through the root domain, entitlement checks, internal assertions; and IOKitUser's `IOPMSchedulePowerEvent` for the "Immediate" (RTC) types |
| `RootDomain_freebsd.c` | the kernel shim: `IOServiceOpen` / `IOConnectCallMethod` on the IOPMrootDomain as `/dev/iopmroot` ioctls (selector table), `getRootDomain()`, `getRootDomainConnect()`, `_set`/`_copyRootDomainProperty()`, and pmconfigd.m's root-domain general-interest listener |
| `libproc.h` | launchd's `libproc.h` shim plus `proc_name()` |
| `os/log.h`, `os_log_shim.c` | `os_log()`: `%{public}` annotations and `%@` handled, written to stderr (`/var/log/pmconfigd.stderr`) |
| `os/signpost.h` | `os_signpost_*()` as no-ops |
| `IOKit/pwr_mgt/` | vendored from IOKitUser and xnu, see above |

The launchd job is `overlay/System/Library/LaunchDaemons/org.nextbsd.powerd.plist`.
