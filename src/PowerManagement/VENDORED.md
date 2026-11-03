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

The IOKit power management and power source headers pmconfigd builds
against are libIOKit's (`src/libIOKit/IOKit/pwr_mgt/` and `.../ps/`,
nextbsd-userland#232 and #233), as Apple builds it against IOKit.framework:
IOKitUser's `IOPMLib.h`, `IOPMLibPrivate.h`, `IOPMKeys.h`,
`IOPMAssertionCategories.h`, `powermanagement.defs`, `powermanagement_mig.h`
and `ps.subproj/` headers (IOKitUser-100231.120.3, commit
`23a611d9d194b63465f6df2abfdda63d71c87e97`) and xnu's `IOPM.h`,
`IOPMPrivate.h` and `IOPMLibDefs.h` (xnu-12377.121.6, commit
`ac9718fb1af618d5ce8678d0dc6e8a58f252216f`). So is the Energy Saver
preferences code (IOKitUser's `IOPMEnergyPrefs.c`) that PMSettings.m reads
the settings through.

`pmconfigd/powermanagement.defs` is a one-line
`#include <IOKit/pwr_mgt/powermanagement.defs>`, which resolves to libIOKit's
copy: the one description of the wire for both ends, pmconfigd's server and
libIOKit's clients, generated once by build-userland.sh. It drops
`UseSpecialReplyPort 1;`, as configd's `config.defs` and Libnotify's
`notify_ipc.defs` do; the routine order (the msgh_ids from 73000) is unchanged.

## What is built

The shipping macOS target is `powerd` in `PowerManagement.xcodeproj`, and the
Xcode project, not Apple's `pmconfigd/Makefile`, is the source of truth for
what it contains. That Makefile predates the Xcode project (i386/ppc, files
that no longer exist), so it has been **replaced** by NextBSD's bsd.prog.mk
Makefile, which `build-userland.sh` drives. It is the one Apple file
overwritten in place. Like the Xcode target, it builds `powerd` and installs
it where macOS does, `/System/Library/CoreServices/powerd.bundle/powerd`
(nextbsd-userland#240; before that it was `/usr/sbin/pmconfigd`).

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

D5 (nextbsd-userland#229) adds sleep and wake. `PMConnection_freebsd.c` is a
C port of `PMConnection.m`'s state machine and IOPMConnection server: it
registers for the root domain's app-power messages, turns each into the
capability change xnu would send (NextBSD has no dark wake, so every wake
is a full one), and holds each sleep until IOPMConnection clients answer.
`pmconfigd_freebsd.c` gains `pmconfigd.m`'s own `IORegisterForSystemPower`
client. `RootDomain_freebsd.c` claims the lid and power-button policy at
start and requests "Low Power Sleep" on a critical battery, which xnu's
root domain does itself.

D4 (nextbsd-userland#228) adds the assertion engine and the idle-sleep
policy: Apple's `PMAssertions.c`, `PMAssertionLog.c`, `TTYKeepAwake.c` and
`PMSettings.m` (as C). They serve the `io_pm_assertion_*`,
`io_pm_declare_*`, `io_pm_ctl_assertion_type`, `io_pm_assertion_notify`
and `io_pm_set_exception_limits` MIG routines and push the Energy Saver
settings and the assertion levels to the root domain. xnu runs the idle
sleep and display sleep timers in the kernel (IOPMrootDomain,
IODisplayWrangler); NextBSD's root domain stores those inputs but has no
timer, so `RootDomainIdle_freebsd.c` runs xnu's timer logic in pmconfigd
and asks the root domain for an "Idle Sleep" when it runs out. Left out:
the iOS async-offload assertion protocol (XPC), IOReport aggregates,
energytrace, RunningBoard App Nap, the powerlog activity buffer and SMC
battery commands.

## Apple files changed

Each change is under `#ifdef __FreeBSD__`, with Apple's code kept in the
`#else`:

| File | Change |
|---|---|
| `pmconfigd/AutoWakeScheduler.c` | includes `PrivateLib_freebsd.h` in place of `PrivateLib.h`, `PMAssertions.h` and `PMSettings.h` |
| `pmconfigd/RepeatingAutoWake.h` | the same, for `PrivateLib.h` |
| `pmconfigd/RepeatingAutoWake.c` | the same; the Gregorian `CFCalendar` calls become `localtime_r(3)` / `mktime(3)` in the same local time zone |
| `pmconfigd/PMAssertions.h` | includes `PrivateLib_freebsd.h`; no IOReport headers; no `CFMachPort` prototype |
| `pmconfigd/PMAssertions.c` | the include list; without the async-offload XPC handlers, IOReport aggregates, `os_state`, CrashReporter and the CF/XPC bridge; no dark-wake proxies (every wake is a full wake) and no IOService busy-state interest |
| `pmconfigd/PMAssertionLog.c` | the include list; without the powerlog activity buffer (its keys are not in the IOKitUser drop and its reader needs an entitlement), kdebug, IOReport and the CF/XPC bridge |
| `pmconfigd/PMSettings.m` | the include list; no hibernation, PowerNap or pre-CFPreferences settings migration; `ActivatePMSettings()` hands each profile to `CPUPower_freebsd.c` |
| `pmconfigd/TTYKeepAwake.c` | watches `/var/run/utx.active` with a vnode source, since NextBSD's libc posts no utmpx notification |

The Xcode project also builds `pmset`, `caffeinate`, `pmtool`, `ioupsd` and
the Low Power Mode and battery-health pieces. None is built from this tree
yet. `src/caffeinate` is a separate, older import (PowerManagement-1049).

## NextBSD files

All in `freebsd-shims/` unless noted (the Libnotify pattern):

| File | Purpose |
|---|---|
| `pmconfigd/Makefile` | the build (replaces Apple's stale Makefile, see above) |
| `pmconfigd/powerd.8` | manual page; Apple ships none for powerd |
| `pmconfigd_freebsd.c` | `main()`, `powerd_init()` and the two listeners, in Apple's shape, until `pmconfigd.m` and `PrivateLib.m` build |
| `powermanagement_stubs.c` | the `_io_*` MIG routines whose Apple files are not built yet; `io_pm_assertion_copy_details` serves only the two scheduled-event selectors |
| `PrivateLib_freebsd.h` | the share of `PrivateLib.h` (and the IOKit private headers it includes) that the ported Apple files use, until `PrivateLib.h` builds |
| `PrivateLib_freebsd.c` | `PrivateLib.m` functions those files call: scheduled shutdown/restart over launchd's init signals, scheduled sleep through the root domain, entitlement checks, ASL, `powerd_notify_and_log()`, `_getSharedPMDynamicStore()`; IOKitUser's `IOPMSchedulePowerEvent` for the "Immediate" (RTC) types; and one interim block per Apple file not built yet (PMConnection.m, SystemLoad.c, PMDisplay.m, Platform.c) |
| `RootDomain_freebsd.c` | the kernel shim: `IOServiceOpen` / `IOConnectCallMethod` on the IOPMrootDomain as `/dev/iopmroot` ioctls (selector table), `getRootDomain()`, `_set`/`_copyRootDomainProperty()`, pmconfigd.m's root-domain general-interest listener, and the sleep policy the kernel leaves to pmconfigd: claiming the lid and power-button policy, and the low-battery sleep |
| `PMConnection_freebsd.c` | `PMConnection.m`'s sleep/wake state machine (app-power messages as capability changes), the IOPMConnection MIG routines, `io_pm_get_capability_bits` and `getRootDomainConnect()`, until `PMConnection.m` builds |
| `RootDomainIdle_freebsd.c` | xnu's IOPMrootDomain idle sleep timer and IODisplayWrangler idle timer, fed by the aggressiveness, assertion levels and tickles pmconfigd sends the kernel |
| `PMSettings_freebsd.c` | compiles `pmconfigd/PMSettings.m` as C |
| `CPUPower_freebsd.c` | the CPU part of a Mac's kernel platform plugin: the active profile's energy/performance preference on `hwpstate_intel(4)` (`dev.hwpstate_intel.N.epp`), Low Power Mode on frequency-level CPUs, and the `LowPowerMode` feature for the energy prefs (nextbsd-userland#231) |
| `libproc.h` | launchd's `libproc.h` shim plus `proc_name()` |
| `os/log.h`, `os_log_shim.c` | `os_log()`: `%{public}` annotations and `%@` handled, written to stderr (`/var/log/powerd.stderr`) |
| `os/signpost.h` | `os_signpost_*()` as no-ops |
| `PMStore_freebsd.c` | compiles Apple's `pmconfigd/PMStore.c` unmodified, standing in for the `PrivateLib.h` it includes |
| `BatteryTimeRemaining_freebsd.c` | the power-source core of `BatteryTimeRemaining.m` as C (nextbsd-userland#227): IOPMPowerSource notifications, the internal battery's kIOPS dictionary, the notify(3) state, the `io_ps_*` MIG routines. Apple's code where it does not touch Foundation; each NextBSD difference is marked |
| `SystemConfiguration/SCValidation.h`, `SCDynamicStorePrivate.h` | `isA_CF*()` (as in `PrivateLib_freebsd.h`), and a no-op `SCDynamicStoreSetDisconnectCallBack()` (NextBSD's SystemConfiguration has no reconnect) |
| `AppleFeatures/AppleFeatures.h` | empty; `IOPowerSourcesPrivate.h` includes it |

The launchd job is `overlay/System/Library/LaunchDaemons/org.nextbsd.powerd.plist`.
