# Vendored: Apple PowerManagement (pmconfigd)

**Upstream:** https://github.com/apple-oss-distributions/PowerManagement
**Upstream tag:** `PowerManagement-1846.120.8.0.1`
**Upstream commit:** `dc081c6cca8184bd1f49987ac70a800f2b19b0de`
**Imported:** 2026-11-02 (nextbsd-userland#224, E17 D1)
**License:** APSL 2.0 (see `APPLE_LICENSE`)

The whole tree is imported as Apple ships it, minus `.git`. Apple's files are
unmodified except `pmconfigd/Makefile` (below); a diff against a checkout of
the upstream tag shows exactly that file plus the NextBSD files listed at the
end.

Also vendored, into `freebsd-shims/IOKit/pwr_mgt/`:

| File | Upstream |
|---|---|
| `powermanagement.defs` | apple-oss-distributions/IOKitUser @ `IOKitUser-100231.120.3` (commit `23a611d9d194b63465f6df2abfdda63d71c87e97`), `pwr_mgt.subproj/powermanagement.defs` |
| `powermanagement_mig.h` | same tag, `pwr_mgt.subproj/powermanagement_mig.h`, unmodified |

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
| `powermanagement_stubs.c` | the `_io_*` MIG routines whose Apple files are not built yet |
| `os/log.h`, `os_log_shim.c` | `os_log()`: `%{public}` annotations and `%@` handled, written to stderr (`/var/log/pmconfigd.stderr`) |
| `os/signpost.h` | `os_signpost_*()` as no-ops |
| `IOKit/pwr_mgt/` | vendored from IOKitUser, see above |

The launchd job is `overlay/System/Library/LaunchDaemons/org.nextbsd.powerd.plist`.
