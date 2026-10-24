# Vendored: Apple libxslt

**Upstream:** https://github.com/apple-oss-distributions/libxslt
**Tag:** `libxslt-21.13` (commit `85153e309238f638a9f7448072cd77376434e181`)
**Tag archive SHA-256:** `bd6c89da7a4101e4c7408bdc3aea8be1bb01fb98e187d41966ad80b59c12258d`
(`https://github.com/apple-oss-distributions/libxslt/archive/refs/tags/libxslt-21.13.tar.gz`)
**Carries:** libxslt **1.1.35** (libexslt 0.8.20) plus Apple's fixes
**Imported:** 2026-10-24 ([nextbsd-userland#442](https://github.com/nextbsd/nextbsd-userland/issues/442), E24 decision D4)
**License:** MIT (`Copyright`)

## Why Apple's

macOS ships libxslt and libexslt as system libraries, and E24 decision D4
takes Apple's source wherever Apple ships an equivalent under a licence that
allows it. It builds against Apple's libxml2 in `src/libxml2`.

## What is here

`src/libxslt` is the `libxslt/` directory of Apple's tag, unmodified, with
these removed because nothing builds or ships them: `doc/` except
`xsltproc.1`, `tests/`, `examples/`, `python/`, `win32/`, `vms/`,
`.gitlab-ci.yml`. Apple's top-level Xcode build (`libxslt.xcodeproj`,
`Configurations/`, `Pregenerated Files/`, `Makefile*`) is not imported;
`libxslt.plist` is kept for its provenance record.

`patches/` holds NextBSD's changes as a series. `build-userland.sh` copies
this tree to `.build/libxslt-src-<arch>` and applies the series there with
`patch -p1`, so the tree in git stays byte-for-byte Apple's. Re-vendor as
for libxml2 (see `src/libxml2/VENDORED.md`).

## Build

Like libxml2: no generated `configure` in Apple's tree, so the
`CMakeLists.txt` that upstream added in 1.1.35 is built with host
cmake/ninja and `cmake/cross-<arch>.cmake`, against the libxml2 just built.

Apple's stricter behaviour needs no patch here: Apple's
`linkedOnOrAfterFall2022OSVersions()` and `linkedOnOrAfterFall2023OSVersions()`
(`libxslt/xsltutils.c`) already return true off Darwin, and the public
headers enable the 1.1.34+ API off Darwin.

Installed:

| Path | What |
|---|---|
| `/usr/lib/libxslt.so.1` (→ `.so.1.1.35`), `libxslt.so` | library, upstream soname |
| `/usr/lib/libexslt.so.0` (→ `.so.0.8.20`), `libexslt.so` | library, upstream soname |
| `/usr/include/libxslt/*.h`, `/usr/include/libexslt/*.h` | headers |
| `/usr/libdata/pkgconfig/libxslt.pc`, `libexslt.pc` | pkg-config (`prefix=/usr`) |
| `/usr/lib/cmake/libxslt-1.1.35/` | CMake package config |
| `/usr/bin/xslt-config` | upstream config script |
| `/usr/bin/xsltproc` | the command |
| `xsltproc(1)`, `libxslt(3)`, `libexslt(3)` | man pages |

Not built: Python bindings, tests, the libgcrypt-based EXSLT crypto
functions. Plugins are looked for in `/usr/lib/libxslt-plugins` (none ship).

## Patches

| Patch | Why |
|---|---|
| `0001-cmake-soversion.patch` | 1.1.35's CMake build has no `SOVERSION`, so its sonames were the full versions; upstream's are `libxslt.so.1` and `libexslt.so.0`. |
| `0002-cmake-apple-include-layout.patch` | Apple's tree includes headers by bare name across directories (`"xsltconfig.h"`, `"xsltutils.h"`), which its Xcode header maps resolve; the CMake build gets those directories on its build-tree include path. |
| `0003-cmake-libexslt-libm.patch` | libexslt's math functions need libm, which the CMake build only linked into libxslt; Darwin's libSystem hid that. |
| `0010`, `0011` | Upstream 1.1.39 stack-depth limits (imports, default templates); see the CVE check below. |

## Security fixes since 1.1.35 (CVE check)

**Result:** Apple's libxslt code already has every libxslt CVE fix (CVE-2023-40403, CVE-2024-55549, CVE-2025-24855, CVE-2025-7424, CVE-2025-11731), and they compile on NextBSD. CVE-2025-7425, CVE-2025-10911 and CVE-2025-9714 are fixed on the libxml2 side, by `src/libxml2/patches/0012`, `0014` and `0013`. Two OSS-Fuzz stack-depth fixes from 1.1.39 (no CVE) are carried as `patches/0010` and `0011`. Not carried: libxslt MR !77 for CVE-2025-10911 (unmerged upstream, ABI change; the libxml2 fix closes the CVE per upstream), malloc-failure-only fixes from 1.1.38, and the unreleased wip/davidk/aisle-issues branch.

Checked on 2026-10-24 against upstream libxml2 up to v2.15.4 and master, libxslt up to v1.1.45 and master, the Debian security tracker, and the libxml2 GitLab issues labelled "1. Security". "In Apple tree?" is judged on the code that compiles on NextBSD (no `__APPLE__`); several of Apple's own fixes sit behind `__APPLE__` or OS-version checks and do not count. Line numbers refer to the unpatched Apple tree.

| component | CVE / issue | upstream fixed-in + commit | in Apple tree? (evidence) | action |
|---|---|---|---|---|
| libxslt | CVE-2023-40403 | 1.1.38, 82f6cbf8 | Yes: libxslt/functions.c:xsltGenerateIdFunction:790-861 (psvi id path is compiled; linkedOnOrAfter*() returns true on non-Apple) | already fixed |
| libxslt | CVE-2024-55549 | 1.1.43, 46041b65 | Yes: libxslt/xslt.c:exclPrefixPush:157-169 (dict-owned prefix) | already fixed |
| libxslt | CVE-2025-24855 | 1.1.43, c7c7f1f7 | Yes: numbers.c:xsltNumberFormatGetValue:711-730; templates.c:73/102, 143/173; xsltutils.c:xsltComputeSortResultInternal:1046/1109 save and restore the context node | already fixed |
| libxslt | CVE-2025-7424 | 1.1.44 f6f7f599 (backport branch 3d1bd3e4) | Yes: functions.c:xsltDocumentFunction:174-193 (xmlCopyDoc + xsltCleanupSourceDoc); transformInternals.h | already fixed |
| libxslt | CVE-2025-11731 | 1.1.44 fe508f20 (backport 686bcd0e) | Yes: libexslt/functions.c:exsltFuncResultComp:616-623 stops at the document node | already fixed |
| libxslt | CVE-2025-7425 | no libxslt fix; mitigated in libxml2 | Vulnerable on NextBSD unless libxml2 patch 0012 is applied (see libxml2 row) | covered by libxml2 **patch 0012** |
| libxslt | CVE-2025-10911 | libxslt MR !77 (unmerged); libxml2 d583ca9b | No: libxslt still chains RVTs through xmlDoc prev/next (variables.c) | covered by libxml2 **patch 0014**; MR !77 not carried (unmerged, ABI change) |
| libxslt | CVE-2025-9714 | libxml2 677a4264; libxslt b7994c3b is test-only | n/a for libxslt code | covered by libxml2 **patch 0013** |
| libxslt | import/include nesting depth (OSS-Fuzz stack overflow, no CVE) | 1.1.39, e4a272bb | No: imports.c:xsltCheckCycle:76 is the 1.1.35 version | **patch 0010** |
| libxslt | default-template recursion depth (OSS-Fuzz, no CVE) | 1.1.39, fe9da133 | No: transform.c:xsltDefaultProcessOneNode has no depth check | **patch 0011** |
| libxslt | import cycle check c37d3087; xsltEvalUserParams overflow 096b5ff3; number/date overflows 677b2e8f, f13329a6, a8c3824f, cbf9c76f | 1.1.38 | Yes: all added lines present (date formatting is bounded by existing `cur < end` macros) | already fixed |
| libxslt | malloc-failure-only UAF / double frees: ac9d7522, cf0e40ed, dd5a4064 | 1.1.38 | Partly | not carried. Low: needs an allocation failure to trigger |
| libxslt | unmerged branch wip/davidk/aisle-issues: xsltproc writesubtree check, reject XInclude under security prefs | not released (Sept 2026) | No | not carried. Upstream fix still pending; revisit when merged |
