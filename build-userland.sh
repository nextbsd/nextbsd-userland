#!/usr/bin/env bash
# =============================================================================
# build-userland.sh — cross-build driver for the NextBSD Apple SYSTEM LAYER
# =============================================================================
#
# SCOPE
#   Cross-compiles ONLY Tiers 0-2 of the Apple userland — the "system layer":
#   the host MIG tool, the libsystem_* core libraries, launchd/launchctl, and
#   the first-tier system daemons (configd, IOKit facade, kext_tools, notify,
#   ASL/syslog, IPConfiguration, mDNSResponder, DiskArbitration, hostnamed),
#   for ONE architecture, into $DESTDIR (/stage), in strict dependency order.
#
# INTENTIONALLY EXCLUDED (do NOT add here)
#   * Command suites / the broad userland — those come from FreeBSD base and
#     are curated via compat's srclist, not built in this repo.
#   * PAM (OpenPAM port, build.sh step 3y2 ~2609+) — a later tier; it overlays
#     FreeBSD-pam and needs its own conflict handling. Not a Tier 0-2 concern.
#     (The one PAM piece built here is pam_launchd, a single module for the
#     inherited OpenPAM that no base path collides with; see its block.)
#   * Every smoke/round-trip TEST binary build.sh compiles next to each
#     component (test_libmach, configtest, iokittest, sctest, datest, ...).
#     Those exec just-built TARGET binaries on the host — impossible when
#     cross-building. The runtime assertions they provided MOVE to the boot
#     test (CI runs them inside the booted VM, not here).
#
# EXECUTION CONTEXT (set up by CI BEFORE this script runs)
#   Running inside the nextbsd-toolchain container on a GitHub Ubuntu
#   (x86_64) runner. CI has already:
#     1. staged a SYSROOT from compat's continuous base into $SYSROOT, and
#     2. run, in the freebsd src tree:
#          cd /usr/src && ./tools/build/make.py \
#              --cross-bindir=$CROSS_BINDIR TARGET=$T TARGET_ARCH=$TA \
#              toolchain _includes
#        so the cross clang/lld exist in $CROSS_BINDIR and the staged
#        sysroot carries the FreeBSD base headers.
#
# HOW EACH COMPONENT TYPE IS BUILT (cross-shaping, vs build.sh's host+chroot)
#   * bsd.lib.mk / bsd.prog.mk components (the majority): build.sh ran these
#     as `make -C src/X DESTDIR=$WORK/rootfs ...` on a FreeBSD host. Here they
#     must compile for $TA, so we drive them THROUGH the FreeBSD cross build
#     env, which sets MACHINE_ARCH, the cross clang, and the sysroot
#     consistently:
#         run_buildenv "make -C $ROOT/src/X DESTDIR=$DESTDIR SYSROOT=$SYSROOT ..."
#     via `make.py ... buildenv BUILDENV_SHELL=<script>`. We pass the same
#     DESTDIR / PREFIX / SYSROOT / MIGOUT variables build.sh passed, verbatim,
#     so the component Makefiles behave identically — only the compiler/arch
#     underneath changes.
#   * The 2 CMake components (libdispatch, swift-foundation-icu) are HOST
#     cmake/ninja runs that emit TARGET objects via -DCMAKE_TOOLCHAIN_FILE.
#     No chroot; no `make.py buildenv`.
#   * migcom (Tier 0) is a HOST tool — built with the runner's plain `cc`,
#     NOT cross. Its codegen output is arch-neutral, so one host migcom serves
#     every target's MIG steps.
#
# WHAT WAS DROPPED FROM build.sh's blocks (per cross-shaping rules)
#   * Every `chroot $WORK/rootfs ldconfig -m/-r` hint-prime and every
#     `chroot ... ldd ... | grep` resolution check. We can't chroot into a
#     foreign-arch rootfs on the x86 runner. Where a resolution assertion was
#     load-bearing we replace it with a cross `readelf -d` DT_NEEDED check
#     (see needed_check); elsewhere we omit it (rtld resolution is re-verified
#     in the boot test).
#   * Build-time runs of just-built TARGET binaries (test_corefoundation
#     ~1429, the kextdeps dependency smoke ~2005). Can't exec cross binaries.
#
# IDEMPOTENCE
#   Re-runnable: mkdir -p is harmless, installs overwrite, MIG out dirs are
#   recreated. Not a hermetic clean — CI gives a fresh checkout per run.
#
# STATUS
#   v0. Expected to need CI iteration — several blocks copy build.sh flags
#   verbatim and carry `# TODO verify under cross` where behaviour under the
#   cross build env (vs build.sh's native host build) is unproven.
# =============================================================================

set -euo pipefail

# ---- required environment ----------------------------------------------------
: "${T:?set T to the target (e.g. amd64 / arm64)}"
: "${TA:?set TA to the target_arch (e.g. amd64 / aarch64)}"
: "${SYSROOT:?set SYSROOT to the staged sysroot}"
: "${DESTDIR:=/stage}"
: "${CROSS_BINDIR:?set CROSS_BINDIR to the dir holding the cross clang/lld}"

# Root of the freebsd src tree make.py lives in (CI's `cd /usr/src`).
: "${SRCTREE:=/usr/src}"

# Repo root — this script lives at the top of nextbsd-userland.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/src"

# Per-target CMake cross toolchain file (we wrote cross-amd64 / cross-arm64).
CMAKE_TOOLCHAIN="$ROOT/cmake/cross-${T}.cmake"

# MIG artifact dirs (pure build artifacts; never installed into $DESTDIR).
MIGROOT="${MIGROOT:-$ROOT/.mig}"

# Host migcom — built by Tier 0 (FreeBSD legacy stage) and used build-only by
# every MIG step. It is a runner-native (x86_64-Linux) binary, so it is NOT
# shipped; Tier 0 points MIGCOM at its legacy build location.
# (mig.sh shells out to $MIGCOM; $MIGCC preprocesses .defs — host cc, arch-neutral.)
MIGCOM=""   # set by Tier 0 (legacy-built, build-scratch — never shipped)
MIG_SH="$SRC/bootstrap_cmds/migcom.tproj/mig.sh"
MIGCC="${MIGCC:-cc}"

export SYSROOT DESTDIR CROSS_BINDIR

# Explicit cross-clang for the few hand-links (ioreg, ipconfig CLI) that build.sh
# did as a bare `cc`. Mirrors the cmake toolchain files (triple + sysroot + lld),
# and is run DIRECTLY — NOT via run_buildenv: inside `make.py buildenv` bmake
# expands `$CC` as `${C}C` ("C: not found"), and the buildenv $CC carries no
# --target. SYSROOT/CROSS_BINDIR are container paths, reachable here as-is.
case "$T" in
    amd64) CROSS_TRIPLE=x86_64-unknown-freebsd ;;
    arm64) CROSS_TRIPLE=aarch64-unknown-freebsd ;;
    *)     CROSS_TRIPLE="${TA}-unknown-freebsd" ;;
esac
CROSS_CC="$CROSS_BINDIR/clang --target=$CROSS_TRIPLE --ld-path=$CROSS_BINDIR/ld.lld"

# ---- logging helpers ---------------------------------------------------------
tier()  { echo; echo "############################################################"; \
          echo "## $*"; echo "############################################################"; }
comp()  { echo; echo "==> [$T/$TA] $*"; }
note()  { echo "    - $*"; }

# ---- run a make/shell command inside the FreeBSD cross build env -------------
# Replaces build.sh's `make -C src/X ... DESTDIR=$WORK/rootfs` (native host)
# and its chroot wrappers. make.py's `buildenv` sets MACHINE/MACHINE_ARCH,
# CC=$CROSS_BINDIR/clang, the cross binutils, and the sysroot so the component
# Makefiles cross-compile without per-component CC plumbing.
run_buildenv() {
    local script="$1"
    ( cd "$SRCTREE" && ./tools/build/make.py \
        --cross-bindir="$CROSS_BINDIR" \
        TARGET="$T" TARGET_ARCH="$TA" \
        buildenv BUILDENV_SHELL="/bin/sh -ex -c '$script'" )
}

# Like run_buildenv but pinned to the RUNNER's native arch (amd64) — for HOST
# tools (migcom) that must execute on the x86_64 runner regardless of the job's
# target. Uses the FreeBSD buildenv (bmake + share/mk) so `.include <bsd.prog.mk>`
# parses; plain `make` on Ubuntu is GNU make and fails ("missing separator").
run_buildenv_host() {
    local script="$1"
    ( cd "$SRCTREE" && ./tools/build/make.py \
        --cross-bindir="$CROSS_BINDIR" \
        TARGET=amd64 TARGET_ARCH=amd64 \
        buildenv BUILDENV_SHELL="/bin/sh -ex -c '$script'" )
}

# ---- cross readelf DT_NEEDED sanity check (replaces chroot ldd assertions) ---
# build.sh asserted resolution with `chroot ... ldd ... | grep`. We can't run a
# foreign-arch ldd on the runner, so verify the just-linked TARGET object
# DECLARES the expected DT_NEEDED instead. Actual rtld resolution is re-checked
# in the boot test. $1 = path under $DESTDIR; $2 = soname substring to require.
#
# Reading the dynamic section is target-independent, so any ELF reader will
# do. The toolchain image's CROSS_BINDIR (/usr/lib/llvm-N/bin) ships clang and
# lld but no llvm-readelf, which left every check here a silent skip; fall back
# to llvm-readobj, versioned names on PATH, and the host binutils readelf
# (nextbsd/nextbsd-userland#354).
find_readelf() {
    local c
    for c in "$CROSS_BINDIR/llvm-readelf" "$CROSS_BINDIR/readelf" \
             "$CROSS_BINDIR/llvm-readobj"; do
        [ -x "$c" ] && { echo "$c"; return 0; }
    done
    for c in llvm-readelf llvm-readelf-19 llvm-readelf-20 llvm-readelf-18 \
             llvm-readobj readelf; do
        command -v "$c" 2>/dev/null && return 0
    done
    return 1
}
READELF="$(find_readelf || true)"

needed_check() {
    local obj="$1" want="$2" dyn
    if [ -z "$READELF" ]; then
        echo "FAIL: no ELF reader (llvm-readelf/llvm-readobj/readelf) for DT_NEEDED check of $obj"
        exit 1
    fi
    # Capture first: piping into grep -q under pipefail can SIGPIPE the reader.
    dyn="$("$READELF" -d "$obj" 2>&1)" \
        || { echo "FAIL: $READELF -d $obj: $dyn"; exit 1; }
    printf '%s\n' "$dyn" | grep -q "NEEDED.*$want" \
        || { echo "FAIL: $obj missing DT_NEEDED $want"; printf '%s\n' "$dyn" | grep NEEDED; exit 1; }
    note "DT_NEEDED ok: $(basename "$obj") -> $want"
}

# ---- run one mig.sh invocation (host migcom; arch-neutral codegen) -----------
# Mirrors build.sh's `MIGCC=cc MIGCOM=.../migcom /bin/sh mig.sh ...` pattern.
run_mig() {
    local outdir="$1"; shift
    ( cd "$outdir" && MIGCC="$MIGCC" MIGCOM="$MIGCOM" /bin/sh "$MIG_SH" "$@" )
}

echo "==> nextbsd-userland system-layer cross build"
echo "    T=$T TA=$TA"
echo "    SYSROOT=$SYSROOT"
echo "    DESTDIR=$DESTDIR"
echo "    CROSS_BINDIR=$CROSS_BINDIR"
echo "    CMAKE_TOOLCHAIN=$CMAKE_TOOLCHAIN"
echo "    READELF=${READELF:-<none>}"
[ -f "$CMAKE_TOOLCHAIN" ] || { echo "FAIL: missing $CMAKE_TOOLCHAIN"; exit 1; }
mkdir -p "$MIGROOT"

# =============================================================================
# TIER 0 — host MIG tool (built for the RUNNER, NOT cross)
# =============================================================================
tier "TIER 0 : migcom + mig wrapper (HOST tool)"

# migcom is a build-time HOST code generator (turns .defs into C). Build it the
# FreeBSD way — the SAME machinery that bootstraps yacc/lex: stage it into the
# in-container /usr/src and build it host-native via the `_legacy` stage with
# LOCAL_LEGACY_DIRS. The legacy loop runs `make obj includes all install
# DESTDIR=${WORLDTMP}/legacy` with BMAKE (the build host's gcc). Ephemeral,
# in-container — like the kernel build patching /usr/src. (`_legacy` is the
# exposed top-level wrapper that sets up WORLDTMP/BMAKE; the baked kernel-toolchain
# already ran it, so this just adds migcom atop the baked tools/build.)
#
# Placement mirrors the install location: migcom installs to /usr/libexec, so its
# source dir lives at /usr/src/libexec/migcom (FreeBSD convention). The Makefile's
# relative -I../../libmach/include then resolves to /usr/src/libmach/include, where
# we drop our libmach headers.
#
# BUILD-ONLY: the result is a runner-native x86_64-Linux binary — it must NOT ship
# on the FreeBSD ISO (it could not run there). So we do NOT install it into
# $DESTDIR; we point MIGCOM at its legacy build location for the MIG steps. `mig`
# is not a base-system runtime dependency — revisit cross-building a TARGET migcom
# only if we ever want `mig` on the installed system.
comp "migcom (legacy stage, host-native, build-only)"
rm -rf /usr/src/libexec/migcom /usr/src/libmach
mkdir -p /usr/src/libexec/migcom /usr/src/libmach
cp -a "$SRC/bootstrap_cmds/migcom.tproj/." /usr/src/libexec/migcom/
cp -a "$SRC/libmach/include" /usr/src/libmach/
( cd "$SRCTREE" && ./tools/build/make.py --cross-bindir="$CROSS_BINDIR" \
    TARGET="$T" TARGET_ARCH="$TA" \
    LOCAL_LEGACY_DIRS=libexec/migcom \
    _legacy )
MIGCOM=$(find /usr/obj -path '*/legacy/usr/libexec/migcom' -type f 2>/dev/null | head -1)
[ -n "$MIGCOM" ] || { echo "FAIL: legacy stage produced no migcom"; exit 1; }
note "host migcom built (build-only, not shipped): $MIGCOM"
# Smoke it — host binary, run directly (no chroot/cross).
MIGCC="$MIGCC" MIGCOM="$MIGCOM" /bin/sh "$MIG_SH" -version \
    || { echo "FAIL: mig -version (host) exited non-zero"; exit 1; }
note "host migcom installed at $MIGCOM"

# =============================================================================
# TIER 1 — core system libraries (cross)
# =============================================================================
tier "TIER 1 : libmach -> libdispatch -> libxpc -> liblaunch -> ICU -> CF"

# ---- Populate the cross sysroot (FOUNDATIONAL) ------------------------------
# The buildenv compiler is invoked with --sysroot=${WORLDTMP} and NO host
# include/lib fallback (Makefile.inc1 XCFLAGS, :879/:887). The baked
# kernel-toolchain skips the _includes/_libraries stages
# (KERNEL_TOOLCHAIN_TGTS = TOOLCHAIN_TGTS:N_obj:N_cleanobj:N_includes:N_libraries),
# so ${WORLDTMP} has no base headers (sys/types.h, net/*) or libs. Stage the
# compat-continuous base (the workflow extracted it to $SYSROOT) INTO ${WORLDTMP}
# and collapse SYSROOT->${WORLDTMP}, so the compiler's --sysroot AND the
# Makefiles' -I$SYSROOT/-L$SYSROOT resolve against one populated root. Copy only
# headers+libs (usr/include, usr/lib, lib) — NOT usr/bin/bin/sbin, which would
# clobber WORLDTMP's cross-tools with FreeBSD-target binaries. DESTDIR stays
# /stage (clean artifact); sync_sysroot mirrors each built lib back into WORLDTMP.
WORLDTMP=${MIGCOM%/legacy/usr/libexec/migcom}
[ -d "$WORLDTMP/usr" ] || { echo "FAIL: could not derive WORLDTMP from MIGCOM=$MIGCOM"; exit 1; }
echo "==> staging compat base into WORLDTMP=$WORLDTMP; collapsing SYSROOT -> WORLDTMP"
for d in usr/include usr/lib lib; do
    [ -d "$SYSROOT/$d" ] && { mkdir -p "$WORLDTMP/$d"; cp -a "$SYSROOT/$d/." "$WORLDTMP/$d/"; }
done
SYSROOT="$WORLDTMP"
export SYSROOT

# ---- libmach (libsystem_kernel) ---------------------------------------------
# build.sh ~742-774. Installs mach/* headers into the sysroot FIRST — every
# later component (-I sysroot) and the CMake HAVE_MACH detection depend on
# them. bsd.lib.mk does NOT auto-create LIBDIR/INCSDIR/FILESDIR, so pre-make
# the /usr/lib/system convention dirs (carried verbatim from build.sh).
#
# NOTE: build.sh deferred installing <mach/mach.h> until just before libxpc so
# libdispatch's CMake __has_include(<mach/mach.h>) wouldn't auto-flip HAVE_MACH.
# We instead force -DHAVE_MACH=ON, which bypasses that detection entirely — so
# the deferral is unnecessary AND harmful: with HAVE_MACH=ON, libdispatch's
# SOURCES (mach_private.h) #include <mach/mach.h> and need it present to compile.
# So we keep mach.h installed (no removal).
# ---- libmach MIG clients (mach_port, task, host_priv, mach_host) ------------
# Generate libmach's MIG clients from the kernel's .defs, so the libmach entry
# points that used to fabricate success are real RPCs (nextbsd-kernel#125,
# #83, #84, #93).
#
# The .defs live in nextbsd-kernel, deliberately: each describes the WIRE, and
# a wire needs exactly one description or the two ends can drift -- which is
# the whole failure this work removes. The kernel repo generates its servers
# from the same files. This mirrors nextbsd-kernel's own build, which checks
# out THIS repo to build migcom.
#
# UserPrefix _kernelrpc_ in each .defs means the generated stubs are named
# _kernelrpc_<routine>, so they sit underneath the public entry points in
# mach_traps.c rather than colliding with them -- the libsyscall arrangement.
# libmach defines public task_* and host_* symbols (task_get_special_port,
# task_policy_*, host_info, semaphore_*, ...), so an unprefixed client would
# not link.
KERNEL_DEFS="${KERNEL_DEFS:-$ROOT/.kernel}"
LIBMACH_MIG="$MIGROOT/libmach"
rm -rf "$LIBMACH_MIG"; mkdir -p "$LIBMACH_MIG"

# subsystem:routine -- the routine is one the subsystem must contain, used to
# assert the prefix took effect.
LIBMACH_MIG_SUBS="mach_port:mach_port_get_set_status task:task_get_special_port host_priv:host_set_special_port mach_host:host_info"

for ent in $LIBMACH_MIG_SUBS; do
    sub=${ent%%:*}
    if [ ! -f "$KERNEL_DEFS/src-overlay/sys/sys/mach/$sub.defs" ]; then
        echo "FAIL: $sub.defs not found under KERNEL_DEFS=$KERNEL_DEFS"
        echo "      Expected a nextbsd-kernel checkout; CI provides one, and a"
        echo "      local build can point KERNEL_DEFS at its own clone."
        exit 1
    fi
done

# Includes, all from the kernel repo except the userland wire vocabulary
# (std_types / mach_types / machine_types.defs, which the kernel build also
# takes from here):
#   mach_debug_types.defs  -- every one of the four
#   clock_types.defs       -- host_priv.defs and mach_host.defs; userland
#                             ships no .defs for it
MIGTMP="$LIBMACH_MIG/incl"
mkdir -p "$MIGTMP/mach" "$MIGTMP/mach_debug"
cp "$SRC/libmach/include/mach/"*.defs "$MIGTMP/mach/"
cp "$KERNEL_DEFS/src-overlay/sys/sys/mach/clock_types.defs" "$MIGTMP/mach/"
cp "$KERNEL_DEFS/src-overlay/sys/sys/mach_debug/mach_debug_types.defs" "$MIGTMP/mach_debug/"

for ent in $LIBMACH_MIG_SUBS; do
    sub=${ent%%:*}
    probe=${ent#*:}
    user="${sub}User.c"
    cp "$KERNEL_DEFS/src-overlay/sys/sys/mach/$sub.defs" "$MIGTMP/mach/"
    run_mig "$LIBMACH_MIG" -I"$MIGTMP" \
        -header "${sub}_mig.h" -user "$user" \
        -server /dev/null \
        "$MIGTMP/mach/$sub.defs" \
        || { echo "FAIL: mig could not process $sub.defs"; exit 1; }
    test -s "$LIBMACH_MIG/$user" \
        || { echo "FAIL: mig produced no $user"; exit 1; }
    # The prefix is load-bearing: without it these would collide with the
    # public entry points. Assert it rather than trust it: the probe routine
    # is there under its prefixed name, and no generated entry point
    # (migcom writes "mig_external <type> <name>" on one line) lacks it.
    grep -qE "^mig_external [a-z_]+ _kernelrpc_${probe}\$" "$LIBMACH_MIG/$user" \
        || { echo "FAIL: $user has no _kernelrpc_$probe -- UserPrefix missing?"; exit 1; }
    grep -qE "^kern_return_t _kernelrpc_${probe}\$" "$LIBMACH_MIG/${sub}_mig.h" \
        || { echo "FAIL: ${sub}_mig.h declares no _kernelrpc_$probe -- UserPrefix missing?"; exit 1; }
    if grep -E '^mig_external [a-z_]+ ' "$LIBMACH_MIG/$user" | grep -vqE ' _kernelrpc_[A-Za-z0-9_]+$'; then
        echo "FAIL: $user defines UNPREFIXED entry points; would collide:"
        grep -E '^mig_external [a-z_]+ ' "$LIBMACH_MIG/$user" | grep -vE ' _kernelrpc_[A-Za-z0-9_]+$'
        exit 1
    fi
    grep -qE "^${probe}\(" "$LIBMACH_MIG/$user" \
        && { echo "FAIL: $user defines UNPREFIXED entry points; would collide"; exit 1; }
    echo "==> LIBMACH-MIG-OK: $(wc -l < "$LIBMACH_MIG/$user") lines of $sub client"
done

# ---- kernel event headers (PF_SYSTEM / SYSPROTO_EVENT) ----------------------
# <sys/sys_domain.h>, <sys/kern_event.h> and <net/net_kev.h> describe the
# kernel event socket's wire (nextbsd-kernel#292), so like the .defs above
# they live in nextbsd-kernel and are taken from the same checkout. Install
# them into the image and the sysroot; launchd and the configd stack include
# them from there. Stock FreeBSD has none of these paths.
for h in sys/sys_domain.h sys/kern_event.h net/net_kev.h; do
    if [ ! -f "$KERNEL_DEFS/src-overlay/sys/$h" ]; then
        echo "FAIL: $h not found under KERNEL_DEFS=$KERNEL_DEFS"
        echo "      Expected a nextbsd-kernel checkout; CI provides one, and a"
        echo "      local build can point KERNEL_DEFS at its own clone."
        exit 1
    fi
    for root in "$DESTDIR" "$SYSROOT"; do
        mkdir -p "$root/usr/include/${h%/*}"
        cp "$KERNEL_DEFS/src-overlay/sys/$h" "$root/usr/include/$h"
    done
done

comp "libmach (libsystem_kernel) — installs mach/* headers into sysroot first"
# bsd.lib.mk does NOT auto-create INCSDIR, hence the pre-creation here.
# mach_debug/ joins the list for <mach_debug/ipc_info.h>, which the generated
# mach_port MIG client needs (#83).
mkdir -p "$DESTDIR/usr/lib/system" \
         "$DESTDIR/usr/include/mach" \
         "$DESTDIR/usr/include/mach_debug" \
         "$DESTDIR/usr/libdata/pkgconfig"
run_buildenv "make -C $SRC/libmach DESTDIR=$DESTDIR PREFIX=/usr MIGOUT=$LIBMACH_MIG all install"
# Headers also need to be visible in the SYSROOT (not just DESTDIR) for the
# cross compiles of every later component that does -I\$SYSROOT/usr/include.
# DESTDIR==/stage and SYSROOT are distinct; mirror libmach's headers across.
# TODO verify under cross: if CI points SYSROOT at /stage this copy is a
# no-op; if SYSROOT is a separate staged tree it is required.
if [ "$SYSROOT" != "$DESTDIR" ]; then
    mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib/system" "$SYSROOT/usr/libdata/pkgconfig"
    cp -a "$DESTDIR/usr/include/mach"        "$SYSROOT/usr/include/" 2>/dev/null || true
    cp -a "$DESTDIR/usr/include/mach_debug"  "$SYSROOT/usr/include/" 2>/dev/null || true
    cp -a "$DESTDIR/usr/lib/system/."        "$SYSROOT/usr/lib/system/" 2>/dev/null || true
    cp -a "$DESTDIR/usr/libdata/pkgconfig/." "$SYSROOT/usr/libdata/pkgconfig/" 2>/dev/null || true
fi
# (mach/mach.h is kept — see note above; HAVE_MACH is forced, not auto-detected.)
# Apple-convention <servers/bootstrap.h> (build.sh ~874).
mkdir -p "$DESTDIR/usr/include/servers"
cp "$SRC/libmach/include/servers/bootstrap.h" "$DESTDIR/usr/include/servers/bootstrap.h"
[ "$SYSROOT" != "$DESTDIR" ] && { mkdir -p "$SYSROOT/usr/include/servers"; \
    cp "$SRC/libmach/include/servers/bootstrap.h" "$SYSROOT/usr/include/servers/bootstrap.h"; }
# DROPPED: chroot ldconfig hint-prime, test_libmach/test_mach_port/... builds
# (host-exec of cross binaries — moved to boot test).

sync_sysroot() {
    # Mirror freshly-installed libs+headers from DESTDIR into a distinct
    # SYSROOT so the next cross component links/includes against them.
    # Replaces build.sh's per-component `chroot ... ldconfig -m`. No-op when
    # SYSROOT == DESTDIR.
    [ "$SYSROOT" = "$DESTDIR" ] && return 0
    mkdir -p "$SYSROOT/usr/lib/system" "$SYSROOT/usr/include"
    cp -a "$DESTDIR/usr/lib/system/." "$SYSROOT/usr/lib/system/" 2>/dev/null || true
    cp -a "$DESTDIR/usr/include/."    "$SYSROOT/usr/include/"    2>/dev/null || true
}

# ---- libdispatch [CMAKE, host cmake -> target objs] -------------------------
# build.sh ~971-1039. build.sh ran cmake/ninja inside the chroot. Here cmake +
# ninja run on the HOST but cross-compile via -DCMAKE_TOOLCHAIN_FILE. Same
# -D install layout (PREFIX=/usr, LIBDIR=lib/system, header dirs, HAVE_MACH).
comp "libdispatch [cmake cross]"
DISPATCH_BUILD="$ROOT/.build/libdispatch-$T"
rm -rf "$DISPATCH_BUILD"; mkdir -p "$DISPATCH_BUILD"
# libdispatch's src/CMakeLists.txt does its OWN MIG step at configure time:
#   find_program(MIG_EXECUTABLE mig PATHS /usr/bin ... REQUIRED)
#   ... COMMAND ${MIG_EXECUTABLE} -I/usr/include ... protocol.defs
# We build migcom build-only (no `mig` on PATH), and its -I/usr/include can't see
# our mach .defs (they're in the sysroot). Hand CMake a `mig` wrapper that bakes
# in MIGCOM/MIGCC and prepends -I$SYSROOT/usr/include so <mach/*.defs> resolve.
MIG_BIN="$ROOT/.build/mig-bin"
mkdir -p "$MIG_BIN"
cat > "$MIG_BIN/mig" <<EOF
#!/bin/sh
export MIGCOM="$MIGCOM" MIGCC="$MIGCC"
exec /bin/sh "$MIG_SH" -I"$SYSROOT/usr/include" "\$@"
EOF
chmod +x "$MIG_BIN/mig"
cmake -G Ninja -S "$SRC/libdispatch" -B "$DISPATCH_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
    -DMIG_EXECUTABLE="$MIG_BIN/mig" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib/system \
    -DINSTALL_DISPATCH_HEADERS_DIR=/usr/include/dispatch \
    -DINSTALL_BLOCK_HEADERS_DIR=/usr/include \
    -DINSTALL_OS_HEADERS_DIR=/usr/include/os \
    -DINSTALL_PRIVATE_HEADERS=ON \
    -DHAVE_MACH:BOOL=ON \
    -DCMAKE_BUILD_TYPE=Release
DESTDIR="$DESTDIR" ninja -C "$DISPATCH_BUILD" install
# CMakeLists sets no SOVERSION, so the objects are unversioned. Both base libs
# now carry NextBSD-unique OUTPUT_NAMEs (nextbsd#378): libdispatch -> the REAL
# file libsystem_dispatch.so, and BlocksRuntime -> the REAL file
# libsystem_blocks.so. The base no longer creates libdispatch.so OR
# libBlocksRuntime.so at all (those names belong to Gershwin's separate stock
# copies) — only the .so.0 aliases remain, so nothing but NextBSD base components
# can pick these up.
( cd "$DESTDIR/usr/lib/system"
  ln -sf libsystem_dispatch.so libsystem_dispatch.so.0
  ln -sf libsystem_blocks.so   libsystem_blocks.so.0 )
sync_sysroot
# DROPPED: chroot ldconfig re-prime; test_libdispatch* host-exec builds.

# ---- libxpc -----------------------------------------------------------------
# build.sh ~1094-1164. bsd.lib.mk component; SYSROOT tells its Makefile where
# the in-build rootfs headers/libs are. Pass SYSROOT + DESTDIR + PREFIX exactly
# as build.sh did (no direct CFLAGS/LDFLAGS — bmake would clobber CFLAGS+=).
comp "libxpc"
# build.sh ~1115: install the <mach/mach.h> umbrella NOW (deferred past the
# libdispatch HAVE_MACH detection above).
cp "$SRC/libmach/include/mach/mach.h" "$DESTDIR/usr/include/mach/mach.h"
[ "$SYSROOT" != "$DESTDIR" ] && cp "$SRC/libmach/include/mach/mach.h" "$SYSROOT/usr/include/mach/mach.h"
# build.sh ~1127-1134: Apple-shim public headers consumers need from /usr/include.
mkdir -p "$DESTDIR/usr/include/uuid"
cp "$SRC/libxpc/uuid/uuid.h"   "$DESTDIR/usr/include/uuid/uuid.h"
cp "$SRC/libxpc/Availability.h" "$DESTDIR/usr/include/Availability.h"
cp "$SRC/libxpc/launch.h"       "$DESTDIR/usr/include/launch.h"
mkdir -p "$DESTDIR/usr/include/xpc"
run_buildenv "make -C $SRC/libxpc DESTDIR=$DESTDIR PREFIX=/usr SYSROOT=$SYSROOT all install"
sync_sysroot
# build.sh ~1156-1158: shape asserts. .so.4 is the lib, .so is the dev symlink.
test -f "$DESTDIR/usr/lib/system/libxpc.so.4" || { echo "FAIL: libxpc.so.4 missing"; exit 1; }
test -L "$DESTDIR/usr/lib/system/libxpc.so"   || { echo "FAIL: libxpc.so symlink missing"; exit 1; }
# DROPPED: chroot ldconfig hint + ldd. libxpc links --allow-shlib-undefined for
# bootstrap_* (resolved by liblaunch at runtime) — no DT_NEEDED to assert here.

# ---- launchd MIG stubs + liblaunch ------------------------------------------
# build.sh ~1192-1268. Phase I1a: run host mig over launchd's .defs; Phase I1b:
# build liblaunch (bsd.lib.mk) against those stubs via MIGOUT.
comp "launchd MIG stubs (host mig)"
MIG_OUT="$MIGROOT/launchd"
rm -rf "$MIG_OUT"; mkdir -p "$MIG_OUT"
MIG_INCS="-I$SRC/libmach/include -I$SRC/launchd/liblaunch -I$SRC/launchd/src"
for d in job job_forward job_reply internal helper mach_exc notify; do
    note "mig: ${d}.defs"
    run_mig "$MIG_OUT" $MIG_INCS \
        -header "${d}.h" -user "${d}User.c" \
        -server "${d}Server.c" -sheader "${d}Server.h" \
        "$SRC/launchd/src/${d}.defs" \
        || { echo "FAIL: mig could not process ${d}.defs"; exit 1; }
done
for f in job.h jobUser.c jobServer.c; do
    test -s "$MIG_OUT/$f" || { echo "FAIL: mig produced no $f from job.defs"; exit 1; }
done

comp "liblaunch"
run_buildenv "make -C $SRC/launchd/liblaunch DESTDIR=$DESTDIR PREFIX=/usr MIGOUT=$MIG_OUT SYSROOT=$SYSROOT all install"
sync_sysroot
# liblaunch calls Mach and dispatch_* itself, so it must name both; without
# them any dlopen of a liblaunch consumer (pam_launchd) fails (#354).
needed_check "$DESTDIR/usr/lib/system/liblaunch.so.1" "libsystem_kernel"
needed_check "$DESTDIR/usr/lib/system/liblaunch.so.1" "libsystem_dispatch"
# DROPPED: chroot ldconfig re-prime.

# ---- swift-foundation-icu [CMAKE] -------------------------------------------
# build.sh ~1299-1377. Second CMake component; host cmake/ninja -> target objs.
# Three vendored CMakeLists patches (drop Swift lang, U_DISABLE_RENAMING=1,
# standard install dirs) live in the source tree — no extra -D here.
comp "swift-foundation-icu [cmake cross]"
ICU_BUILD="$ROOT/.build/icu-$T"
rm -rf "$ICU_BUILD"; mkdir -p "$ICU_BUILD"
cmake -G Ninja -S "$SRC/swift-foundation-icu" -B "$ICU_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
    -DFREEBSD_HOST_CC="${HOSTCC:-cc}" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib/system \
    -DCMAKE_INSTALL_INCLUDEDIR=include \
    -DCMAKE_BUILD_TYPE=Release
DESTDIR="$DESTDIR" ninja -C "$ICU_BUILD" install
# build.sh ~1350-1352: libicucore.so alias for the _FoundationICU target.
# The library is ICU 76.1 (uvernum.h), so the versioned aliases say .76 (#441).
( cd "$DESTDIR/usr/lib/system"
  ln -sf lib_FoundationICU.so libicucore.so
  ln -sf lib_FoundationICU.so libicucore.so.76
  ln -sf lib_FoundationICU.so lib_FoundationICU.so.76 )
# Third-party ICU consumers (GNUstep, #441): <unicode/...> headers through an
# icucore/unicode link (not a global /usr/include/unicode), and pkg-config
# icu-uc/icu-i18n/icu-io, all -licucore. No libicuuc/libicui18n names.
mkdir -p "$DESTDIR/usr/include/icucore" "$DESTDIR/usr/libdata/pkgconfig"
ln -sfn ../_foundation_unicode "$DESTDIR/usr/include/icucore/unicode"
for pc in icu-uc icu-i18n icu-io; do
    install -m 0444 "$SRC/swift-foundation-icu/pkgconfig/$pc.pc" \
        "$DESTDIR/usr/libdata/pkgconfig/$pc.pc"
done
sync_sysroot

# ---- libCoreFoundation ------------------------------------------------------
# build.sh ~1379-1402. bsd.lib.mk; pre-create the CoreFoundation include dir
# (bsd.incs.mk doesn't). DEPLOYMENT_RUNTIME_SWIFT=0 + the ICU-using SRCS drop
# are baked into the Makefile, so the invocation is the plain DESTDIR/SYSROOT one.
comp "libCoreFoundation"
mkdir -p "$DESTDIR/usr/include/CoreFoundation"
run_buildenv "make -C $SRC/libCoreFoundation DESTDIR=$DESTDIR PREFIX=/usr SYSROOT=$SYSROOT all install"
sync_sysroot
test -f "$DESTDIR/usr/lib/system/libCoreFoundation.so.6" \
    || { echo "FAIL: libCoreFoundation.so.6 missing"; exit 1; }
# RELOCATED (build.sh ~1410-1431): test_corefoundation was BUILT and then RUN
# under chroot to prove CF initializes (CF resolves _CFGetCurrentDirectory /
# _CFThreadSetName eagerly at library init). We can't exec a cross binary on
# the x86 runner, so the CF-init runtime assertion MOVES to the boot test.

# =============================================================================
# TIER 2 — system daemons + facades (cross)
# =============================================================================
tier "TIER 2 : launchd -> configd -> SC -> IOKit -> kext_tools -> notify -> syslog -> IPConfig -> mDNS -> DA -> hostnamed"

# Man pages: bsd.man.mk's install doesn't mkdir MANDIR (same missing
# distrib-dirs/hierarchy pass as BINDIR below). Pre-create the section dirs
# once, up front, for every component that now ships a page (launchd/launchctl,
# configd, libIOKit/ioreg, kext_tools, notifyd + libnotify, the syslog/ASL
# stack, mDNSResponder, DiskArbitration, cpdup). libdispatch installs its
# dispatch(3) pages via CMake, which creates its own destination dir.
mkdir -p "$DESTDIR/usr/share/man/man1" "$DESTDIR/usr/share/man/man3" \
         "$DESTDIR/usr/share/man/man5" "$DESTDIR/usr/share/man/man8"

# ---- launchd daemon + launchctl ---------------------------------------------
# build.sh ~1433-1493. launchd (bsd.prog.mk) consumes the I1a MIG stubs + CF;
# launchctl links CF/ICU/xpc/dispatch/liblaunch via its Makefile + freebsd-shims.
comp "launchd daemon (post-CF)"
# Pre-create install dirs (bsd.prog.mk's install doesn't mkdir BINDIR; no
# distrib-dirs/hierarchy pass in this piecemeal cross-install). launchd -> /sbin,
# launchctl -> /bin.
mkdir -p "$DESTDIR/sbin" "$DESTDIR/bin"
run_buildenv "make -C $SRC/launchd/src DESTDIR=$DESTDIR MIGOUT=$MIG_OUT SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/sbin/launchd" || { echo "FAIL: /sbin/launchd not installed"; exit 1; }

comp "launchctl"
mkdir -p "$DESTDIR/bin"
run_buildenv "make -C $SRC/launchd/support DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/bin/launchctl" || { echo "FAIL: /bin/launchctl not installed"; exit 1; }
# build.sh ~1487-1492 asserted CF + ICU resolution with chroot ldd. Replace
# with cross DT_NEEDED checks.
needed_check "$DESTDIR/bin/launchctl" "libCoreFoundation"
needed_check "$DESTDIR/bin/launchctl" "lib_FoundationICU"   # TODO verify under cross: soname may be libicucore

# ---- configd ----------------------------------------------------------------
# build.sh ~1511-1543. Generate config.defs MIG stubs (host mig) here, for
# libSystemConfiguration below. The configd daemon itself (bsd.prog.mk,
# MIGOUT pointing at them) builds after libnotify: it links -lnotify (#339).
comp "configd MIG stubs (host mig)"
mkdir -p "$DESTDIR/usr/sbin"
CONFIGD_MIG="$MIGROOT/configd"
rm -rf "$CONFIGD_MIG"; mkdir -p "$CONFIGD_MIG"
run_mig "$CONFIGD_MIG" -I"$SRC/libmach/include" \
    -header config.h -user configUser.c \
    -server configServer.c -sheader configServer.h \
    "$SRC/configd/config.defs" \
    || { echo "FAIL: mig could not process config.defs"; exit 1; }
test -s "$CONFIGD_MIG/configServer.c" || { echo "FAIL: mig produced no configServer.c"; exit 1; }
# DROPPED: configtest/notifytest/patterntest/listtest/multitest (host-exec).

# ---- libSystemConfiguration -------------------------------------------------
# build.sh ~1621-1651. Reuses configUser.c MIG stubs (MIGOUT=$CONFIGD_MIG) +
# config_wire.c; links CF + liblaunch. Pre-create the SC include dir.
comp "libSystemConfiguration"
mkdir -p "$DESTDIR/usr/include/SystemConfiguration"
run_buildenv "make -C $SRC/libSystemConfiguration DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$CONFIGD_MIG all install"
sync_sysroot
test -f "$DESTDIR/usr/lib/system/libSystemConfiguration.so.1" \
    || { echo "FAIL: libSystemConfiguration.so.1 not installed"; exit 1; }
test -f "$DESTDIR/usr/include/SystemConfiguration/SCDynamicStore.h" \
    || { echo "FAIL: SCDynamicStore.h not installed"; exit 1; }
# DROPPED: sctest/scnotifytest/scbridgetest (host-exec) + chroot ldconfig/ldd.

# ---- libIOKit + ioreg -------------------------------------------------------
# build.sh ~1910-1939 (lib) and ~2059-2077 (ioreg). libIOKit is bsd.lib.mk;
# pre-create the IOKit include dir. ioreg(8) in build.sh was a standalone `cc`
# link of ioreg.c; cross-shape it through the build env with the cross clang.
# ---- KextManager MIG stubs + libkext (before libIOKit) ----------------------
# nextbsd-userland#438: KextManager is the client half of kextd's
# org.nextbsd.KernelExtensionServer, and on Darwin it and the OSKext engine
# live in IOKit.framework. So libkext (OSKext + KextManager + the MIG user
# stubs) is built here, before libIOKit, which links it whole; kextd builds
# the server stubs from the same MIGOUT later. The generated code imports
# <IOKit/kext/kextmanager_types.h>, so MIGOUT gets that path too.
# NO_SECURITY_FRAMEWORK drops the Authorization types (no Security.framework);
# __DARWIN_64_BIT_INO_T sizes mountpoint_t to MAXPATHLEN, which is FreeBSD's
# MNAMELEN (both 1024); mig's preprocessor sees no <sys/param.h>, so
# MAXPATHLEN is passed in.
comp "KextManager MIG stubs (host mig)"
KEXTMGR_MIG="$MIGROOT/kextmanager"
rm -rf "$KEXTMGR_MIG"; mkdir -p "$KEXTMGR_MIG/IOKit/kext"
cp "$SRC/kext_tools/kext.subproj/kextmanager_types.h" "$KEXTMGR_MIG/IOKit/kext/"
run_mig "$KEXTMGR_MIG" -I"$SRC/libmach/include" \
    -DNO_SECURITY_FRAMEWORK -D__DARWIN_64_BIT_INO_T=1 -DMAXPATHLEN=1024 \
    -header kextmanager_mig.h -user kextmanager_migUser.c \
    -server kextmanager_migServer.c -sheader kextmanager_migServer.h \
    "$SRC/kext_tools/kext.subproj/kextmanager_mig.defs" \
    || { echo "FAIL: mig could not process kextmanager_mig.defs"; exit 1; }
for f in kextmanager_mig.h kextmanager_migUser.c kextmanager_migServer.c; do
    test -s "$KEXTMGR_MIG/$f" || { echo "FAIL: mig produced no $f"; exit 1; }
done

comp "libkext (OSKext + KextManager, for libIOKit)"
run_buildenv "make -C $SRC/kext_tools/kext.subproj SYSROOT=$SYSROOT MIGOUT=$KEXTMGR_MIG all"
test -f "$SRC/kext_tools/kext.subproj/libkext.a" || { echo "FAIL: libkext.a not built"; exit 1; }

comp "libIOKit"
mkdir -p "$DESTDIR/usr/include/IOKit"
run_buildenv "make -C $SRC/libIOKit DESTDIR=$DESTDIR SYSROOT=$SYSROOT LIBKEXT=$SRC/kext_tools/kext.subproj/libkext.a all install"
# The public kext API headers, as IOKit.framework ships them: KextManager.h
# (and the libkern/OSReturn.h it includes) and the MIG types.
mkdir -p "$DESTDIR/usr/include/IOKit/kext" "$DESTDIR/usr/include/libkern"
install -m 0644 "$SRC/kext_tools/kext.subproj/KextManager.h" \
    "$SRC/kext_tools/kext.subproj/kextmanager_types.h" \
    "$SRC/kext_tools/kext.subproj/NBSDFilesystemKext.h" "$DESTDIR/usr/include/IOKit/kext/"
install -m 0644 "$SRC/kext_tools/compat/libkern/OSReturn.h" "$DESTDIR/usr/include/libkern/"
sync_sysroot
test -f "$DESTDIR/usr/lib/system/libIOKit.so.1" || { echo "FAIL: libIOKit.so.1 not installed"; exit 1; }
test -f "$DESTDIR/usr/include/IOKit/IOKitLib.h" || { echo "FAIL: IOKit/IOKitLib.h not installed"; exit 1; }

comp "ioreg(8)"
mkdir -p "$DESTDIR/usr/sbin"
# build.sh ~2064-2071 used a bare `cc -fblocks ... -lIOKit -lCoreFoundation
# -lsystem_kernel -llaunch -lpthread`. Cross-shape: same flags, cross clang +
# sysroot, --allow-shlib-undefined preserved. TODO verify under cross: if
# ioreg gains a Makefile, prefer run_buildenv make over this hand link.
$CROSS_CC -fblocks --sysroot="$SYSROOT" \
    -I"$SYSROOT/usr/include" -L"$SYSROOT/usr/lib/system" \
    -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
    -o "$DESTDIR/usr/sbin/ioreg" "$SRC/libIOKit/ioreg.c" \
    -lIOKit -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$DESTDIR/usr/sbin/ioreg" || { echo "FAIL: /usr/sbin/ioreg not installed"; exit 1; }
needed_check "$DESTDIR/usr/sbin/ioreg" "libIOKit"
# DROPPED: iokittest/iokitmatchtest/iokitnotify* (host-exec).

comp "caffeinate(8)"
mkdir -p "$DESTDIR/usr/bin"
run_buildenv "make -C $SRC/caffeinate DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/bin/caffeinate" || { echo "FAIL: /usr/bin/caffeinate not installed"; exit 1; }
needed_check "$DESTDIR/usr/bin/caffeinate" "libIOKit"

# ---- kext_tools (libkext + OSKext CLIs + kextd) -----------------------------
# build.sh ~1941-2018. SUBDIR build (bsd.prog.mk + bsd.lib.mk) builds
# kextload/kextunload/kextstat/kextdeps + libexec/kextd; installs sys/iocatalogue.h.
comp "kext_tools"
mkdir -p "$DESTDIR/usr/sbin" "$DESTDIR/usr/libexec" "$DESTDIR/usr/include/sys"
run_buildenv "make -C $SRC/kext_tools DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$KEXTMGR_MIG all install"
for b in kextload kextunload kextstat kextdeps; do
    test -x "$DESTDIR/usr/sbin/$b" || { echo "FAIL: /usr/sbin/$b not installed"; exit 1; }
done
test -x "$DESTDIR/usr/libexec/kextd" || { echo "FAIL: kextd not installed"; exit 1; }
needed_check "$DESTDIR/usr/sbin/kextload" "libCoreFoundation"
needed_check "$DESTDIR/usr/libexec/kextd" "libCoreFoundation"
# build.sh ~1982-1984 installs the IOCatalogue ABI header for other consumers.
install -m 0644 "$SRC/kext_tools/kextd/iocatalogue.h" "$DESTDIR/usr/include/sys/iocatalogue.h"
echo "==> kext_tools built; sys/iocatalogue.h installed"
# RELOCATED (build.sh ~2000-2018): the kextdeps dependency-resolution smoke
# RAN /usr/sbin/kextdeps under chroot to assert base-before-leaf load order.
# That execs a cross binary — the assertion MOVES to the boot test.
# DROPPED: test_kextd_mach (host-exec).

# ---- Libnotify (libnotify) + notifyd ----------------------------------------
# build.sh ~2122-2177 (lib) and ~2221-2238 (daemon). Generate notify MIG stubs
# (host mig) + the short-name symlinks notifyd/notify_proc include; build
# libnotify (bsd.lib.mk), then notifyd (bsd.prog.mk) against the same MIGOUT.
comp "libnotify MIG stubs (host mig)"
LIBNOTIFY_MIG="$MIGROOT/libnotify"
rm -rf "$LIBNOTIFY_MIG"; mkdir -p "$LIBNOTIFY_MIG"
NOTIFY_INCS="-I$SRC/libmach/include -I$SRC/Libnotify"
for d in notify_ipc notify_old_ipc; do
    note "mig: ${d}.defs"
    run_mig "$LIBNOTIFY_MIG" $NOTIFY_INCS \
        -header "${d}.h" -user "${d}User.c" \
        -server "${d}Server.c" -sheader "${d}Server.h" \
        "$SRC/Libnotify/${d}.defs" \
        || { echo "FAIL: mig could not process ${d}.defs"; exit 1; }
done
for f in notify_ipc.h notify_ipcUser.c; do
    test -s "$LIBNOTIFY_MIG/$f" || { echo "FAIL: mig produced no $f from notify_ipc.defs"; exit 1; }
done

# The Mach NOTIFICATION subsystem (`subsystem notify 64`), which is a
# different thing from notify_ipc and must be generated separately.
#
# notifyd demuxes do_notify_subsystem on its mach_notifs_channel to receive
# MACH_NOTIFY_SEND_POSSIBLE and MACH_NOTIFY_DEAD_NAME. Those msgids are 64-73.
# This used to be satisfied by symlinking notify_ipcServer.c to notifyServer.c,
# which aliased do_notify_subsystem to _notify_ipc_subsystem -- base 1000. The
# ranges do not overlap, so no notification ever matched the demux and every
# one was destroyed. notifyd suspends a client on MACH_SEND_TIMED_OUT and waits
# for SEND_POSSIBLE to resume it, so a suspended client was never resumed
# (#78).
#
# -DMACH_NOTIFY_SEND_POSSIBLE_EXPECTED is REQUIRED and not cosmetic: notify.defs
# guards mach_notify_send_possible behind it and emits `skip;` otherwise. Built
# without it, this produces a notification server that handles dead_name and
# friends while still silently dropping the one notification #78 needs.
# Verified by generating both ways and diffing the routine table.
note "mig: notify.defs (Mach notification subsystem, base 64)"
# The -header/-user names are deliberately NOT notify.h / notifyUser.c.
# MIGOUT is on notifyd's include path, and libnotify's PUBLIC notify.h --
# which defines NOTIFY_STATUS_OK and friends -- would be shadowed by a
# generated file of the same name, so notifyd.c fails to compile with
# "use of undeclared identifier 'NOTIFY_STATUS_OK'". notifyServer.h does not
# reference the -header output at all (it includes <mach/notify.h>), so the
# name is free to be anything that does not collide.
run_mig "$LIBNOTIFY_MIG" $NOTIFY_INCS -DMACH_NOTIFY_SEND_POSSIBLE_EXPECTED=1 \
    -header "mach_notify_gen.h" -user "mach_notify_genUser.c" \
    -server "notifyServer.c" -sheader "notifyServer.h" \
    "$SRC/Libnotify/notify.defs" \
    || { echo "FAIL: mig could not process notify.defs"; exit 1; }
for f in notifyServer.c notifyServer.h; do
    test -s "$LIBNOTIFY_MIG/$f" || { echo "FAIL: mig produced no $f from notify.defs"; exit 1; }
done
grep -q "do_mach_notify_send_possible" "$LIBNOTIFY_MIG/notifyServer.c" \
    || { echo "FAIL: notifyServer.c has no send_possible -- is MACH_NOTIFY_SEND_POSSIBLE_EXPECTED set?"; exit 1; }
# libnotify's public notify.h must never be shadowed by MIG output.
if [ -e "$LIBNOTIFY_MIG/notify.h" ]; then
    echo "FAIL: MIG emitted notify.h into MIGOUT -- it shadows libnotify's public"
    echo "      header, and notifyd.c then fails on NOTIFY_STATUS_OK."
    exit 1
fi

comp "libnotify"
mkdir -p "$DESTDIR/usr/lib/system"
run_buildenv "make -C $SRC/Libnotify DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$LIBNOTIFY_MIG all install"
sync_sysroot
test -f "$DESTDIR/usr/lib/system/libnotify.so.1" || { echo "FAIL: libnotify.so.1 not installed"; exit 1; }
echo "==> NOTIFY-LIB-OK"

# configd builds AFTER libnotify — it posts network_change via -lnotify (#339).
comp "configd"
run_buildenv "make -C $SRC/configd DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$CONFIGD_MIG all install"
test -x "$DESTDIR/usr/sbin/configd" || { echo "FAIL: /usr/sbin/configd not installed"; exit 1; }
echo "==> CONFIGD-BUILD-OK"

# ---- ASL / syslog stack -----------------------------------------------------
# build.sh ~2179-2295. Generate asl_ipc MIG stubs (host mig); build
# libsystem_asl (lib) + syslogd + aslmanager + syslog(1) (progs), all against
# MIGOUT=$ASL_MIG. aslcommon is a static lib pulled in by the component Makefiles.
comp "ASL MIG stubs (host mig)"
ASL_MIG="$MIGROOT/asl"
rm -rf "$ASL_MIG"; mkdir -p "$ASL_MIG"
ASL_INCS="-I$SRC/libmach/include -I$SRC/syslog/aslcommon -I$SRC/syslog/libsystem_asl.tproj/include"
run_mig "$ASL_MIG" $ASL_INCS \
    -header asl_ipc.h -user asl_ipcUser.c \
    -server asl_ipcServer.c -sheader asl_ipcServer.h \
    "$SRC/syslog/aslcommon/asl_ipc.defs" \
    || { echo "FAIL: mig could not process asl_ipc.defs"; exit 1; }
for f in asl_ipc.h asl_ipcUser.c; do
    test -s "$ASL_MIG/$f" || { echo "FAIL: mig produced no $f from asl_ipc.defs"; exit 1; }
done

comp "libsystem_asl"
mkdir -p "$DESTDIR/usr/lib/system"
run_buildenv "make -C $SRC/syslog/libsystem_asl.tproj DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$ASL_MIG all install"
sync_sysroot
test -f "$DESTDIR/usr/lib/system/libsystem_asl.so.1" || { echo "FAIL: libsystem_asl.so.1 not installed"; exit 1; }
test -f "$DESTDIR/usr/include/asl.h" || { echo "FAIL: /usr/include/asl.h not installed"; exit 1; }
echo "==> ASL-LIB-OK"

# notifyd builds AFTER libsystem_asl — it links -lnotify AND -lsystem_asl.
comp "notifyd"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/Libnotify/notifyd DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$LIBNOTIFY_MIG all install"
test -x "$DESTDIR/usr/sbin/notifyd" || { echo "FAIL: /usr/sbin/notifyd not installed"; exit 1; }
echo "==> NOTIFYD-BUILD-OK"

comp "syslogd"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/syslog/syslogd.tproj DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$ASL_MIG all install"
test -x "$DESTDIR/usr/sbin/syslogd" || { echo "FAIL: /usr/sbin/syslogd not installed"; exit 1; }
echo "==> SYSLOGD-BUILD-OK"

comp "aslmanager"
run_buildenv "make -C $SRC/syslog/aslmanager.tproj DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$ASL_MIG all install"
test -x "$DESTDIR/usr/sbin/aslmanager" || { echo "FAIL: /usr/sbin/aslmanager not installed"; exit 1; }
echo "==> ASLMANAGER-BUILD-OK"

comp "syslog(1) CLI"
mkdir -p "$DESTDIR/usr/bin"
run_buildenv "make -C $SRC/syslog/util.tproj DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$ASL_MIG all install"
test -x "$DESTDIR/usr/bin/syslog" || { echo "FAIL: /usr/bin/syslog not installed"; exit 1; }
echo "==> SYSLOG-CLI-BUILD-OK"

# ---- IPConfiguration --------------------------------------------------------
# build.sh ~2297-2391. Generate ipconfig.defs MIG stubs (host mig); build
# ipconfigd (bsd.prog.mk) with MIGOUT. build.sh also hand-`cc`-links the
# `ipconfig` CLI from ipconfig.c + the MIG user stub; cross-shape that link.
comp "ipconfigd"
mkdir -p "$DESTDIR/usr/sbin"
IPCFG_MIG="$MIGROOT/ipcfg"
rm -rf "$IPCFG_MIG"; mkdir -p "$IPCFG_MIG"
run_mig "$IPCFG_MIG" -I"$SRC/libmach/include" -I"$SRC/IPConfiguration" \
    -header ipconfig.h -user ipconfigUser.c \
    -server ipconfigServer.c -sheader ipconfigServer.h \
    "$SRC/IPConfiguration/ipconfig.defs" \
    || { echo "FAIL: mig could not process ipconfig.defs"; exit 1; }
test -s "$IPCFG_MIG/ipconfigServer.c" || { echo "FAIL: mig produced no ipconfigServer.c"; exit 1; }
run_buildenv "make -C $SRC/IPConfiguration DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$IPCFG_MIG all install"
test -x "$DESTDIR/usr/sbin/ipconfigd" || { echo "FAIL: /usr/sbin/ipconfigd not installed"; exit 1; }

comp "ipconfig(8) CLI"
# build.sh ~2376-2388 bare `cc` link. Same flags (-Wno-macro-redefined,
# --allow-shlib-undefined) under the cross clang.  TODO verify under cross.
# CF + SC (+ dispatch/blocks, which the SC header pulls) for `set` /
# `getsummary` (property lists) and `waitall` (SCDynamicStore) — the
# same libraries ipconfigd links.
$CROSS_CC --sysroot="$SYSROOT" -fblocks \
    -I"$IPCFG_MIG" -I"$SRC/IPConfiguration" \
    -I"$SRC/launchd/liblaunch" -I"$SRC/launchd/freebsd-shims" \
    -I"$SYSROOT/usr/include" -L"$SYSROOT/usr/lib/system" \
    -Wno-macro-redefined -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
    -o "$DESTDIR/usr/sbin/ipconfig" \
    "$SRC/IPConfiguration/ipconfig.c" "$IPCFG_MIG/ipconfigUser.c" \
    -llaunch -lsystem_kernel \
    -lSystemConfiguration -lCoreFoundation -lsystem_dispatch -l:libsystem_blocks.so
test -x "$DESTDIR/usr/sbin/ipconfig" || { echo "FAIL: /usr/sbin/ipconfig not built"; exit 1; }
echo "==> ipconfigd + ipconfig built"
# DROPPED: ipconfigtest/ipconfigrpctest (host-exec).

# ---- WLAN (wland + the wlan CLI) --------------------------------------------
# The 802.11 management daemon. Same shape as IPConfiguration above: generate the
# wlan.defs MIG stubs with the host mig, build the daemon (bsd.prog.mk) with
# MIGOUT, then hand-link the CLI from wlan.c + the MIG *user* stub.
#
# Built AFTER IPConfiguration deliberately: wland publishes
# State:/Network/Interface/<if>/AirPort, and ipconfigd's DHCP gate consumes it.
# They are independent binaries, but the dependency runs that way and the build
# order should read the same.
#
# wland drives STOCK wpa_supplicant from FreeBSD base — nothing to build here.
# It is NOT vendored, NOT patched, and NOT the security/wpa_supplicant port.
comp "wland"
mkdir -p "$DESTDIR/usr/sbin"
WLAN_MIG="$MIGROOT/wlan"
rm -rf "$WLAN_MIG"; mkdir -p "$WLAN_MIG"
run_mig "$WLAN_MIG" -I"$SRC/libmach/include" -I"$SRC/WLAN" \
    -header wlan.h -user wlanUser.c \
    -server wlanServer.c -sheader wlanServer.h \
    "$SRC/WLAN/wlan.defs" \
    || { echo "FAIL: mig could not process wlan.defs"; exit 1; }
test -s "$WLAN_MIG/wlanServer.c" || { echo "FAIL: mig produced no wlanServer.c"; exit 1; }
run_buildenv "make -C $SRC/WLAN DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$WLAN_MIG all install"
test -x "$DESTDIR/usr/sbin/wland" || { echo "FAIL: /usr/sbin/wland not installed"; exit 1; }

comp "wlan(8) CLI"
# Same bare cross-`cc` link as ipconfig(8). Pure-C MIG client — no CF, no SC, so
# it links only liblaunch + the Mach syscall shim. This is deliberately the ONLY
# way anything above the Mach line talks to wland: no Gershwin process has ever
# spoken Mach, and Gershwin's libdispatch is built with HAVE_MACH force-disabled,
# so a CLI is the bridge.
$CROSS_CC --sysroot="$SYSROOT" \
    -I"$WLAN_MIG" -I"$SRC/WLAN" \
    -I"$SRC/launchd/liblaunch" -I"$SRC/launchd/freebsd-shims" \
    -I"$SYSROOT/usr/include" -L"$SYSROOT/usr/lib/system" \
    -Wno-macro-redefined -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
    -o "$DESTDIR/usr/sbin/wlan" \
    "$SRC/WLAN/wlan.c" "$WLAN_MIG/wlanUser.c" \
    -llaunch -lsystem_kernel
test -x "$DESTDIR/usr/sbin/wlan" || { echo "FAIL: /usr/sbin/wlan not built"; exit 1; }
echo "==> wland + wlan built"

# ---- mDNSResponder + libdns_sd ----------------------------------------------
# build.sh ~2400-2461. mDNSResponder daemon (bsd.prog.mk) then libdns_sd
# (bsd.lib.mk). No MIG surface in these blocks.
comp "mDNSResponder"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/mDNSResponder DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/mDNSResponder" || { echo "FAIL: /usr/sbin/mDNSResponder not installed"; exit 1; }

comp "libdns_sd"
run_buildenv "make -C $SRC/mDNSResponder/libdns_sd DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
sync_sysroot
test -f "$DESTDIR/usr/lib/system/libdns_sd.so.1" || { echo "FAIL: libdns_sd.so.1 not installed"; exit 1; }
test -f "$DESTDIR/usr/include/dns_sd.h"          || { echo "FAIL: /usr/include/dns_sd.h not installed"; exit 1; }

# dns-sd command-line client (bsd.prog.mk). Links the libdns_sd built+synced
# above; installs /usr/bin/dns-sd. Upstream Clients/dns-sd.c + ClientCommon.c.
comp "dns-sd"
run_buildenv "make -C $SRC/mDNSResponder/Clients DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/bin/dns-sd" || { echo "FAIL: /usr/bin/dns-sd not installed"; exit 1; }
echo "==> mDNSResponder + libdns_sd + dns-sd built"
# DROPPED: mdnstest/dnssdtest (host-exec).

# ---- DiskArbitration --------------------------------------------------------
# build.sh ~2463-2504. diskarbitrationd (bsd.prog.mk) links libIOKit (kernel
# notify channel). Built AFTER libIOKit so -lIOKit resolves from the staged
# /usr/lib/system. build.sh asserted libIOKit resolution with chroot ldd ->
# replace with cross DT_NEEDED check.
comp "diskarbitrationd"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/DiskArbitration DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/diskarbitrationd" || { echo "FAIL: /usr/sbin/diskarbitrationd not installed"; exit 1; }
needed_check "$DESTDIR/usr/sbin/diskarbitrationd" "libIOKit"
echo "==> diskarbitrationd built"
# DROPPED: datest (host-exec).

# ---- hostnamed --------------------------------------------------------------
# build.sh ~2506-2607. One-shot daemon; no MIG surface. bsd.prog.mk links
# SC/CF/dispatch/BlocksRuntime/system_kernel via its Makefile (-fblocks).
comp "hostnamed"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/hostnamed DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/hostnamed" || { echo "FAIL: /usr/sbin/hostnamed not installed"; exit 1; }
echo "==> hostnamed built"
# DROPPED: hostnametest/hostnameprefset/hostnamedhcpset/hostnamedmdnsset
#          (all host-exec CI fixtures — moved to the boot test).

# ---- scutil -----------------------------------------------------------------
# Apple's scutil(8) (configd scutil.tproj), trimmed to what libSystemConfiguration
# backs: the interactive SCDynamicStore session, -w, --prefs, --get/--set for the
# three host names, --error. bsd.prog.mk; links SC/CF + base libedit.
comp "scutil(8)"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/scutil DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/scutil" || { echo "FAIL: /usr/sbin/scutil not installed"; exit 1; }
needed_check "$DESTDIR/usr/sbin/scutil" "libSystemConfiguration"
echo "==> scutil built"

# ---- cpdup + nextbsd-installer (the on-target installer) --------------------
# The FTXUI TUI installer clones the running system onto the target disk with
# cpdup. Both are vendored under src/ but were not previously built; wired in
# here so they ship in the image for on-target install testing.
#
# cpdup: upstream uses a GNU make + pkg-config Makefile (Linux-shaped, pulls in
# libbsd-overlay). Cross that doesn't fit run_buildenv (bmake) — so hand-link
# with the cross clang like ioreg. -std=gnu99 keeps FreeBSD header visibility
# (st_flags, u_int, ...); -D_ST_FLAGS_PRESENT_ is the upstream FreeBSD knob; its
# MD5 (src/md5.c hard-includes <openssl/md5.h>) resolves against the base
# libcrypto. Lands on PATH at /usr/bin so do-install.sh finds `cpdup` bare.
comp "cpdup [cross hand-link]"
mkdir -p "$DESTDIR/usr/bin"
$CROSS_CC --sysroot="$SYSROOT" -O -pipe -std=gnu99 -D_ST_FLAGS_PRESENT_ \
    -I"$SYSROOT/usr/include" -L"$SYSROOT/usr/lib" \
    -o "$DESTDIR/usr/bin/cpdup" "$SRC/cpdup/src/"*.c -lcrypto
test -x "$DESTDIR/usr/bin/cpdup" || { echo "FAIL: /usr/bin/cpdup not installed"; exit 1; }
# cpdup.1: the hand-link bypasses cpdup's GNUmakefile (which normally installs
# the page), so install + gzip it here, mirroring that Makefile's man rule.
mkdir -p "$DESTDIR/usr/share/man/man1"
install -m 0644 "$SRC/cpdup/cpdup.1" "$DESTDIR/usr/share/man/man1/cpdup.1"
gzip -9f "$DESTDIR/usr/share/man/man1/cpdup.1"

# nextbsd-installer: CMake C++ — the FIRST C++ program in the pipeline. Its
# CMakeLists add_subdirectory()s the vendored ../ftxui and links it statically,
# so ftxui needs no stanza of its own (and its examples/docs/tests are forced
# OFF). The C++ runtime (libc++/libcxxrt/libcompiler_rt/libthr) comes from the
# compat base. Install rules place the binary in usr/sbin and the shell engine
# (do-install.sh + probe-disks.sh) in usr/libexec/nextbsd-installer, matching the
# baked NBI_LIBEXEC_DEFAULT. THREADS_PREFER_PTHREAD_FLAG avoids a cross FindThreads
# miss on FreeBSD's libthr.
comp "nextbsd-installer [cmake cross, C++ + vendored ftxui]"
NBI_BUILD="$ROOT/.build/nextbsd-installer-$T"
rm -rf "$NBI_BUILD"; mkdir -p "$NBI_BUILD"
cmake -G Ninja -S "$SRC/nextbsd-installer" -B "$NBI_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
    -DTHREADS_PREFER_PTHREAD_FLAG=ON \
    -DCMAKE_INSTALL_PREFIX=/ \
    -DCMAKE_BUILD_TYPE=Release
DESTDIR="$DESTDIR" ninja -C "$NBI_BUILD" install
test -x "$DESTDIR/usr/sbin/nextbsd-installer" || { echo "FAIL: /usr/sbin/nextbsd-installer not installed"; exit 1; }
test -f "$DESTDIR/usr/libexec/nextbsd-installer/do-install.sh" || { echo "FAIL: installer engine (do-install.sh) not installed"; exit 1; }

# ---- tzsetup (nextbsd/nextbsd-userland#302, E18 U18) -----------------------
# Replaces FreeBSD's bsddialog tzsetup at the same path, in the installer's
# look: same vendored FTXUI, same amber theme through src/libnbui (#304). The
# base copy is stripped by nextbsd-freebsd-compat's scripts/collisions, so
# only one /usr/sbin/tzsetup ever reaches the image.
comp "tzsetup [cmake cross, C++ + vendored ftxui]"
TZS_BUILD="$ROOT/.build/tzsetup-$T"
rm -rf "$TZS_BUILD"; mkdir -p "$TZS_BUILD"
cmake -G Ninja -S "$SRC/tzsetup" -B "$TZS_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
    -DTHREADS_PREFER_PTHREAD_FLAG=ON \
    -DCMAKE_INSTALL_PREFIX=/ \
    -DCMAKE_BUILD_TYPE=Release
DESTDIR="$DESTDIR" ninja -C "$TZS_BUILD" install
test -x "$DESTDIR/usr/sbin/tzsetup" || { echo "FAIL: /usr/sbin/tzsetup not installed"; exit 1; }
echo "==> tzsetup built"

# ---- libds (nextbsd/nextbsd-userland#287, E18 U15) -------------------------
# Reads and writes the DirectoryServices plists through CoreFoundation's own
# property list implementation, so nothing here serialises XML by hand. A
# static internal library: the account tools and the ds* commands link it in,
# and nothing is installed, so there is no ABI to keep.
#
# NOT used by nss_directory_services, which links libc alone because it is
# dlopened into every process that calls getpwnam(3) and keeps its own
# read-only parser. Built here purely so a break shows up on both arches.
#
# The unit tests also build for the image (TIER 3, LIBDS marker) and run on
# the boot lane, because the Ubuntu runners have no CoreFoundation. The same
# sources run on a developer's machine with `cd tests && make check`.
comp "libds"
run_buildenv "make -C $SRC/libds SYSROOT=$SYSROOT all"
test ! -e "$DESTDIR/usr/lib/libds.a" || { echo "FAIL: libds is internal and must not be installed"; exit 1; }
echo "==> libds built"

# ---- createhomedir (E18 U10) ----------------------------------------------
# Creates a user's home at 0755 and nothing inside it. Plain libc: users are
# enumerated through getpwent(3), so it needs no directory library, and with
# the user template gone it no longer needs the XML plist reader either.
comp "createhomedir"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/accounts/createhomedir DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/createhomedir" || { echo "FAIL: /usr/sbin/createhomedir not installed"; exit 1; }
echo "==> createhomedir built"

# ---- adduser (nextbsd/nextbsd-userland#286, E18 U14) -----------------------
# NextBSD's own adduser at FreeBSD's path: creates the account in the
# DirectoryServices plists through libds, then builds the home by running
# createhomedir. Links CoreFoundation (via libds) and libutil, the latter
# for login.conf's passwd_format, so the hashing policy is not compiled in.
comp "adduser"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/accounts/adduser DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/adduser" || { echo "FAIL: /usr/sbin/adduser not installed"; exit 1; }
echo "==> adduser built"

# ---- passwd (nextbsd/nextbsd-userland#255, E18 U9) -------------------------
# Replaces the one from base, which cannot be wrapped: it switches on the
# source an account was resolved from and refuses anything that is not files
# or NIS, so every directory account is rejected before our code would run.
# Setuid root, because the plists are 0644 root-owned and there is no
# opendirectoryd here to hand the write to. ci/assemble-image.sh re-applies
# the bit after its chown, which strips it.
comp "passwd"
mkdir -p "$DESTDIR/usr/bin"
run_buildenv "make -C $SRC/accounts/passwd DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/bin/passwd" || { echo "FAIL: /usr/bin/passwd not installed"; exit 1; }
echo "==> passwd built"

# ---- rmuser (nextbsd/nextbsd-userland#255, E18 U9) -------------------------
# The counterpart to adduser: removes a regular account from the plists, its
# group memberships, its crontab, its at jobs and its home. Not setuid; this
# is root's work and there is no self-service case.
comp "rmuser"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/accounts/rmuser DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/rmuser" || { echo "FAIL: /usr/sbin/rmuser not installed"; exit 1; }
echo "==> rmuser built"

# ---- chpass, chfn, chsh (nextbsd/nextbsd-userland#255, E18 U9) -------------
# One binary behind six names. Unlike other BSDs the names are not synonyms:
# chfn edits the real name, chsh the shell, and neither touches the other. The
# yp* links exist only to say NIS is unsupported. Setuid root for the same
# reason passwd is; ci/assemble-image.sh re-applies the bit.
comp "chpass"
mkdir -p "$DESTDIR/usr/bin"
run_buildenv "make -C $SRC/accounts/chpass DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/bin/chpass" || { echo "FAIL: /usr/bin/chpass not installed"; exit 1; }
for l in chfn chsh ypchpass ypchfn ypchsh; do
    test -e "$DESTDIR/usr/bin/$l" || { echo "FAIL: /usr/bin/$l link not installed"; exit 1; }
done
echo "==> chpass built"

# ---- pw (nextbsd/nextbsd-userland#255, E18 U9) -----------------------------
# Routes: an existing account is served wherever it lives, a new one by its
# shape, and anything belonging in master.passwd is handed to the base copy at
# the pw vendored under src/accounts/pw/bsd with argv untouched. That is what keeps the
# contract with ports, whose install scripts call pw with a positional name.
#
# The relocation of the base binary is a nextbsd-freebsd-compat change and
# must not land before this: until it does, pw here reports that system
# accounts cannot be managed rather than doing the wrong thing silently.
comp "pw"
mkdir -p "$DESTDIR/usr/sbin"
run_buildenv "make -C $SRC/accounts/pw DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/pw" || { echo "FAIL: /usr/sbin/pw not installed"; exit 1; }
echo "==> pw built"

# ---- directory commands (nextbsd/nextbsd-userland#253, E18 U7) --------------
# dspromote/dsdemote/dsjoin/dsleave/dsstatus: one binary behind five names,
# moving a machine between standalone, directory server and client. Plain
# libc -- it writes small plists by hand rather than parsing or copying any, so it links
# neither libds nor CoreFoundation.
comp "directory commands"
run_buildenv "make -C $SRC/directory DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/sbin/dspromote" || { echo "FAIL: /usr/sbin/dspromote not installed"; exit 1; }
for l in dsdemote dsjoin dsleave dsstatus; do
    test -L "$DESTDIR/usr/sbin/$l" || { echo "FAIL: /usr/sbin/$l link not installed"; exit 1; }
done
echo "==> directory commands built"

# ---- autologin-user (nextbsd/nextbsd-userland#278, E18 U11) ----------------
# Names the account the console logs in automatically, or prints nothing. The
# getty job runs it; the rule is in one place rather than in a shell one-liner.
comp "autologin-user"
mkdir -p "$DESTDIR/usr/libexec/nextbsd"
run_buildenv "make -C $SRC/accounts/autologin-user DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/libexec/nextbsd/autologin-user" || { echo "FAIL: /usr/libexec/nextbsd/autologin-user not installed"; exit 1; }
echo "==> autologin-user built"

# ---- nextbsd-fetch (nextbsd/nextbsd-userland#298, E18 U16) -----------------
# The login banner /etc/zprofile runs. Plain libc: it reads /etc/os-release
# and sysctl directly, so it needs no pkg dependency the way fastfetch would,
# and neofetch is an archived shell script that could not do it at all.
comp "nextbsd-fetch"
run_buildenv "make -C $SRC/banner DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/bin/nextbsd-fetch" || { echo "FAIL: /usr/bin/nextbsd-fetch not installed"; exit 1; }
echo "==> nextbsd-fetch built"

# ---- vm_stat (nextbsd/nextbsd-userland#180, E14 U3) ------------------------
# Darwin-format VM statistics from FreeBSD's vm.stats.* sysctls (no Mach VM).
# pagesize(1) itself is FreeBSD's, which already reads hw.pagesize.
comp "vm_stat(1)"
run_buildenv "make -C $SRC/vm_stat DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -x "$DESTDIR/usr/bin/vm_stat" || { echo "FAIL: /usr/bin/vm_stat not installed"; exit 1; }
echo "==> vm_stat built"

# ---- nss_directory_services (nextbsd/nextbsd-userland#249, E18 U3) ---------
# The `directory_services` source for passwd and group: users and groups
# straight from /Local (or /Network) Library/DirectoryServices/{Users,Groups}
# .plist, no daemon. Plain libc, bsd.lib.mk, one shared object at the name
# nsdispatch(3) dlopens. nsswitch.conf itself comes from nextbsd-overlays.
comp "nss_directory_services"
mkdir -p "$DESTDIR/usr/lib"
run_buildenv "make -C $SRC/nss_directory_services DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -f "$DESTDIR/usr/lib/nss_directory_services.so.1" || { echo "FAIL: /usr/lib/nss_directory_services.so.1 not installed"; exit 1; }
test ! -e "$DESTDIR/usr/lib/libnss_directory_services.a" || { echo "FAIL: nss_directory_services must not install a static library"; exit 1; }
echo "==> nss_directory_services built"

# ---- nss_mdns (nextbsd/nextbsd-userland#461, #435) -------------------------
# The `mdns` source for hosts: names under local. answered by mDNSResponder
# through libdns_sd, the way Libinfo's mdns module does on macOS. One shared
# object at the name nsdispatch(3) dlopens; links libc and libdns_sd only.
# nsswitch.conf's `hosts: files mdns dns` comes from nextbsd-overlays.
comp "nss_mdns"
mkdir -p "$DESTDIR/usr/lib"
run_buildenv "make -C $SRC/nss_mdns DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -f "$DESTDIR/usr/lib/nss_mdns.so.1" || { echo "FAIL: /usr/lib/nss_mdns.so.1 not installed"; exit 1; }
test ! -e "$DESTDIR/usr/lib/libnss_mdns.a" || { echo "FAIL: nss_mdns must not install a static library"; exit 1; }
needed_check "$DESTDIR/usr/lib/nss_mdns.so.1" "libdns_sd.so.1"
echo "==> nss_mdns built"

# ---- pam_launchd (nextbsd/nextbsd-userland#354, PR 3) ----------------------
# Apple's launchd PAM session module (pam_modules-126), built for the OpenPAM
# inherited from FreeBSD: OpenPAM headers and libpam come from the compat base
# in the sysroot, liblaunch and libsystem_kernel from the steps above. It moves
# a login session onto the user's per-user launchd bootstrap; every failure is
# PAM_IGNORE. Installed as /usr/lib/pam_launchd.so.6 (+ the pam_launchd.so
# link), the name every base module uses. NOTHING references it yet: the
# pam.d policy files live in nextbsd-overlays and add it later.
#
# Its probe is a client stub for launchd's get_root_bootstrap and look_up2
# (liveness, then forwarding to PID 1), each with a MIG WaitTime, generated
# here by the host mig from pam_launchd_probe.defs.
comp "pam_launchd"
PAM_LAUNCHD_MIG="$MIGROOT/pam_launchd"
rm -rf "$PAM_LAUNCHD_MIG"; mkdir -p "$PAM_LAUNCHD_MIG"
run_mig "$PAM_LAUNCHD_MIG" -I"$SRC/libmach/include" \
    -header pam_launchd_probe.h -user pam_launchd_probeUser.c \
    -server /dev/null \
    "$SRC/pam_launchd/pam_launchd_probe.defs" \
    || { echo "FAIL: mig could not process pam_launchd_probe.defs"; exit 1; }
test -s "$PAM_LAUNCHD_MIG/pam_launchd_probeUser.c" || { echo "FAIL: mig produced no pam_launchd_probeUser.c"; exit 1; }
grep -q "MACH_RCV_TIMEOUT" "$PAM_LAUNCHD_MIG/pam_launchd_probeUser.c" \
    || { echo "FAIL: pam_launchd probe stub has no receive timeout"; exit 1; }
mkdir -p "$DESTDIR/usr/lib"
run_buildenv "make -C $SRC/pam_launchd DESTDIR=$DESTDIR SYSROOT=$SYSROOT MIGOUT=$PAM_LAUNCHD_MIG all install"
test -f "$DESTDIR/usr/lib/pam_launchd.so.6" || { echo "FAIL: /usr/lib/pam_launchd.so.6 not installed"; exit 1; }
test ! -e "$DESTDIR/usr/lib/libpam_launchd.a" || { echo "FAIL: pam_launchd must not install a static library"; exit 1; }
needed_check "$DESTDIR/usr/lib/pam_launchd.so.6" "liblaunch"
needed_check "$DESTDIR/usr/lib/pam_launchd.so.6" "libpam"
needed_check "$DESTDIR/usr/lib/pam_launchd.so.6" "libsystem_dispatch"
echo "==> pam_launchd built"

# ---- libffi (nextbsd/nextbsd-userland#443, E24 D4) -------------------------
# Apple's libffi-40 (libffi 3.4.0-rc2, MIT) for gnustep-base. Installed in
# /usr like a base library: /usr/lib/libffi.so.7 (+ libffi.so, libffi.a),
# <ffi.h>/<ffitarget.h>, /usr/libdata/pkgconfig/libffi.pc and the ffi(3)
# pages. src/libffi stays exactly as Apple ships it; the NextBSD patch
# series (upstream fixes since 3.4.0, see src/libffi/VENDORED.md) is applied
# to a copy under .build, and that copy is what bsd.lib.mk builds. Plain
# libc, nothing else in this file depends on it.
comp "libffi (Apple libffi-40 + patches/)"
FFI_BUILD="$ROOT/.build/libffi-$T"
rm -rf "$FFI_BUILD"; mkdir -p "$ROOT/.build"
cp -R "$SRC/libffi" "$FFI_BUILD"
for p in "$SRC/libffi/patches/"*.patch; do
    note "patch: $(basename "$p")"
    if command -v patch >/dev/null 2>&1; then
        patch -d "$FFI_BUILD" -p1 -N -s --no-backup-if-mismatch < "$p" \
            || { echo "FAIL: libffi patch $(basename "$p") did not apply"; exit 1; }
    else
        # Ceiling: the copy sits inside this checkout, and git apply run
        # inside a repository would resolve the paths from its top.
        ( cd "$FFI_BUILD" && GIT_CEILING_DIRECTORIES="$ROOT/.build" git apply -p1 "$p" ) \
            || { echo "FAIL: libffi patch $(basename "$p") did not apply"; exit 1; }
    fi
done
mkdir -p "$DESTDIR/usr/lib" "$DESTDIR/usr/include" \
         "$DESTDIR/usr/libdata/pkgconfig" "$DESTDIR/usr/share/man/man3"
run_buildenv "make -C $FFI_BUILD DESTDIR=$DESTDIR SYSROOT=$SYSROOT all install"
test -f "$DESTDIR/usr/lib/libffi.so.7" || { echo "FAIL: /usr/lib/libffi.so.7 not installed"; exit 1; }
test -L "$DESTDIR/usr/lib/libffi.so"   || { echo "FAIL: /usr/lib/libffi.so link not installed"; exit 1; }
test -f "$DESTDIR/usr/include/ffi.h"   || { echo "FAIL: /usr/include/ffi.h not installed"; exit 1; }
test -f "$DESTDIR/usr/include/ffitarget.h" || { echo "FAIL: /usr/include/ffitarget.h not installed"; exit 1; }
grep -q '^Version: 3.4.0-rc2$' "$DESTDIR/usr/libdata/pkgconfig/libffi.pc" \
    || { echo "FAIL: libffi.pc missing or wrong"; exit 1; }
FFI_DYN="$("$READELF" -d "$DESTDIR/usr/lib/libffi.so.7" 2>&1)" \
    || { echo "FAIL: $READELF -d libffi.so.7: $FFI_DYN"; exit 1; }
printf '%s\n' "$FFI_DYN" | grep -q 'SONAME.*libffi\.so\.7' \
    || { echo "FAIL: libffi.so.7 has the wrong SONAME"; printf '%s\n' "$FFI_DYN" | grep -i soname; exit 1; }
sync_sysroot
mkdir -p "$SYSROOT/usr/lib"
cp -a "$DESTDIR/usr/lib/libffi."* "$SYSROOT/usr/lib/"
echo "==> libffi built"

# ---- libxml2 + libxslt (Apple's, nextbsd/nextbsd-userland#442, E24 D4) ------
# Apple's libxml2-39.10 (2.9.13) and libxslt-21.13 (1.1.35), as macOS ships
# them in /usr/lib. Apple's trees carry no generated configure, only
# configure.ac and the CMakeLists.txt upstream added in 2.9.11/1.1.35, so both
# are host cmake/ninja runs like ICU. src/<project> is Apple's tree as pinned
# in its VENDORED.md; src/<project>/patches is applied, in order, to a copy
# under .build so the vendored tree stays byte-for-byte Apple's.
#
# Feature set follows Apple's own build: zlib, no lzma, XPointer locations
# compiled in and disabled at run time, and ICU (not iconv) as the encoding
# converter, linked against base ICU, libicucore, as macOS links it (#453).
# The ICU section above installed it into the sysroot (sync_sysroot), with
# the <unicode/*.h> headers under /usr/include/icucore. CMake's FindICU only
# knows the libicuuc/libicui18n/libicudata names, which base does not have,
# so its cache variables point all three components at libicucore; patch
# 0004 makes libxml-2.0.pc say `Requires.private: icu-uc` instead of those
# names. The commands are the ones macOS ships (xmllint, xmlcatalog,
# xsltproc); Python bindings, tests and the HTML docs are not built or
# shipped. Sonames are upstream's (libxml2.so.2, libxslt.so.1, libexslt.so.0)
# and the .pc files go where base keeps them, /usr/libdata/pkgconfig.
apply_vendor_patches() {
    local src="$1" dst="$2" p
    rm -rf "$dst"; mkdir -p "$(dirname "$dst")"; cp -a "$src" "$dst"
    for p in "$src"/patches/*.patch; do
        [ -f "$p" ] || continue
        note "patch: $(basename "$src")/$(basename "$p")"
        patch -d "$dst" -p1 -s < "$p" || { echo "FAIL: $p does not apply"; exit 1; }
    done
}

comp "libxml2 [cmake cross, Apple libxml2-39.10]"
XML2_SRC="$ROOT/.build/libxml2-src-$T"
XML2_BUILD="$ROOT/.build/libxml2-$T"
apply_vendor_patches "$SRC/libxml2" "$XML2_SRC"
rm -rf "$XML2_BUILD"; mkdir -p "$XML2_BUILD"
cmake -G Ninja -S "$XML2_SRC" -B "$XML2_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
    -DTHREADS_PREFER_PTHREAD_FLAG=ON \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_INCLUDEDIR=include \
    -DCMAKE_INSTALL_MANDIR=share/man \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBXML2_WITH_PYTHON=OFF \
    -DLIBXML2_WITH_TESTS=OFF \
    -DLIBXML2_WITH_ICU=ON \
    -DLIBXML2_WITH_ICONV=OFF \
    -DICU_INCLUDE_DIR="$SYSROOT/usr/include/icucore" \
    -DICU_UC_LIBRARY_RELEASE="$SYSROOT/usr/lib/system/libicucore.so" \
    -DICU_I18N_LIBRARY_RELEASE="$SYSROOT/usr/lib/system/libicucore.so" \
    -DICU_DATA_LIBRARY_RELEASE="$SYSROOT/usr/lib/system/libicucore.so" \
    -DLIBXML2_WITH_LZMA=OFF \
    -DLIBXML2_WITH_ZLIB=ON \
    -DLIBXML2_WITH_XPTR_LOCS=ON
DESTDIR="$DESTDIR" ninja -C "$XML2_BUILD" install
mkdir -p "$DESTDIR/usr/libdata/pkgconfig"
mv "$DESTDIR/usr/lib/pkgconfig/libxml-2.0.pc" "$DESTDIR/usr/libdata/pkgconfig/"
rmdir "$DESTDIR/usr/lib/pkgconfig" 2>/dev/null || true
rm -rf "$DESTDIR/usr/share/doc/libxml2"
rm -f "$DESTDIR/usr/lib/xml2Conf.sh"
gzip -9f "$DESTDIR/usr/share/man/man1/xmllint.1" "$DESTDIR/usr/share/man/man1/xmlcatalog.1" \
         "$DESTDIR/usr/share/man/man3/libxml.3"
test -L "$DESTDIR/usr/lib/libxml2.so.2" || { echo "FAIL: /usr/lib/libxml2.so.2 missing"; exit 1; }
test -x "$DESTDIR/usr/bin/xmllint"     || { echo "FAIL: /usr/bin/xmllint missing"; exit 1; }
test -x "$DESTDIR/usr/bin/xmlcatalog"  || { echo "FAIL: /usr/bin/xmlcatalog missing"; exit 1; }
test -f "$DESTDIR/usr/include/libxml2/libxml/xmlversion.h" \
    || { echo "FAIL: libxml2 headers missing"; exit 1; }
needed_check "$DESTDIR/usr/lib/libxml2.so.2" "libz"
# libicucore's soname is the CMake target's, lib_FoundationICU.so.
needed_check "$DESTDIR/usr/lib/libxml2.so.2" "lib_FoundationICU"
needed_check "$DESTDIR/usr/bin/xmllint" "libxml2.so.2"
# libxslt's find_package(LibXml2 CONFIG) only searches the sysroot (the
# toolchain file's FIND_ROOT_PATH), so stage libxml2 there as sync_sysroot
# does for /usr/lib/system. The exported targets name xmllint and xmlcatalog
# too, and CMake checks every imported file exists, so those come along.
if [ "$SYSROOT" != "$DESTDIR" ]; then
    mkdir -p "$SYSROOT/usr/lib/cmake" "$SYSROOT/usr/include" "$SYSROOT/usr/libdata/pkgconfig" \
             "$SYSROOT/usr/bin"
    cp -a "$DESTDIR/usr/bin/xmllint" "$DESTDIR/usr/bin/xmlcatalog" "$SYSROOT/usr/bin/"
    cp -a "$DESTDIR/usr/lib/"libxml2.so* "$SYSROOT/usr/lib/"
    cp -a "$DESTDIR/usr/lib/cmake/"libxml2-* "$SYSROOT/usr/lib/cmake/"
    cp -a "$DESTDIR/usr/include/libxml2" "$SYSROOT/usr/include/"
    cp -a "$DESTDIR/usr/libdata/pkgconfig/libxml-2.0.pc" \
          "$DESTDIR/usr/libdata/pkgconfig/"icu-*.pc "$SYSROOT/usr/libdata/pkgconfig/"
fi

comp "libxslt + libexslt [cmake cross, Apple libxslt-21.13]"
XSLT_SRC="$ROOT/.build/libxslt-src-$T"
XSLT_BUILD="$ROOT/.build/libxslt-$T"
apply_vendor_patches "$SRC/libxslt" "$XSLT_SRC"
rm -rf "$XSLT_BUILD"; mkdir -p "$XSLT_BUILD"
# LIBXSLT_DEFAULT_PLUGINS_PATH: 1.1.35's CMakeLists sets it only after
# xsltconfig.h is generated, which would leave the header's path empty.
cmake -G Ninja -S "$XSLT_SRC" -B "$XSLT_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
    -DTHREADS_PREFER_PTHREAD_FLAG=ON \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_INCLUDEDIR=include \
    -DCMAKE_INSTALL_MANDIR=share/man \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBXSLT_WITH_PYTHON=OFF \
    -DLIBXSLT_WITH_TESTS=OFF \
    -DLIBXSLT_WITH_CRYPTO=OFF \
    -DLIBXSLT_DEFAULT_PLUGINS_PATH=/usr/lib/libxslt-plugins
DESTDIR="$DESTDIR" ninja -C "$XSLT_BUILD" install
mv "$DESTDIR/usr/lib/pkgconfig/libxslt.pc" "$DESTDIR/usr/lib/pkgconfig/libexslt.pc" \
   "$DESTDIR/usr/libdata/pkgconfig/"
rmdir "$DESTDIR/usr/lib/pkgconfig" 2>/dev/null || true
rm -rf "$DESTDIR/usr/share/doc/libxslt"
rm -f "$DESTDIR/usr/lib/xsltConf.sh"
gzip -9f "$DESTDIR/usr/share/man/man1/xsltproc.1" \
         "$DESTDIR/usr/share/man/man3/libxslt.3" "$DESTDIR/usr/share/man/man3/libexslt.3"
test -L "$DESTDIR/usr/lib/libxslt.so.1"  || { echo "FAIL: /usr/lib/libxslt.so.1 missing"; exit 1; }
test -L "$DESTDIR/usr/lib/libexslt.so.0" || { echo "FAIL: /usr/lib/libexslt.so.0 missing"; exit 1; }
test -x "$DESTDIR/usr/bin/xsltproc"      || { echo "FAIL: /usr/bin/xsltproc missing"; exit 1; }
needed_check "$DESTDIR/usr/lib/libxslt.so.1"  "libxml2.so.2"
needed_check "$DESTDIR/usr/lib/libexslt.so.0" "libxslt.so.1"
needed_check "$DESTDIR/usr/bin/xsltproc"      "libexslt.so.0"
# CMake writes prefix=${pcfiledir}/../.., which pkgconf would prefix again
# with PKG_CONFIG_SYSROOT_DIR when cross-building against /stage. Pin it to
# /usr like base's own .pc files.
for pc in libxml-2.0 libxslt libexslt; do
    sed -i.orig 's|^prefix=.*|prefix=/usr|' "$DESTDIR/usr/libdata/pkgconfig/$pc.pc"
    rm -f "$DESTDIR/usr/libdata/pkgconfig/$pc.pc.orig"
done
echo "==> libxml2 + libxslt built"

# ---- curl + libcurl (nextbsd/nextbsd-userland#444, E24 D4) -----------------
# Apple's curl (apple-oss-distributions/curl curl-166.0.0.0.1 = curl 8.7.1),
# vendored verbatim in src/curl/dist; NextBSD's security backports are the
# series in src/curl/patches, applied to a copy here. See src/curl/VENDORED.md.
#
# Apple's own TLS (Secure Transport) does not exist on NextBSD, so the TLS
# backend is base OpenSSL; zlib and GSS-API (MIT Kerberos) also come from the
# base sysroot. No nghttp2, libpsl, libidn2, libssh2, brotli or zstd: none is
# in base. LDAP is off (base has no public OpenLDAP). The CA bundle is
# /etc/ssl/cert.pem, Apple's and OpenSSL's path; the image assembly writes it
# from base /usr/share/certs (certctl's bundle).
#
# Installs /usr/bin/curl, /usr/bin/curl-config, /usr/lib/libcurl.so.4,
# /usr/include/curl, /usr/libdata/pkgconfig/libcurl.pc (base's .pc location)
# and the man pages. Plain host cmake/ninja with the cross toolchain file,
# like the other CMake components; nothing here links nextbsd-userland libs.
comp "curl + libcurl [cmake cross, Apple curl-166.0.0.0.1 + patches]"
CURL_SRC="$ROOT/.build/curl-src-$T"
CURL_BUILD="$ROOT/.build/curl-$T"
rm -rf "$CURL_SRC" "$CURL_BUILD"; mkdir -p "$CURL_SRC" "$CURL_BUILD"
cp -a "$SRC/curl/dist/." "$CURL_SRC/"
while IFS= read -r p; do
    case "$p" in ''|'#'*) continue ;; esac
    note "patch $p"
    if command -v git >/dev/null 2>&1; then
        # The ceiling keeps git from treating the nextbsd-userland checkout
        # as the repository the patch is relative to.
        ( cd "$CURL_SRC" && GIT_CEILING_DIRECTORIES="$ROOT/.build" \
            git apply -p1 "$SRC/curl/patches/$p" )
    else
        ( cd "$CURL_SRC" && patch -p1 --forward --silent < "$SRC/curl/patches/$p" )
    fi || { echo "FAIL: curl patch $p does not apply"; exit 1; }
done < "$SRC/curl/patches/series"
# pkg-config is pointed at nothing so the RUNNER's .pc files cannot leak in;
# OpenSSL, zlib and GSS-API are found in the sysroot by the toolchain file's
# CMAKE_FIND_ROOT_PATH. HAVE_POLL_FINE / HAVE_WRITABLE_ARGV are run-time
# checks CMake skips when cross compiling; both hold on NextBSD.
PKG_CONFIG_LIBDIR=/nonexistent PKG_CONFIG_PATH= \
cmake -G Ninja -S "$CURL_SRC" -B "$CURL_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DTHREADS_PREFER_PTHREAD_FLAG=ON \
    -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_CURL_EXE=ON \
    -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR=/usr \
    -DCURL_ZLIB=ON \
    -DCURL_USE_GSSAPI=ON -DGSS_ROOT_DIR=/usr \
    -DENABLE_THREADED_RESOLVER=ON -DENABLE_IPV6=ON \
    -DCURL_CA_BUNDLE=/etc/ssl/cert.pem -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF \
    -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
    -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF -DUSE_QUICHE=OFF -DUSE_MSH3=OFF \
    -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF \
    -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DENABLE_WEBSOCKETS=OFF \
    -DHAVE_POLL_FINE=1 -DHAVE_WRITABLE_ARGV=1
grep -q '^#define USE_OPENSSL 1' "$CURL_BUILD/lib/curl_config.h" \
    || { echo "FAIL: curl not configured with OpenSSL"; exit 1; }
grep -q '^#define HAVE_GSSAPI 1' "$CURL_BUILD/lib/curl_config.h" \
    || { echo "FAIL: curl not configured with GSS-API"; exit 1; }
grep -q '^#define HAVE_LIBZ 1' "$CURL_BUILD/lib/curl_config.h" \
    || { echo "FAIL: curl not configured with zlib"; exit 1; }
DESTDIR="$DESTDIR" ninja -C "$CURL_BUILD" install
# FreeBSD base keeps its .pc files in /usr/libdata/pkgconfig (openssl.pc,
# zlib.pc, mit-krb5-gssapi.pc), so libcurl.pc goes there too.
mkdir -p "$DESTDIR/usr/libdata/pkgconfig"
mv "$DESTDIR/usr/lib/pkgconfig/libcurl.pc" "$DESTDIR/usr/libdata/pkgconfig/libcurl.pc"
rmdir "$DESTDIR/usr/lib/pkgconfig" 2>/dev/null || true
# The generated libcurl.pc, curl-config and CMake package files record the
# build: sysroot library paths, the cross compiler, its --target/--sysroot/
# --ld-path flags and the compiler's implicit -lgcc/-lc. On the installed
# system the dependencies are plain /usr/lib libraries and the compiler is cc.
for f in "$DESTDIR/usr/libdata/pkgconfig/libcurl.pc" "$DESTDIR/usr/bin/curl-config" \
         "$DESTDIR"/usr/lib/cmake/CURL/*.cmake; do
    [ -f "$f" ] || continue
    sed -i -E \
        -e "s#$SYSROOT/usr/lib/lib([A-Za-z0-9_]+)\.so#-l\1#g" \
        -e "s#\"$CROSS_BINDIR/clang\"#\"cc\"#g" \
        -e 's# --(target|sysroot|ld-path)=[^ ]*##g' \
        -e "s#$SYSROOT##g" \
        -e ':strip' -e 's# -l(gcc_s|gcc|c)( |$)#\2#' -e 't strip' \
        "$f"
done
if grep -rlE "$SYSROOT|$CROSS_BINDIR/(clang|ld)" "$DESTDIR/usr/libdata/pkgconfig/libcurl.pc" \
       "$DESTDIR/usr/bin/curl-config" "$DESTDIR/usr/lib/cmake/CURL" 2>/dev/null; then
    echo "FAIL: curl's installed metadata still names the build host"; exit 1
fi
# Pre-built man pages from Apple's tree (the CMake doc build would need Perl),
# gzipped like base's.
CURL_MAN="$CURL_BUILD/man"
mkdir -p "$CURL_MAN/man1" "$CURL_MAN/man3"
cp "$CURL_SRC/docs/curl.1" "$CURL_SRC/docs/curl-config.1" "$CURL_MAN/man1/"
find "$CURL_SRC/docs/libcurl" -name '*.3' -exec cp {} "$CURL_MAN/man3/" \;
gzip -9n "$CURL_MAN"/man1/*.1 "$CURL_MAN"/man3/*.3
mkdir -p "$DESTDIR/usr/share/man/man1" "$DESTDIR/usr/share/man/man3"
install -m 0444 "$CURL_MAN"/man1/*.gz "$DESTDIR/usr/share/man/man1/"
install -m 0444 "$CURL_MAN"/man3/*.gz "$DESTDIR/usr/share/man/man3/"
test -x "$DESTDIR/usr/bin/curl" || { echo "FAIL: /usr/bin/curl not installed"; exit 1; }
test -f "$DESTDIR/usr/lib/libcurl.so.4" || { echo "FAIL: /usr/lib/libcurl.so.4 not installed"; exit 1; }
test -f "$DESTDIR/usr/include/curl/curl.h" || { echo "FAIL: <curl/curl.h> not installed"; exit 1; }
needed_check "$DESTDIR/usr/bin/curl" "libcurl.so.4"
needed_check "$DESTDIR/usr/lib/libcurl.so.4" "libssl.so"
needed_check "$DESTDIR/usr/lib/libcurl.so.4" "libcrypto.so"
needed_check "$DESTDIR/usr/lib/libcurl.so.4" "libz.so"
needed_check "$DESTDIR/usr/lib/libcurl.so.4" "libgssapi_krb5.so"
echo "==> curl built"

# =============================================================================
# TIER 3 — on-image test binaries (freebsd-launchd-mach suite).
# build.sh builds these natively and installs them to
# /usr/tests/freebsd-launchd-mach/; CI's run.sh RUNS them post-login (they are
# on-image tests, NOT build-time host-exec tests — so they cross-build safely:
# we install, never exec here). Single-file links against the Tier-1 Darwin
# libs. --sysroot gives base headers/libs, -I$DESTDIR the Mach headers,
# -L$DESTDIR/usr/lib/system libsystem_kernel. Mirrors build.sh ~800-947.
# =============================================================================
tier "TIER 3 : on-image test binaries (freebsd-launchd-mach suite)"
TESTDIR="$DESTDIR/usr/tests/freebsd-launchd-mach"
mkdir -p "$TESTDIR" "$DESTDIR/usr/include/servers" "$DESTDIR/usr/sbin" "$DESTDIR/usr/bin"

# Target mig + migcom for the IMAGE (/usr/bin/mig + /usr/libexec/migcom). The
# Tier-0 migcom is a runner-native (Linux) codegen tool, NOT shippable; build a
# TARGET migcom via the cross buildenv so `mig -version` runs on the booted
# image (the MIG-BUILD marker). mig.sh is an arch-neutral wrapper. (build.sh ~977)
comp "target mig + migcom (on-image MIG-BUILD)"
run_buildenv "make -C $SRC/bootstrap_cmds/migcom.tproj DESTDIR=$DESTDIR BINDIR=/usr/libexec MK_MAN=no all install"
install -m755 "$SRC/bootstrap_cmds/migcom.tproj/mig.sh" "$DESTDIR/usr/bin/mig"
test -x "$DESTDIR/usr/libexec/migcom" || { echo "FAIL: target /usr/libexec/migcom not installed"; exit 1; }
# <servers/bootstrap.h> by hand (libmach ships the mach/ headers; this one sits
# at /usr/include/servers/ per Apple convention).
cp "$SRC/libmach/include/servers/bootstrap.h" "$DESTDIR/usr/include/servers/bootstrap.h"
TCC="$CROSS_CC --sysroot=$SYSROOT -I$DESTDIR/usr/include -L$DESTDIR/usr/lib/system -L$SYSROOT/usr/lib -Wl,-rpath,/usr/lib/system"

comp "mach_kmod tests (libsystem_kernel roundtrips)"
for t in test_libmach test_mach_port test_evfilt_machport test_task_special_port test_host_bootstrap test_mach_name_recycle; do
    $TCC -o "$TESTDIR/$t" "$SRC/mach_kmod/tests/$t.c" -lsystem_kernel
done
$TCC -o "$TESTDIR/test_evfilt_machport_concurrent" "$SRC/mach_kmod/tests/test_evfilt_machport_concurrent.c" -lsystem_kernel -lpthread
# test_busystate: libc-only (sysctlbyname + syscall), no -lsystem_kernel.
$TCC -o "$TESTDIR/test_busystate" "$SRC/mach_kmod/tests/test_busystate.c"
test -x "$TESTDIR/test_libmach" || { echo "FAIL: test_libmach not built"; exit 1; }

comp "bootstrap suite (libbootstrap static + pthread)"
$TCC -I"$SRC/bootstrap" -o "$TESTDIR/test_bootstrap" \
    "$SRC/bootstrap/tests/test_bootstrap.c" "$SRC/bootstrap/libbootstrap.c" -lsystem_kernel -lpthread
$TCC -I"$SRC/bootstrap" -o "$DESTDIR/usr/sbin/bootstrap_server" \
    "$SRC/bootstrap/bootstrap_server.c" "$SRC/bootstrap/libbootstrap.c" -lsystem_kernel -lpthread
$TCC -I"$SRC/bootstrap" -o "$TESTDIR/test_bootstrap_remote" \
    "$SRC/bootstrap/tests/test_bootstrap_remote.c" "$SRC/bootstrap/libbootstrap.c" -lsystem_kernel -lpthread

# ---- remaining on-image test suites (all install into the same TESTDIR). ----
# Recipes mirror build.sh; sources already vendored under src/. CF-touching
# tests add -fblocks; everything from configd on adds --allow-shlib-undefined.
# Apple-PAM tests (pamframeworktest/pammodulestest) are SKIPPED — NextBSD uses
# FreeBSD PAM, those Apple sources aren't vendored.
TUNDEF="-Wl,--allow-shlib-undefined"
LAUNCHINC="-I$SRC/launchd/liblaunch -I$SRC/launchd/freebsd-shims"

comp "libdispatch / libxpc / CoreFoundation tests"
$TCC -o "$TESTDIR/test_libdispatch" "$SRC/libdispatch-tests/test_libdispatch.c" -lsystem_dispatch -lpthread
$TCC -o "$TESTDIR/test_libdispatch_mach" "$SRC/libdispatch-tests/test_libdispatch_mach.c" -lsystem_dispatch -lsystem_kernel -lpthread
$TCC -o "$TESTDIR/test_libxpc" "$SRC/libxpc-tests/test_libxpc.c" -lxpc -llaunch -lsystem_dispatch -lsystem_kernel -lpthread
$TCC -fblocks -o "$TESTDIR/test_corefoundation" "$SRC/libCoreFoundation-tests/test_corefoundation.c" -lCoreFoundation -lsystem_dispatch -lsystem_blocks -lsystem_kernel -lpthread
# acct_test (ACCT-HELPERS marker): the shared account helpers, and the only
# place password hashing is covered at all. Neither adduser nor passwd can set
# a password without a terminal, so nothing end-to-end reaches
# acct_hash_password; this calls it directly, against the crypt(3) we ship.
# That matters because a build host's crypt may not implement the format
# login.conf asks for: Darwin's has no SHA-512 and falls back to DES, so a
# host-only test of hashing would have been testing the wrong algorithm.
$TCC -o "$TESTDIR/acct_test" \
    -I"$SRC/accounts/common" \
    "$SRC/accounts/common/tests/acct_test.c" "$SRC/accounts/common/acct.c" \
    -lcrypt -lutil
test -x "$TESTDIR/acct_test" || { echo "FAIL: acct_test not built"; exit 1; }

# pty_run: a test-only helper that runs a command on a pty and answers its
# prompts, so the on-image suite can drive passwd(1) and later chpass(1).
# Those read secrets with RPP_REQUIRE_TTY, which is correct and also means no
# shell script can feed them; without this, a password change could not be
# tested end to end at all.
$TCC -o "$TESTDIR/pty_run" "$SRC/accounts/tests/pty_run.c" -lutil
test -x "$TESTDIR/pty_run" || { echo "FAIL: pty_run not built"; exit 1; }

# libds_test (LIBDS marker): the DirectoryServices plist writer, exercised
# against NextBSD's own CoreFoundation. Its unit tests cannot run on the
# Ubuntu runners, which have no CF at all, so this is the only place they
# run. It works inside its own mkdtemp fixture and never touches the real
# database. Links the NSS module's independent parser too, because one of
# the things it asserts is that both halves agree on the format.
$TCC -fblocks -DLIBDS_TEST -I"$SRC/libds" -I"$SRC/nss_directory_services" \
    -I"$SRC/Libnotify/freebsd-shims" \
    -o "$TESTDIR/libds_test" \
    "$SRC/libds/tests/libds_test.c" "$SRC/libds/libds.c" \
    "$SRC/nss_directory_services/plist.c" \
    -lCoreFoundation -lsystem_dispatch -lsystem_blocks -lsystem_kernel -lpthread
test -x "$TESTDIR/libds_test" || { echo "FAIL: libds_test not built"; exit 1; }
# test_bsd_logger (SYSLOG-RUN marker): plain libc BSD syslog() round-trip; source
# lives in tests/ (build.sh ~1076). No Darwin libs needed.
$TCC -o "$TESTDIR/test_bsd_logger" "$ROOT/tests/test_bsd_logger.c"

# test_demand_launch (ASLMANAGER-DEMAND marker): bootstrap_look_up + one-way
# mach_msg to a MachServices job, to find out whether launchd demand-launch
# works now that #83 replaced the two stubs #79 blamed. -llaunch for
# bootstrap_look_up/bootstrap_port, -lsystem_kernel for mach_msg; same
# dispatch closure as notifypoke because liblaunch.so is linked
# --no-allow-shlib-undefined.
$TCC -o "$TESTDIR/test_demand_launch" "$ROOT/tests/test_demand_launch.c" \
    -llaunch -lsystem_dispatch -lsystem_kernel -l:libsystem_blocks.so

# notifypoke — posts one notification, so the #91 lost-wakeup test can wake
# notifyd on demand. Deliberately NOT logger(1): that posts to syslogd via
# /var/run/log and never reaches notifyd, which made the first version of that
# test meaningless.
# -llaunch drags in liblaunch.so, which itself references dispatch_* and is
# linked with --no-allow-shlib-undefined, so every dispatch dependency has to
# be named here too. Mirrors Libnotify/Makefile's own LDADD rather than
# guessing the minimal set.
$TCC -o "$TESTDIR/notifypoke" "$ROOT/tests/notifypoke.c" -lnotify -llaunch -lxpc \
    -lsystem_dispatch -lsystem_kernel -l:libsystem_blocks.so

comp "configd tests (reuse MIG configUser.c from \$CONFIGD_MIG)"
for t in configtest notifytest patterntest listtest; do
    $TCC $TUNDEF -I"$CONFIGD_MIG" -I"$SRC/configd" -o "$TESTDIR/$t" \
        "$SRC/configd/$t.c" "$CONFIGD_MIG/configUser.c" -llaunch -lsystem_kernel
done
$TCC $TUNDEF -I"$CONFIGD_MIG" -I"$SRC/configd" -o "$TESTDIR/multitest" \
    "$SRC/configd/multitest.c" "$SRC/configd/config_wire.c" "$CONFIGD_MIG/configUser.c" -llaunch -lsystem_kernel

comp "libSystemConfiguration tests"
SCA="-lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread"
SCB="-lSystemConfiguration -lCoreFoundation -lsystem_dispatch -lsystem_blocks -lsystem_kernel -llaunch -lpthread"
for t in sctest scmultitest scprefstest scpathtest sclocktest scplinktest scnetiftest scnetsvctest scnetsettest scvlantest scbondtest scbridgetest; do
    $TCC -fblocks $TUNDEF -o "$TESTDIR/$t" "$SRC/libSystemConfiguration/$t.c" $SCA
done
for t in scnotifytest scrltest scprefsnotifytest; do
    $TCC -fblocks $TUNDEF -o "$TESTDIR/$t" "$SRC/libSystemConfiguration/$t.c" $SCB
done

comp "libIOKit tests"
IOA="-lIOKit -lCoreFoundation -lsystem_kernel -llaunch -lpthread"
IOB="-lIOKit -lCoreFoundation -lsystem_dispatch -lsystem_blocks -lsystem_kernel -llaunch -lpthread"
$TCC -fblocks $TUNDEF -o "$TESTDIR/iokittest"      "$SRC/libIOKit/iokittest.c"      $IOA
$TCC -fblocks $TUNDEF -o "$TESTDIR/iokitmatchtest" "$SRC/libIOKit/iokitmatchtest.c" $IOA
$TCC -fblocks $TUNDEF -o "$TESTDIR/iokitnotifytest" "$SRC/libIOKit/iokitnotifytest.c" $IOB
$TCC -fblocks $TUNDEF -I"$SRC/libIOKit" -o "$TESTDIR/iokitnotifyrt" "$SRC/libIOKit/iokitnotifyrt.c" $IOB

comp "mDNS / DiskArbitration tests"
$TCC $TUNDEF $LAUNCHINC -o "$TESTDIR/mdnstest" "$SRC/mDNSResponder/mdnstest.c" -llaunch -lsystem_kernel
$TCC $TUNDEF -o "$TESTDIR/dnssdtest" "$SRC/mDNSResponder/dnssdtest.c" -ldns_sd
$TCC $TUNDEF $LAUNCHINC -o "$TESTDIR/datest" "$SRC/DiskArbitration/datest.c" -llaunch -lsystem_kernel

comp "IPConfiguration tests (reuse MIG ipconfigUser.c from \$IPCFG_MIG)"
$TCC $TUNDEF $LAUNCHINC -o "$TESTDIR/ipconfigtest" "$SRC/IPConfiguration/ipconfigtest.c" -llaunch -lsystem_kernel
$TCC $TUNDEF -Wno-macro-redefined -I"$IPCFG_MIG" -I"$SRC/IPConfiguration" $LAUNCHINC \
    -o "$TESTDIR/ipconfigrpctest" "$SRC/IPConfiguration/ipconfigrpctest.c" "$IPCFG_MIG/ipconfigUser.c" -llaunch -lsystem_kernel

comp "hostnamed tests"
HNL="-lSystemConfiguration -lCoreFoundation -lsystem_dispatch -lsystem_blocks -lsystem_kernel -lpthread"
for t in hostnametest hostnameprefset hostnamedhcpset; do
    $TCC -fblocks $TUNDEF $LAUNCHINC -o "$TESTDIR/$t" "$SRC/hostnamed/$t.c" $HNL
done
# dns_sd variants: sysroot include FIRST so the real dns_sd.h shadows the shim stub.
for t in hostnamedmdnsset hostnamedbonjourset; do
    $CROSS_CC --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" -I"$DESTDIR/usr/include" $LAUNCHINC \
        -L"$DESTDIR/usr/lib/system" -L"$SYSROOT/usr/lib" -Wl,-rpath,/usr/lib/system $TUNDEF -fblocks \
        -o "$TESTDIR/$t" "$SRC/hostnamed/$t.c" $HNL -ldns_sd
done

comp "kext_tools test (test_kextd_mach)"
$TCC -o "$TESTDIR/test_kextd_mach" "$SRC/mach_kmod/tests/test_kextd_mach.c" -lsystem_kernel

echo "==> on-image test suite cross-built ($(ls "$TESTDIR" | wc -l | tr -d ' ') binaries in $TESTDIR)"

tier "DONE : system layer (Tiers 0-2) + on-image tests staged into $DESTDIR for $T/$TA"
echo "==> nextbsd-userland cross build complete"
