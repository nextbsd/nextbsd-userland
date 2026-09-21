# Darwin-only kernel probes in launchd / launchctl

launchd and launchctl are vendored Apple source (launchd-842.92.1). They
probe Darwin kernel facilities and wrap the calls in `posix_assumes_zero` /
`os_assumes*`. Those macros log to the console whenever the call fails, so any
facility NextBSD's kernel doesn't have prints an error on every boot
([nextbsd#324](https://github.com/nextbsd/nextbsd/issues/324)).

The rule: if a facility is known to be missing on NextBSD, don't probe it. Put
the code in `#ifdef __FreeBSD__` and return the "feature off" answer. Don't
alias it to a MIB that is guaranteed to fail, because the assert will still log.

## Disposition of every probe

| Site | Facility | On NextBSD | Disposition |
|---|---|---|---|
| `support/launchctl.c` `is_safeboot()` | `kern.safeboot` (`KERN_SAFEBOOT`) | absent | **stub-false.** No safe-boot mode (matches `OSKextGetActualSafeBoot()`, #182). |
| `support/launchctl.c` `is_netboot()` | `kern.netboot` (`KERN_NETBOOT`) | absent | **stub-false.** NextBSD does not NetBoot. |
| `support/launchctl.c` `do_bootroot_magic()` | `IODeviceTree:/chosen` → `boot-root-active` | absent (IOKit shim returns `IO_OBJECT_NULL`) | **stub.** Returns quietly with no BootRoot/kextcache refresh. |
| `src/runtime.c` `launchd_runtime_init()`, `runtime_fork()` | `vfs.generic.noremotehang` | absent | **best-effort** `(void)sysctlbyname(...)` (nextbsd#317). A real port is tracked in nextbsd#318. |
| `support/launchctl.c` `do_sysversion_sysctl()` | `kern.osversion` (`KERN_OSVERSION`) | aliased to `kern.osrelease` | **real MIB.** Always non-empty, so launchctl never writes it. |
| `support/launchctl.c` `sysctl_hw_streq()`, `limitloadtohardware_iterator()` | `hw.machine`, `hw.model`, `hw.<plist key>` | present | **real MIB.** The keys come from job plists. |
| `support/launchctl.c` bootstrap `KERN_HOSTNAME` write | `kern.hostname` | present | **real MIB.** |
| `support/launchctl.c`, `src/runtime.c` | `kern.bootargs` | may be absent | **no assert.** The callers already treat failure as "no boot-args". |
| `src/runtime.c` | `hw.machine` | present | **real MIB.** |
| `src/core.c` `get_kern_max_proc()` | `kern.maxproc` | present | **real MIB.** |
| `src/ipc.c` rlimit sync | `kern.maxproc`, `kern.maxprocperuid`, `kern.maxfiles`, `kern.maxfilesperproc` | present | **real MIB.** |

## Regression check

`tests/boot-test.sh` fails the run if the console log has an
`*_assumes_zero: sysctl...(errno 2)` line, which means a probe of a missing
sysctl got through.
