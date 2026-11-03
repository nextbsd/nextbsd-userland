# pmset(1) — Apple's power management command line tool, ported to NextBSD
# (nextbsd-userland#235, E17 C1).
#
# Builds /usr/bin/pmset, as on macOS. pmset reads and writes the Energy
# Saver settings, schedules power events and lists power sources and
# assertions, all through libIOKit's IOPM* and IOPS* calls to powerd
# (com.apple.PowerManagement.control), plus a few powermanagement.defs calls
# of its own, as Apple's does.
#
# This Makefile is NextBSD's. Apple's drop shipped a Makefile here that
# predates the Xcode project (i386, a local mig run); the Xcode project's
# pmset target (pmset.m, CommonLib.c, the powermanagement.defs user stub) is
# the source of truth. pmset.m is built as C through
# ../freebsd-shims/pmset_freebsd.c, which also carries the CommonLib.c
# functions it calls; ../freebsd-shims/pmset_freebsd.h stands in for the
# Darwin-only headers. See ../VENDORED.md.
#
# Build:   make SYSROOT=/path/to/rootfs MIGOUT=/path/to/migout
# Install: make DESTDIR=/path/to/rootfs install

PROG=		pmset
MAN=		pmset.1
BINDIR=		/usr/bin

.PATH:		${.CURDIR}/../freebsd-shims
SRCS=		pmset_freebsd.c

# powermanagementUser.c — the MIG user stub for powermanagement.defs, which
# Apple's pmset target compiles in for the one call it makes itself
# (io_pm_get_value_int, pmset mt2book). build-userland.sh
# passes MIGOUT=, the same generation libIOKit and powerd use. -w on the
# generated TU: MIG output is not WARNS-clean.
MIGOUT?=
.if empty(MIGOUT)
.error MIGOUT must name the directory holding powermanagementUser.c (build-userland.sh)
.endif
.PATH:				${MIGOUT}
SRCS+=				powermanagementUser.c
CFLAGS+=			-I${MIGOUT}
CFLAGS.powermanagementUser.c+=	-w
CFLAGS+=			-Wno-macro-redefined

# Vendored Apple source; don't gate on its warnings (same as powerd).
# Apple's code converts between pointers and integers, which clang 19 makes
# errors by default; Apple's toolchain warns.
WARNS?=		0
CFLAGS+=	-Wno-int-conversion

CFLAGS+=	-O2 -pipe -fblocks

# Header search order:
#   1. ../freebsd-shims for pmset_freebsd.h, <SystemConfiguration/
#      SCValidation.h> and <libproc.h> (proc_name());
#   2. ../common for Apple's CommonLib.h;
#   3. libIOKit's source headers (<IOKit/IOKitLib.h>, <IOKit/pwr_mgt/...>,
#      <IOKit/ps/...>), ahead of launchd/freebsd-shims' IOKit stub;
#   4. syslog's <asl.h> (pmset -g log reads powerd's ASL store), ahead of
#      launchd/freebsd-shims' empty asl.h;
#   5. liblaunch for <servers/bootstrap.h> and <bootstrap_priv.h>.
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR}/../common
CFLAGS+=	-I${.CURDIR}/../../libIOKit
CFLAGS+=	-I${.CURDIR}/../../syslog/libsystem_asl.tproj/include
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../../launchd/freebsd-shims
#   6. <notify.h> includes <os/base.h>, which only Libnotify's shims carry.
#      Searched after the sysroot, so it adds that header and shadows none.
CFLAGS+=	-idirafter ${.CURDIR}/../../Libnotify/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

#   -lIOKit           IOPM*, IOPS*, the registry
#   -lCoreFoundation  everything
#   -lSystemConfiguration  -g live (State:/IOKit/PowerManagement/...)
#   -lnotify          the notify(3) state pmset reads and listens to
#   -lsystem_asl      pmset -g log
#   -llaunch          bootstrap_look_up2() for powerd's MIG port
#   -lsystem_dispatch, -l:libsystem_blocks.so  the getter table's blocks and
#                     the dispatch sources of the -g ...log listeners
#   -lsystem_kernel   mach_msg and the Mach port calls
# Use the system_* names and the exact-file -l: form for blocks, never
# -ldispatch / -lBlocksRuntime (#378).
LDADD+=		-lIOKit -lCoreFoundation -lSystemConfiguration -lnotify
LDADD+=		-lsystem_asl -llaunch
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
#   m       floor() in the date code (pmset_freebsd.h)
LIBADD+=	pthread m
.include <bsd.prog.mk>
