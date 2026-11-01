# libDiskArbitration — the DiskArbitration client library
# (nextbsd-userland#197; port of Apple's DiskArbitration.framework,
# DiskArbitration-535.120.1.0.1, minus the legacy DiskArbitrationPrivate.c).
#
# What a client links (-lDiskArbitration) to talk to diskarbitrationd:
# DASessionCreate, DASessionSetDispatchQueue / DASessionScheduleWithRunLoop,
# the DARegister*Callback family, DADiskCreateFromBSDName /
# DADiskCreateFromVolumePath, DADiskCopyDescription, DADiskMount and the
# rest of Apple's public API, over the DAServer.defs MIG protocol.
#
# Flat layout, like every Darwin library in this tree (libIOKit,
# libSystemConfiguration, libCoreFoundation), not a .framework bundle:
#   -> /usr/lib/system/libDiskArbitration.so(.1)
#   -> /usr/include/DiskArbitration/{DiskArbitration,DADisk,DASession,
#      DADissenter,DiskArbitrationPrivate}.h
#
# Like libSystemConfiguration, this needs the MIG-generated client stubs
# (DAServerUser.c, header DAServerUser.h: the daemon has a DAServer.h of
# its own) and cannot build without MIGOUT: build-userland.sh runs
# mig on ../DiskArbitration/DAServer.defs and passes MIGOUT=. DAInternal.c
# (serialization, shared with the daemon) is compiled from
# ../DiskArbitration.
#
# Build:   cd src/libDiskArbitration && make SYSROOT=... MIGOUT=...
# Install: make DESTDIR=/path/to/rootfs install

LIB=		DiskArbitration
SHLIB_MAJOR=	1

SRCS=		DASession.c DADisk.c DADissenter.c DiskArbitration.c
.PATH:		${.CURDIR}/../DiskArbitration
SRCS+=		DAInternal.c
CFLAGS+=	-I${.CURDIR}/../DiskArbitration	# DAInternal.h

MIGOUT?=
.if !empty(MIGOUT)
.PATH:			${MIGOUT}
SRCS+=			DAServerUser.c
CFLAGS+=		-I${MIGOUT}
CFLAGS.DAServerUser.c+=	-w
.endif
# <mach/notify.h> redefines the MACH_NOTIFY_* macros <mach/message.h> also
# defines (identical values); configd and libSystemConfiguration suppress
# the same benign clash.
CFLAGS+=		-Wno-macro-redefined

# WARNS=0: the CoreFoundation header tree trips -Wsystem-headers -Werror at
# WARNS>0 (CFLocking.h's #warning, -Wexpansion-to-defined), as for
# libSystemConfiguration and libIOKit.
WARNS=		0
NO_MAN=		yes

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-I${.CURDIR}		# <DiskArbitration/...>

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDADD+=		-L${SYSROOT}/usr/lib/system
.endif

# <servers/bootstrap.h> and <bootstrap_priv.h> (bootstrap_look_up2,
# BOOTSTRAP_PRIVILEGED_SERVER) come from liblaunch, whose headers pull
# Apple shims from launchd/freebsd-shims. <notify.h> includes os/base.h from
# the libnotify shims.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
CFLAGS+=	-I${.CURDIR}/../Libnotify/freebsd-shims

INCS=		DiskArbitration/DiskArbitration.h \
		DiskArbitration/DADisk.h \
		DiskArbitration/DASession.h \
		DiskArbitration/DADissenter.h \
		DiskArbitration/DiskArbitrationPrivate.h
INCSDIR=	${PREFIX}/include/DiskArbitration
LIBDIR=		${PREFIX}/lib/system

# -lCoreFoundation: the CF object model and property-list serialization.
# -lsystem_dispatch + -lsystem_blocks: the MACH_RECV source on the client
#   port, and the keep-alive notify_register_dispatch block.
# -lsystem_kernel: mach_msg and the Mach port calls behind the MIG stubs.
# -llaunch: bootstrap_look_up2. -lnotify: the keep-alive session's
#   com.apple.diskarbitrationd.launched registration.
LDADD+=		-lCoreFoundation -lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel -llaunch -lnotify
LDADD+=		-Wl,--allow-shlib-undefined
LDADD+=		-Wl,-rpath,${PREFIX}/lib/system

PREFIX?=	/usr

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD.
LIBADD+=	pthread
.include <bsd.lib.mk>
