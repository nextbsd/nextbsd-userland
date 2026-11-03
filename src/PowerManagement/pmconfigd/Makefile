# pmconfigd — Apple's power management daemon (macOS ships it as powerd),
# ported to NextBSD.
#
# Builds /usr/sbin/pmconfigd; runs as the launchd job org.nextbsd.powerd
# (overlay/System/Library/LaunchDaemons/org.nextbsd.powerd.plist), vending
# Apple's two MachServices, com.apple.PowerManagement.control (MIG) and
# com.apple.iokit.powerdxpc (XPC).
#
# This Makefile is NextBSD's. Apple's drop shipped a Makefile here that
# predates the Xcode project (i386/ppc, files that no longer exist); the
# Xcode project's powerd target is the source of truth for what the daemon
# is made of, and SRCS below grows toward it. See ../VENDORED.md.
#
# The binary is pmconfigd, not powerd: /usr/sbin/powerd is still FreeBSD's
# CPU-frequency daemon until pmconfigd owns CPU policy (nextbsd-userland#231).
#
# Build:   make SYSROOT=/path/to/rootfs MIGOUT=/path/to/migout
# Install: make DESTDIR=/path/to/rootfs install

PROG=		pmconfigd
# NextBSD-original page: Apple ships no page for powerd.
MAN=		pmconfigd.8
BINDIR=		/usr/sbin

# NextBSD sources live in ../freebsd-shims, Apple's in this directory and
# ../common. As each Apple file is ported it joins SRCS and the matching part
# of freebsd-shims goes away:
#   pmconfigd_freebsd.c     main() and powerd_init() until pmconfigd.m and
#                           PrivateLib.m build
#   powermanagement_stubs.c the powermanagement.defs routines whose Apple
#                           files are not built yet
#   os_log_shim.c           os_log() for every file (os/log.h)
.PATH:		${.CURDIR}/../freebsd-shims ${.CURDIR}/../common
SRCS=		pmconfigd_freebsd.c
SRCS+=		powermanagement_stubs.c
SRCS+=		os_log_shim.c

# powermanagementServer.c — MIG server stub for powermanagement.defs.
# build-userland.sh runs the host mig and passes MIGOUT=; a standalone build
# without it fails at the generated #include "powermanagementServer.h", the
# same way wland / ipconfigd do.
#   -w on the generated TU: MIG output is not WARNS-clean.
#   -Wno-macro-redefined: the MIG stubs pull <mach/notify.h>, which redefines
#     the MACH_NOTIFY_* macros <mach/message.h> also defines (identical
#     values), the same benign clash wland and configd document.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:				${MIGOUT}
SRCS+=				powermanagementServer.c
CFLAGS+=			-I${MIGOUT}
CFLAGS.powermanagementServer.c+= -w
CFLAGS+=			-Wno-macro-redefined
.endif

# Vendored Apple source joins this build ticket by ticket; don't gate on its
# warnings (same as Libnotify, syslog and configd).
WARNS?=		0

CFLAGS+=	-O2 -pipe -fblocks

# Header search order matters:
#   1. ../freebsd-shims first, so <os/log.h>, <os/signpost.h> and
#      <IOKit/pwr_mgt/powermanagement*.h> resolve to pmconfigd's shims ahead
#      of launchd's freebsd-shims/os and the sysroot;
#   2. this directory and ../common for Apple's own headers;
#   3. libIOKit's source headers (<IOKit/IOKitLib.h>, the real one), ahead
#      of launchd/freebsd-shims' IOKit stub, as wland and DiskArbitration do;
#   4. liblaunch for <servers/bootstrap.h> and its launchd/freebsd-shims pair.
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR} -I${.CURDIR}/../common
CFLAGS+=	-I${.CURDIR}/../../libIOKit
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../../launchd/freebsd-shims

# pmconfigd's MIG listener is Apple's: dispatch_mach_create_f() and
# dispatch_mach_mig_demux(), libdispatch SPI that NextBSD's libdispatch
# carries (notifyd uses the same pair). <dispatch/private.h> exposes it only
# with DISPATCH_MACH_SPI.
CFLAGS+=	-DDISPATCH_MACH_SPI=1

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

#   -llaunch          bootstrap_check_in + the bootstrap_port global
#   -lxpc             the com.apple.iokit.powerdxpc listener
#   -lsystem_dispatch the main queue and the dispatch_mach MIG channel
#   -lCoreFoundation  %@ in os_log_shim.c (and, later, everything)
#   -lsystem_kernel   mach_msg, vm_deallocate, the Mach port calls
#   -l:libsystem_blocks.so  the XPC event-handler blocks
# Use the system_* names and the exact-file -l: form for blocks, never
# -ldispatch / -lBlocksRuntime (#378).
LDADD+=		-llaunch -lxpc -lsystem_dispatch -lCoreFoundation
LDADD+=		-lsystem_kernel -l:libsystem_blocks.so

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
LIBADD+=	pthread
.include <bsd.prog.mk>
