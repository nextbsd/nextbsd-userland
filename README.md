# nextbsd-userland

Cross-built **Darwin system layer** for NextBSD — the Mach / launchd / configd /
CoreFoundation / IOKit stack and its daemons — built on an Ubuntu host with the
`nextbsd-kernel-toolchain` clang cross-compiler, exactly like `nextbsd-kernel`
and `nextbsd-freebsd-compat`. Publishes a rolling `continuous` userland artifact
that the `nextbsd` ISO assembler ingests.

> **Status: CI workflow scaffold.** This commit is the workflow harness only —
> container + cross sysroot + buildenv setup, no build/publish. The vendored
> Darwin sources (Tier 0–2), the `build-userland.sh` driver, the cmake cross
> toolchains, the build/pack/publish steps, and the license all land in a
> follow-up **code-drop PR**, reviewed in one pass.

## Scope

**In (Tiers 0–2 — the system layer that has no FreeBSD equivalent):**

| Tier | Components |
|------|-----------|
| 0 — host tool | `migcom` + `mig` (arch-neutral MIG codegen, built for the runner) |
| 1 — foundation libs | `libmach`→`libsystem_kernel` → `libdispatch` *(cmake)* → `libxpc` → `liblaunch` → `swift-foundation-icu` *(cmake)* → `libCoreFoundation` |
| 2 — daemons/services | `launchd`+`launchctl` → `configd` → `libSystemConfiguration` → `libIOKit`+`ioreg`+`caffeinate` → `kext_tools` → `Libnotify`+`notifyd` → syslog/asl stack → `IPConfiguration` → `mDNSResponder` → `DiskArbitration` → `hostnamed` |

**Out (by design):**

- **POSIX command suites** (`file_cmds`, `shell_cmds`, `text_cmds`, `adv_cmds`,
  `system_cmds`) — for generic POSIX tools Apple's build buys nothing over
  FreeBSD's, so these come from **FreeBSD source curated in
  `nextbsd-freebsd-compat`'s `srclist.txt`**, synced to the releng train.
- **PAM** (OpenPAM + pam_modules) — a later pass.

## Where it fits in the build chain

```
toolchain → nextbsd-freebsd-compat (base) ─┐
                                           ├→ nextbsd-userland → nextbsd (ISO)
            nextbsd-kernel → modules ──────┘
```

- **Sysroot:** staged from compat's `continuous` base tarball (hard dependency —
  userland links the from-source libc/libsys/headers).
- **Trigger:** `repository_dispatch: base-updated` from compat; `workflow_dispatch`.
- **Publishes:** `continuous` (`nextbsd-userland-<arch>.tar.gz`), then dispatches
  `userland-updated` → `nextbsd`.
- **Arches:** `amd64` + `arm64`, both cross-built on the x86_64 runner.

## Build model

`bsd.lib.mk`/`bsd.prog.mk` components cross-compile under `make.py … buildenv`
with the toolchain clang (zero source changes). The two CMake holdouts
(`libdispatch`, `swift-foundation-icu`) build on the runner with a cross
toolchain file (`cmake/cross-<arch>.cmake`). `migcom` builds for the runner as a
host tool. Driver order and the full design are in the
[repo-split plan](https://pkgdemon.github.io/nextbsd-userland-repo-plan.html).

## System path layout — the four-domain model

NextBSD uses an explicit
[**four-domain filesystem layout**](https://pkgdemon.github.io/nextbsd-path-domain-conformance-plan.html) —
**not** Apple's convention where the bare `/Library` *is* the Local domain. Every
system resource lives under an explicit domain root rather than a bare `/Library`,
keeping `/System` reserved for the System domain and the OS's admin-installed
resources in the Local domain:

| Domain  | Root            | Owner / meaning                    |
|---------|-----------------|------------------------------------|
| System  | `/System/Library` | OS-provided, immutable            |
| Local   | `/Local/Library`  | this machine, admin-installed     |
| Network | `/Network/Library`| network-shared (reserved)         |
| User    | `~/Library`       | per-user                          |

Consequently the Darwin-source components in this repo use `/Local/Library` for
everything Apple would have put in the bare `/Library` (Local) domain. The
canonical locations these components read/write:

| Path | Component | Purpose |
|------|-----------|---------|
| `/System/Library/LaunchDaemons` | launchd (PID 1) | OS daemon plists (scanned first) |
| `/Local/Library/LaunchDaemons`  | launchd (PID 1) | third-party daemon plists (scanned second) |
| `/Local/Library/StartupItems`   | launchctl | legacy StartupItems bootstrap dir |
| `/Local/Library/Preferences/SystemConfiguration` | `libSystemConfiguration` (`PREFS_DEFAULT_DIR`) | SCPreferences store — `preferences.plist` (`ComputerName`, network config), `com.apple.Boot.plist` |
| `/Local/Library/Preferences`    | `libCoreFoundation` | CFPreferences "any user" / computer domain |
| `~/Library/Preferences`         | `libCoreFoundation` | CFPreferences current-user (User domain — **not** `/Local`) |
| `/System/Library/Extensions`    | `kext_tools` | OS kext bundles |
| `/Local/Library/Extensions`     | `kext_tools` | auxiliary (admin-installed) kext bundles |
| `/Local/Library/Logs`, `/Local/Library/Logs/CrashReporter` | syslog/ASL | log + crash-report directories |

Rule of thumb for new code: a shared/computer-wide resource goes under
`/Local/Library`; an OS-shipped one under `/System/Library`; a per-user one under
`~/Library`. Never introduce a bare `/Library/...` path.

## Boot / trace debugging flags

Four knobs control how much a NextBSD boot tells you. All are **off by default**:
a shipped image boots quiet and silent, and you opt into noise. They are set at
the FreeBSD loader `OK` prompt (`set name=value`, then `boot`), which survives the
kernel → PID-1 handoff — that is how `tests/boot-test.sh` drives them under qemu.

| Flag | Kind | Read by | Effect |
|------|------|---------|--------|
| `boot_mutemsgs` | loader var → `RB_MUTEMSGS` | kernel | Mutes kernel `printf` to the console after the copyright banner. Shipped as `YES` by [nextbsd-overlays](https://github.com/nextbsd-redux/nextbsd-overlays) (`/boot/loader.conf.d/nextbsd.conf`) so IOKit/kext spew never clobbers the getty `login:` prompt. |
| `mach.debug_enable` | sysctl, `CTLFLAG_RWTUN` | **kernel** ([nextbsd-kernel](https://github.com/nextbsd-redux/nextbsd-kernel)) | Mach IPC + launchd dispatch trace. Emits `[T41] <comm>:<tid> …` via kernel `printf`, filtered to processes whose `p_comm` starts `lau` (launchd/launchctl/launchproxy). |
| `launchd_trace` | kenv | **userland** (this repo: `launchd`, `liblaunch`, `libxpc`) | `[T41-*]` / `[T39-*]` trace points. Emits to `stderr`. Each consumer reads the kenv independently — `libxpc` and `liblaunch` lazily on first call, since they run in daemons that can't see launchd's global. |
| `BOOT_TRACE` | env var for `tests/boot-test.sh` | the test harness | `BOOT_TRACE=1` makes the harness set both `mach.debug_enable=1` and `launchd_trace=1` at the loader. |

Two traps worth knowing:

**Both trace flags print `[T41]`, but only one is a kernel `printf`.** Kernel
lines carry a `<comm>:<tid>` prefix (`[T41] launchd:100002 …`); userland lines do
not. When a boot log is drowning in `[T41]`, check the prefix before deciding
which flag to turn off.

**`mach.debug_enable` is expensive on a serial console.** Kernel `printf` to a
115200 serial console is synchronous and blocks the writing thread. A traced boot
emits ~1800 such lines (~144 KB, ~12 s of blocking writes), which measurably
changes boot timing. It is *not* an established cause of the Mach-handshake
failures in [nextbsd#369](https://github.com/nextbsd-redux/nextbsd/issues/369):
disabling the trace did not make the amd64 boot green, and both #369 failure
modes reproduce with zero `[T41]` lines. Treat the flood as a confound to
eliminate when reading a boot log, not as a cause. Note that `boot_mutemsgs="YES"`
hides this cost rather than removing it: the `printf` still runs, it just isn't
displayed. CI leaves `BOOT_TRACE` off and sets it only for root-cause runs.

Note that `boot_mutemsgs` **cannot** hide userland output: `RB_MUTEMSGS` gates
kernel `printf`, while test markers and daemon `stderr` reach the console through
the tty driver. A missing marker in a boot log means it was never emitted.

Live toggling: `mach.debug_enable` is `RWTUN`, so `sysctl -w mach.debug_enable=1`
works on a running system. `launchd_trace` is read once at process start, so it
is loader-only in practice.

Related harness env vars: `BOOT_GATE=full` runs the on-image Mach/launchd/configd
suite; `BOOT_GATE=login` stops at the `login:` prompt.

## WLAN (802.11)

Wireless is owned by **`wland`** (`/usr/sbin/wland`, launchd job
`org.nextbsd.wland`, Mach service `org.nextbsd.wlan`) and driven by the
**`wlan`** CLI. Full detail: `man wland`, `man wlan`, and the
[WLAN plan](https://pkgdemon.github.io/nextbsd-wlan-plan.html).

```sh
wlan list                          # WLAN interfaces wland owns
wlan scan                          # SSID / BSSID / signal / channel / security
wlan connect <ssid> [passphrase]   # join, and remember it
wlan status                        # current association
wlan disconnect
```

```
$ wlan scan
SSID                             BSSID              SIGNAL   CH SECURITY
Home                             a4:2b:b0:11:22:33  ****  -42   6 WPA2
Cafe                             de:ad:be:ef:00:01  **    -71  36 open

$ wlan connect Home hunter2hunter2
associating with 'Home' on wlan0...

$ wlan status
interface: wlan0
state:     connected
ssid:      Home
signal:    -42 dBm
channel:   6
security:  WPA2
```

### The one thing to understand: `associated` is not `connected`

802.11 has two moments that look alike and are not. **`associated`** means the
station joined the BSS and net80211 reached RUN state — but **the port is not
yet keyed**. The interface reports its link UP at that instant, which is exactly
why a naive DHCP client fires a DHCPDISCOVER there and has every packet silently
dropped. **`connected`** means the WPA 4-way handshake completed and traffic can
actually flow.

NextBSD does not guess. `wland` publishes
`State:/Network/Interface/<if>/AirPort` with an `Authenticated` flag that is true
only when *connected*, and `ipconfigd` **holds DHCP until it sees it**:

```
DHCP requires:  Link{Active}  AND  AirPort{Authenticated}
```

So if `wlan status` says `associated` and you have no IP, that is the system
working **correctly**. If it stays there, authentication is failing — nearly
always a wrong passphrase.

**Key-absent-means-ready** is the load-bearing default: an interface with no
`AirPort` key (i.e. every wired NIC) is unconditionally ready, so `em0` behaves
bit-for-bit as it always has. Inverting that default would deadlock DHCP on
every Ethernet machine in existence.

### Design points that are easy to get wrong

- **`wpa_supplicant` is stock, from FreeBSD base.** Not vendored, not patched,
  not taught Mach, and *not* the `security/wpa_supplicant` port (same 2.11; it
  merely shadows base). It speaks net80211 ioctls downward and a UNIX control
  socket upward and does not know configd exists — all Mach knowledge lives in
  `wland`. A CVE is therefore a base rebuild, not a merge against local patches
  forever.
- **`wland` is the sole author of `wpa_supplicant`'s network blocks.** It spawns
  the supplicant with an **empty** config (`update_config=0`) and injects every
  network at runtime. Putting SSIDs in that config file creates a *second*
  autojoin brain competing with `wland`'s — the classic NetworkManager failure
  mode. Don't.
- **`wland` creates the VAP.** On FreeBSD that is `rc.conf`'s job
  (`wlans_iwlwifi0="wlan0"`); NextBSD has no `rc.conf` and no `/etc/rc.d`, so it
  had no owner at all. `wland` does it natively via `SIOCIFCREATE2`, with a
  wildcard name so the kernel picks the unit.
- **Passphrases are plaintext**, in a root-owned `preferences.plist` (0600).
  Unavoidable: there is no keychain, so they are plaintext on disk either way —
  the only real choice is *which* file.
- **Scan results do not go in the dynamic store.** `CONFIG_DATA_MAX` caps any
  value at 8 KiB and a busy scan blows past it, and MIG out-of-line data is
  broken in this kernel — hence the flat `wlan_scan_count()` +
  `wlan_scan_entry(i)` RPC pattern.
- **amd64 only.** arm64 `GENERIC` never sets `device wlan`, so net80211 is
  absent from that kernel entirely.

### Debugging

There is **no `scutil`** in this port, so `wlan` *is* the introspection tool.

```sh
launchctl list | grep wland      # is it running?
tail -f /var/log/wland.stderr    # what is it doing?
sysctl net.wlan.devices          # did the driver attach at all?
ifconfig wlan0                   # did the VAP get cloned?
```

Log markers worth grepping: `WLAN-IF-OK` (VAP up), `WLAN-RPC-OK` (Mach service
checked in), `WLAN-AIRPORT-OK` (the association-complete signal reached the
store — the thing ipconfigd's gate waits on), and on the ipconfigd side
`IPCFG-LINK-GATED` (DHCP deliberately held) vs `IPCFG-LINK-UP` (gate open).

## Manpages

Every shipped binary and library that has a man page installs it into
`/usr/share/man/man<N>` at build time (the component Makefiles declare `MAN=`;
`bsd.prog.mk`/`bsd.lib.mk` gzip and install them — `libdispatch` installs its
via CMake). Pages are vendored from the same upstream tag as the code they
document. Where an Apple page referenced a bare `/Library/...` path it is
rewritten to the Local domain `/Local/Library/...` per the four-domain model
above; `/System/Library` and `~/Library` paths are kept as-is.

| Component | Manpage | Documents |
|-----------|---------|-----------|
| launchd | `man launchd` (8) | the PID 1 service manager |
| launchd | `man launchctl` (1) | launchd control utility |
| launchd | `man launchd.plist` (5) | job / agent / daemon plist format |
| configd | `man configd` (8) | System Configuration daemon |
| DiskArbitration | `man diskarbitrationd` (8) | disk arbitration daemon |
| libIOKit | `man ioreg` (8) | I/O Kit registry viewer |
| kext_tools | `man kextd` (8) | kext-loading daemon |
| kext_tools | `man kextload` (8) | load a kernel extension |
| kext_tools | `man kextunload` (8) | unload a kernel extension |
| kext_tools | `man kextstat` (8) | list loaded kernel extensions |
| Libnotify | `man notifyd` (8) | notification daemon |
| Libnotify | `man notify` (3) | notify(3) API (plus the `notify_*` pages) |
| syslog/ASL | `man syslogd` (8) | syslog daemon |
| syslog/ASL | `man syslog` (1) | syslog command-line tool |
| syslog/ASL | `man syslog.conf` (5), `man asl.conf` (5) | syslogd config formats |
| syslog/ASL | `man aslmanager` (8) | ASL log-store manager |
| syslog/ASL | `man asl` (3), `man syslog` (3) | ASL / syslog library API |
| mDNSResponder | `man mDNSResponder` (8) | multicast DNS / DNS-SD responder |
| libdispatch | `man dispatch` (3) | Grand Central Dispatch API (plus the `dispatch_*` pages) |
| cpdup | `man cpdup` (1) | filesystem copy / mirror utility |
| WLAN | `man wlan` (8) | WLAN command-line client |
| WLAN | `man wland` (8) | 802.11 management daemon |

**Shipped binaries with no man page** (deliberately none — not fabricated):
`ipconfigd` (upstream ships only `ipconfig.8` for the client CLI, which NextBSD
does not build — there is no daemon page), and `hostnamed` / `kextdeps`
(NextBSD-original, no upstream page). `wland` / `wlan` are also
NextBSD-originals with no upstream page — but unlike those, they ship
*original* pages written for them, because `wlan` is a CLI users actually type
and there is no `scutil` to fall back on. Writing our own page for our own tool
is not the same as fabricating an Apple one. `mig` / `migcom` ship man pages upstream
but are host-only build tools not installed into the target userland, so their
pages are not shipped.
