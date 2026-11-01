# diskarbitrationd — freebsd-launchd-mach DiskArbitration daemon.
#
# main + bootstrap_check_in for com.apple.DiskArbitration.diskarbitrationd,
# the kernel registry storage subscription (da_iokit_subscribe.c) and the GEOM disk
# model (DADisk.c, DAGeom.c; #192) with adopted mounts (DAVolume.c;
# #193), the bundle probe stage (DAFileSystem.c, DAProbe.c; #194; the
# bundles' <name>.util helper is fsutil/), and the stage machine and mount
# executor (DAStage.c, DAMount.c, DAMountHelper.c, DACommand.c,
# DASupport.c; #195), and the /etc/fstab mount map (DASupport.c over
# fstab.c, Apple's vendored getfsent copy; #196), and the MIG server for
# libDiskArbitration clients: sessions, callback queues and the mount
# request (DAServer.c, DASession.c, DAQueue.c, DARequest.c, DAInternal.c,
# which the library builds too; #197), and unmount/eject with their
# approval rounds (DARequest.c, DAThread.c, DADevice.c; #198), and the
# console user from configd (DASupport.c, DAServer.c; #199).
#
# Build:   cd src/DiskArbitration && make SYSROOT=/path/to/rootfs MIGOUT=/path
# Install: make DESTDIR=/path/to/rootfs install

PROG=		diskarbitrationd
# Man page vendored from apple-oss-distributions/DiskArbitration @ DiskArbitration-535.120.1.0.1 (diskarbitrationd/diskarbitrationd.8).
# bsd.prog.mk gzips + installs into MANDIR.
MAN=		diskarbitrationd.8
SRCS=		diskarbitrationd.c da_iokit_subscribe.c DADisk.c DAGeom.c DAVolume.c \
		DAFileSystem.c DAProbe.c DACommand.c DAMount.c DAMountHelper.c \
		DAStage.c DASupport.c fstab.c \
		DAInternal.c DASession.c DAQueue.c DAServer.c DARequest.c \
		DAThread.c DADevice.c

# DAServerServer.c — MIG-generated server stub for DAServer.defs.
# build-userland.sh runs host mig and passes MIGOUT=; a bare standalone
# build without it fails at the generated #include "DAServerServer.h",
# the same way configd / wland do. -w on the generated TU: MIG output is
# not warning-clean. -Wno-macro-redefined: <mach/notify.h> redefines the
# MACH_NOTIFY_* macros <mach/message.h> also defines (identical values).
MIGOUT?=
.if !empty(MIGOUT)
.PATH:				${MIGOUT}
SRCS+=				DAServerServer.c
CFLAGS+=			-I${MIGOUT}
CFLAGS.DAServerServer.c+=	-w
.endif
# The generated stubs import "DAServer.defs.h" from this directory.
CFLAGS+=			-I${.CURDIR}
CFLAGS+=			-Wno-macro-redefined

# <DiskArbitration/DADissenter.h> (the kDAReturn* codes) from the
# library's source tree, as Apple's daemon includes the framework header
# with __DISKARBITRATIOND__ defined. libDiskArbitration builds after the
# daemon.
CFLAGS+=	-I${.CURDIR}/../libDiskArbitration

# WARNS=0: da_iokit_subscribe.c pulls <IOKit/IOKitLib.h>, which drags in the
# whole CoreFoundation + dispatch public header tree (the IONotificationPort /
# IOServiceMatchingCallback / CFDictionary surface). At WARNS>0 the FreeBSD
# warning machinery adds -Wsystem-headers -Werror, which promotes warnings
# *inside* the vendored CF headers (CFLocking.h's "CF is not thread-safe"
# #warning, -Wexpansion-to-defined) into hard errors. libIOKit + its test
# clients + kext_tools all build at WARNS=0 for exactly this reason. The other
# two DA sources (diskarbitrationd.c, hwreg_subscribe.c) still get the explicit
# warning set below.
WARNS=		0

CFLAGS+=	-O2 -pipe
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# -fblocks: <IOKit/IOKitLib.h> pulls CoreFoundation + <dispatch/dispatch.h>,
# whose headers carry block-typed function pointers that need -fblocks even
# where DA itself uses no blocks. Same flag libIOKit + its clients carry.
CFLAGS+=	-fblocks

# Resolve <IOKit/IOKitLib.h> straight to libIOKit's REAL source header. The
# installed sysroot copy wasn't reliably winning over the freebsd-shims IOKit
# stub (IONotificationPortRef / IOIteratorNext / IOObjectGetClass etc. went
# undeclared in da_iokit_subscribe.c), so point at the source dir first. The
# matching libIOKit.so is still linked from the sysroot (-lIOKit).
CFLAGS+=	-I${.CURDIR}/../libIOKit

# ${SYSROOT}/usr/include MUST precede freebsd-shims: <IOKit/IOKitLib.h> has
# to resolve to libIOKit's REAL installed header (IONotificationPortRef /
# io_iterator_t / io_name_t / IOServiceAddMatchingNotification, which
# da_iokit_subscribe.c needs) and NOT the freebsd-shims IOKit stub
# (IOKitWaitQuiet-only, for launchctl). IOKitLib.h is the only header present
# in both trees; AvailabilityMacros.h / NSSystemDirectories.h etc. are
# Apple-only and still resolve from freebsd-shims regardless of order.
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
.endif

# <servers/bootstrap.h> (bootstrap_check_in + bootstrap_port global)
# comes from liblaunch; its bootstrap.h pulls in Apple shim headers
# (AvailabilityMacros.h, ...) from launchd/freebsd-shims. Same -I
# pair hwregd / configd / ipconfigd / mDNSResponder use.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
# <notify.h> includes os/base.h from the libnotify shims (configd takes the
# same -I).
CFLAGS+=	-I${.CURDIR}/../Libnotify/freebsd-shims

.if !empty(SYSROOT)
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -llaunch: bootstrap_check_in + the bootstrap_port global.
# -lsystem_kernel: mach_msg + the Mach port syscalls behind it.
# -lpthread: libIOKit's IONotificationPort receive loop (linked transitively).
# -lIOKit: the IONotificationPort / IOServiceAddMatchingNotification kernel-
#   notify path (C1.3, #218). -lCoreFoundation: the CFDictionary matching-dict
#   object model libIOKit's API consumes. -lsystem_dispatch + -lsystem_blocks: the
#   dispatch queue DA delivers notification callbacks on
#   (IONotificationPortSetDispatchQueue) + the -fblocks runtime CF/dispatch pull.
# -lnotify: notify_post(com.apple.diskarbitrationd.launched) at startup,
#   which keep-alive client sessions wait for (#197).
LDADD+=		-llaunch -lsystem_kernel -lnotify
# -lSystemConfiguration: the console user, State:/Users/ConsoleUser,
#   watched through configd (DASupport.c, #199).
LDADD+=		-lSystemConfiguration
LDADD+=		-lIOKit -lCoreFoundation -lsystem_dispatch -l:libsystem_blocks.so

BINDIR=		/usr/sbin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
# geom: DAGeom.c walks the GEOM tree with libgeom (#192); src.libnames.mk
# pulls in its bsdxml + sbuf dependencies.
# procstat: DADevice.c finds the process keeping a volume busy, fuser -c's
# way (#198). cam: DADevice.c's START STOP UNIT eject for da(4) (#198).
LIBADD+=	pthread geom procstat cam
.include <bsd.prog.mk>
