# Vendored Apple sources — mDNSResponder

The `mDNSCore/`, `mDNSPosix/`, and `mDNSShared/` subdirs were imported
verbatim from upstream Apple
[apple-oss-distributions/mDNSResponder](https://github.com/apple-oss-distributions/mDNSResponder)
(`/Users/jmaloney/Documents/launchd/mDNSResponder/` in this dev env).
`LICENSE` is the upstream license file (Apache 2.0 + BSD-3-Clause hybrid
per Apple convention).

## Subdir map

| Subdir | Purpose |
|---|---|
| `mDNSCore/` | Platform-agnostic mDNS protocol engine: `mDNS.c`, `DNSDigest.c`, `uDNS.c`, `DNSCommon.c`. Pure RFC 6762/6763 + DNS message parsing. Zero Mach surface. |
| `mDNSPosix/` | POSIX platform binding: `mDNSPosix.c` (kqueue + sockets), `mDNSUNP.c` (UNP helpers), `PosixDaemon.c` (the daemon's `main`). |
| `mDNSShared/` | Cross-cut shared code: `uds_daemon.c` (Unix Domain Socket server), `dnssd_ipc.c`, `ClientRequests.c`, `dnssd_clientshim.c`, `mDNSDebug.c`, `GenLinkedList.c`, `PlatformCommon.c`, plus `utilities/mdns_addr_tailq.c` + `utilities/misc_utilities.c`. |
| `Clients/` | The reference DNS-SD command-line client — `dns-sd.c` (browse `-B`, resolve `-L`, register `-R`, getaddrinfo `-G`, enumerate types via `-B _services._dns-sd._udp`) + `ClientCommon.c`. Links `libdns_sd`; installed as `/usr/bin/dns-sd`. Man page is `mDNSShared/dns-sd.1`. Only these two files are taken (verbatim). |

## Skipped — what we intentionally don't vendor

| Subdir | Why |
|---|---|
| `DSO/` | DNS Stateful Operations (RFC 8490). Required only when `MDNSRESPONDER_SUPPORTS_COMMON_DNS_PUSH=1` is set, and the default in `mDNSShared/mDNSFeatures.h` is 0 — so the `#if MDNSRESPONDER_SUPPORTS(COMMON, DNS_PUSH)` blocks in `mDNSCore/uDNS.c` + `uDNS.h` are excluded at the preprocessor level. DSO also pulls `srp-features.h` + `srp-log.h` from `ServiceRegistration/`, which in turn pull Apple-only `<os/log.h>`. Skipping DSO cleanly avoids the entire dependency tree. iter 7+ can revisit if DNS Push becomes a real consumer demand. |
| `mDNSMacOSX/` | Apple-tied platform binding — IOKit, KeychainServices, SystemConfiguration-private, powerd. None translate to FreeBSD; the cross-platform `mDNSPosix/` is what we use instead. |
| `mDNSWindows/` | Windows platform binding. Out of scope. |
| `Clients/` (rest) | Only `dns-sd.c` + `ClientCommon.c` are vendored (see the subdir map above). The remaining clients, unit tests, and the Xcode / Visual Studio project files remain out of scope. |
| `ServiceRegistration/` | SRP / Thread border-router specific. Not needed for the standalone mDNSResponder daemon. |
| `mDNSResponder.proj/` + `Documents/` | Xcode + docs, not source. |

## Local edits

### `mDNSPosix/PosixDaemon.c` — Mach service hook (4 lines)

Two changes to `main()`:

1. Right after `ParseCmdLineArgs(argc, argv);`, call `mDNSResponderMachBridgeInit()` so the daemon claims `com.apple.mDNSResponder` via `bootstrap_check_in` before the engine starts. Logs `MDNS-BOOT-OK`.
2. After `mDNS_Init` returns `mStatus_NoError`, log `MDNS-ENGINE-OK` (the iter-2 marker the boot test gates on).

See `mach_bridge.c` for the bridge implementation.

### `mDNSPosix/PosixDaemon.c` — static service files (nextbsd-userland#250)

Two more NextBSD hooks in `main()` and `MainLoop()`: `StaticServicesInit()`
after `Reconfigure()` (still root, so the directory can be created) and
`StaticServicesIdle()` once per loop pass (re-arms the directory watch if it
failed at start; a no-op otherwise). The implementation is NextBSD code,
BSD-2-Clause, in two new files that are not upstream's:

| File | Purpose |
|---|---|
| `mDNSPosix/StaticServices.c` | Registers every `*.plist` in `/Local/Library/Preferences/mDNSResponder/Services/` through `mDNS_RegisterService()` and keeps the set in step with the directory via `kqueue(2)` `EVFILT_VNODE`, so a one-shot tool such as `dscli promote` can announce a service and exit. File format: `Name`, `Type`, `Port`, optional `Domain`, `Host`, `TXT` dict. |
| `mDNSPosix/nbsd_plist.[ch]` | The small XML plist reader, copied from `src/nss_directory_services/plist.[ch]` (same repo, BSD-2); keep in sync by copying. |

Everything else in PosixDaemon.c is unchanged.
