# Vendored sources &amp; licenses

`nextbsd-userland` vendors the Darwin system-layer sources we use, each **under
its own upstream license** (preserved in-tree — see each component's `LICENSE`
files and source headers, which are authoritative). This NOTICE summarizes
provenance; it does not relicense anything.

> The top-level **`LICENSE` (BSD 2-Clause)** covers the build harness we author —
> `build-userland.sh`, `cmake/`, `.github/workflows/`, this `NOTICE.md`. The
> vendored `src/` components keep their own upstream licenses (below), which are
> authoritative for those files.

## Per-component (Tier 0–2)

| Component | Upstream | License |
|-----------|----------|---------|
| `bootstrap_cmds` (migcom/mig) | apple-oss-distributions/bootstrap_cmds | APSL-2.0 |
| `libmach` → libsystem_kernel | Apple Mach userland | APSL-2.0 |
| `libdispatch` | apple/swift-corelibs-libdispatch | Apache-2.0 |
| `libxpc` | ravynOS `lib/libxpc` | BSD-2-Clause *(confirm in review)* |
| `launchd` (+ liblaunch, launchctl) | apple-oss-distributions/launchd | Apache-2.0 |
| `swift-foundation-icu` | apple/swift-foundation-icu (ICU) | Apache-2.0 (Unicode-ICU) |
| `libCoreFoundation` | apple/swift-corelibs-foundation (CF) | Apache-2.0 |
| `configd` | apple-oss-distributions/configd | APSL-2.0 |
| `libSystemConfiguration` | apple-oss-distributions/configd | APSL-2.0 |
| `libIOKit` | apple-oss-distributions/IOKitUser | APSL-2.0 |
| `kext_tools` | apple-oss-distributions/kext_tools | APSL-2.0 |
| `Libnotify` (+ notifyd) | apple-oss-distributions/Libnotify | APSL-2.0 |
| `syslog` (asl/syslogd) | apple-oss-distributions/syslog | APSL-2.0 |
| `IPConfiguration` | apple-oss-distributions/IPConfiguration (bootp) | APSL-2.0 |
| `mDNSResponder` (+ libdns_sd) | apple-oss-distributions/mDNSResponder | Apache-2.0 |
| `DiskArbitration` | apple-oss-distributions/DiskArbitration | APSL-2.0 |
| `hostnamed` | NextBSD (Apple-shape) | APSL-2.0 *(component header)* |
| `caffeinate` | apple-oss-distributions/PowerManagement | APSL-2.0 |
| `nss_directory_services` | Joseph Maloney; adapted from his Gershwin DirectoryServices NSS module (gershwin-desktop/gershwin-components) for NextBSD | BSD-2-Clause |

Two license families dominate: **APSL-2.0** (classic Apple open source) and
**Apache-2.0** (the swift.org-era projects + mDNSResponder). Both are permissive
and OSI-recognized. Where a component carries its own `LICENSE`/headers, those
control over this table.
