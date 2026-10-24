# Vendored: Apple libffi

**Upstream:** https://github.com/apple-oss-distributions/libffi
**Tag:** `libffi-40` (commit `72e95a49029b75b2a0de094477f8410a32013c87`, 2025-04-30)
**Tarball:** `https://github.com/apple-oss-distributions/libffi/archive/refs/tags/libffi-40.tar.gz`
**SHA-256:** `92a07516242177db37b5a058827552fc704d8cc214e4f19449e97f6a37739ad7`
**Carries:** libffi **3.4.0-rc2** (`configure.ac` `AC_INIT`; upstream tag `v3.4.0-rc2`, commit `4557f232`)
**Imported:** 2026-10-24 (nextbsd/nextbsd-userland#443, E24 decision D4)
**License:** MIT (`LICENSE`)
**Consumer:** gnustep-base (built by Gershwin), through `pkg-config libffi`

## What is vendored

A verbatim subset of the tag: the files below are byte-identical to the
tarball above. Nothing in them is edited in place; NextBSD's changes are
the patch series in `patches/`, applied at build time.

| Path | What |
|---|---|
| `src/*.c` | Generic code: `prep_cif.c`, `types.c`, `raw_api.c`, `java_raw_api.c`, `closures.c` (+ `dlmalloc.c`), `tramp.c`, `debug.c` |
| `src/x86/` | x86 port; NextBSD builds the x86-64 files (`ffi64.c`, `unix64.S`, `ffiw64.c`, `win64.S`) |
| `src/aarch64/` | AArch64 port (`ffi.c`, `sysv.S`) |
| `include/` | `ffi.h.in`, `ffi_common.h`, `ffi_cfi.h`, `tramp.h` |
| `man/` | `ffi(3)`, `ffi_call(3)`, `ffi_prep_cif(3)`, `ffi_prep_cif_var(3)` |
| `libffi.map.in`, `libffi.pc.in`, `libtool-version` | Symbol versions, pkg-config template, ABI version |
| `configure.ac`, `configure.host`, `acinclude.m4`, `Makefile.am` | Not run; kept as the reference `nextbsd/fficonfig.h` and `Makefile` follow |
| `darwin/` | Apple's hand-maintained headers; not built, kept as the pattern NextBSD follows |

**Left out:** the other architectures' ports, the testsuite, `doc/`
(texinfo), the Xcode project, the MSVC files, `ChangeLog.old`, `m4/`,
`config.guess`/`config.sub` and Apple's helper scripts.

## NextBSD files

| File | Purpose |
|---|---|
| `Makefile` | `bsd.lib.mk` build standing in for `Makefile.am`: picks the port from `MACHINE_CPUARCH`, writes `ffi.h` from `ffi.h.in`, preprocesses `libffi.map.in` into the version script, writes `libffi.pc` |
| `nextbsd/fficonfig.h` | What `configure` would have found for amd64 and arm64 with clang + lld. Not installed. Same approach as Apple's `darwin/include` |
| `patches/` | The NextBSD patch series (below) |
| `VENDORED.md` | This file |

`build-userland.sh` copies this directory to `.build/libffi-<arch>`, applies
`patches/*.patch` in order, and runs the `Makefile` there through the
FreeBSD cross build environment.

## What it installs

| Path | |
|---|---|
| `/usr/lib/libffi.so.7`, `libffi.so`, `libffi.a` | SONAME `libffi.so.7` |
| `/usr/include/ffi.h`, `/usr/include/ffitarget.h` | Per-architecture, as upstream's `make install` writes them |
| `/usr/libdata/pkgconfig/libffi.pc` | `Version: 3.4.0-rc2`, `Libs: -L/usr/lib -lffi` |
| `/usr/share/man/man3/ffi*.3.gz` | |

**SONAME.** The library's name is what this source's own build gives
it: `libtool-version` is `8:0:1`, which libtool turns into
`libffi.so.7`. Upstream 3.4.0 has `9:0:1` (`libffi.so.8`); Apple took
its version back to 8 when it removed upstream's Intel CET support
(commit `78556561`), which also took the x86-64 trampoline back from
32 to 24 bytes, so `sizeof(ffi_closure)` on amd64 is 3.3's, not
3.4's. The symbol versions are still 3.4's (`LIBFFI_BASE_8.0`, ...).
Everything that links it is built on NextBSD against these headers,
so the mix doesn't matter in practice. It would if prebuilt binaries
linked against upstream's `libffi.so.8` were ever expected to run
against it, which they should not be.

## What Apple changed, and what is left on NextBSD

Apple's changes against upstream `v3.4.0-rc2`:

- **Trampoline tables** (`closures.c`, `aarch64/ffi.c`, `aarch64/trampoline.S`):
  `vm_remap` of a trampoline page, `FFI_TRAMPOLINE_WHOLE_DYLIB`, arm64_32.
  Only under `FFI_EXEC_TRAMPOLINE_TABLE` / `__MACH__`; not built on
  NextBSD, where closures come from `FFI_MMAP_EXEC_WRIT` (as
  `configure.ac` sets for `*-*-freebsd*`).
- **Pointer authentication** (`HAVE_PTRAUTH`): arm64e only.
- **`ffi_find_closure_for_code_np()`** (`x86/ffi64.c`, `aarch64/ffi.c`,
  declared in `ffi.h` for x86-64): Apple SPI. It is compiled but not
  in `libffi.map.in`, so the version script keeps it local, as it is
  on upstream.
- **Intel CET removed** (`x86/unix64.S`, `x86/ffitarget.h`, `x86/ffi64.c`):
  see SONAME above. NextBSD builds without `-fcf-protection`, so
  nothing on NextBSD needs the `endbr64` landing pads.
- **`ffi_prep_closure` made optional** (`prep_cif.c`, behind
  `FFI_LEGACY_CLOSURE_API`): `nextbsd/fficonfig.h` sets it, so the
  deprecated call is built and exported as on upstream.
- `sysv.S` `cfi_startproc` moved after the label; an Xcode project and
  the `darwin/` headers.

So on NextBSD this is upstream 3.4.0-rc2 without CET.

## Patch series

| Patch | Upstream | Fix |
|---|---|---|
| `0001-Emit-.note.GNU-stack-on-FreeBSD-for-x86-and-aarch64` | `19cc1424` (3.7.0); aarch64 is NextBSD's | Every assembly object carries `.note.GNU-stack`, so none asks for an executable stack (CVE-2017-1000376 class). lld already defaults to a non-executable stack; this keeps it so under any linker |
| `0002-x86-64-copy-at-most-one-eightbyte-into-each-GPR-slot` | `d21881f5` (3.4.7) + `fe203ffb` (3.4.8) | A struct split across a GPR and an SSE register wrote past its GPR slot and clobbered SSE arguments; the first fix read past small arguments |
| `0003-Pass-large-structs-by-value-on-x86-64-and-aarch64` | `e409225b`, `fd1fe195`, `769b7366` (3.4.3); `3874fd98`, `93cfb655`, `af0f14cd` (3.4.4); `110156e1` (3.7.0); `a2ef91c7` (3.7.1) | Structs over 16 bytes reached the callee as a pointer to the caller's own object (callee writes changed the caller's struct). Plus that fix's follow-ups: an aarch64 strict-aliasing miscompile, truncated 16/32-bit integers on aarch64, `avalue[]` modified and left dangling, an aarch64 argument area too small for many large structs |
| `0004-Only-check-the-variadic-arguments-in-ffi_prep_cif_va` | `de95947a` (3.4.3) | `ffi_prep_cif_var` rejected legal fixed `float`/`char` arguments |
| `0005-dlmalloc-check-segment_holding-s-result-in-sys_trim` | `4639c933` (3.6.0) | NULL dereference in the closure allocator when its heap is already corrupt; now aborts |

## CVE check (E24 plan step 3)

Done 2026-10-24 against upstream `v3.4.0-rc2..v3.8.0` (610 commits) and
the release notes of 3.4.2 through 3.8.0.

**CVEs:** NVD lists one libffi CVE, **CVE-2017-1000376** (executable
stack, libffi 3.2.1, fixed in 3.3). Apple's tree has the 3.3 fix (the
Linux `.note.GNU-stack` sections); patch 0001 extends it to FreeBSD.
CVE-2015-7551 is Ruby's Fiddle, not libffi. libffi has **no GitHub
security advisories** and no CVE has been assigned against 3.4.0 or
later. So **no CVE is open against this source on NextBSD.**

**Memory-safety and wrong-code fixes since 3.4.0** that affect amd64 or
arm64 are all carried (patches 0002–0005).

**Reviewed and not carried**, because they don't apply to this build:

| Upstream | Why not |
|---|---|
| Static trampolines: `60671187` (fd close-on-exec), `b53b0042`, `ff30d376` (bounded `sscanf` of `/proc/self/maps`), `76883c62` | `tramp.c` is Linux-only and `FFI_EXEC_STATIC_TRAMP` is off |
| `50baa045` x86-64 closure argument buffer sized `n * 8` | Only vector types (added in 3.8.0) classify more than two eightbytes; 3.4.0-rc2 has none, so 16 bytes is enough |
| `da905276` closures `dlmmap` locking, PaX MPROTECT | The PaX part is Linux-only; the locking change is redundant here because dlmalloc's global lock already serialises every `dlmmap` call |
| `fc67b31a`, `2a0b4b52` dlmalloc 2.8.6, TSan-clean `mparams` init | A data race on one-time initialisation reported by ThreadSanitizer; the full allocator update is too large for this pin |
| `3ac265d5` CET jump-table slots | Apple removed CET support |
| `45d284f2`, `98881ecb`, `d93c0be8`, `205fc530`, `5ceddf40`, `547449d7` (aarch64 PAC, BTI, GCS) | New hardening features, not fixes. Without the GNU property note the library just isn't BTI-enforced, as now |
| `7c3144c8` int128 split on aarch64 | Darwin's arm64 ABI only |
| `e6db2d38`, `93b274ce`, `5eaa8a38` | Vector type support, not in 3.4.0-rc2 |
| `5b8fa3fe`, `ebe306be` and the other i386, Win64, PA, PowerPC, MIPS, s390, RISC-V fixes | Not amd64 or arm64 |

Re-run this check when the pin moves: list
`git log v3.4.0-rc2..<new tag>` for `src/{x86,aarch64}`, the generic
`src/*.c` and `include/`, and the release notes.
