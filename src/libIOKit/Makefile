# libIOKit — the IOKit userland facade (freebsd-launchd-mach K1+K2 of
# the IOKit-userland port plan), iter 1.
#
# A thin CF-flavored client over the kernel /dev/ioregistry device (the
# in-kernel IORegistry, nextbsd#214/#225). Not a port of Apple's IOKitUser
# source — that library decomposes to a kernel device.defs surface this repo
# implements differently; instead the facade re-aims the Apple-shape API at
# /dev/ioregistry. iter 1 ships the read-only registry walk
# (IORegistryGetRootEntry, IORegistryEntryGetChildIterator, IOIteratorNext,
# GetName, GetPath, IOObject{Retain,Release}); later iters add properties +
# matching, the ioreg tool, and notifications.
#
# Build:   cd src/libIOKit && make SYSROOT=...
# Install: make DESTDIR=/path/to/rootfs install
#   -> /usr/lib/system/libIOKit.so(.1)
#   -> /usr/include/IOKit/IOKitLib.h

LIB=		IOKit
SHLIB_MAJOR=	1

SRCS=		IOKitLib.c IOKitMatching.c IOKitNotify.c
SRCS+=		IOPMLib.c
SRCS+=		IOHIDUserDevice.c

# Power sources and power management (nextbsd-userland#232, #233):
# IOKitUser's ps.subproj and pwr_mgt.subproj clients, vendored from
# apple-oss-distributions/IOKitUser @ IOKitUser-100231.120.3 with
# __FreeBSD__ changes listed at the top of each file (IOPowerSources.c,
# IOPowerSourcesPrivate.c, IOPMAssertions.c, IOPMLibPrivate.c,
# IOPMEnergyPrefs.c; for pmset, #235: the scheduled and repeating power
# events of IOPMAutoWake.c and IOPMRepeatingPower.c and the warning levels
# of IOPMPowerNotifications.c; ps_freebsd.h and pwr_mgt_freebsd.h stand in
# for the Darwin-only headers). They reach powerd over MIG, through the user stub
# that build-userland.sh generates from IOKit/pwr_mgt/powermanagement.defs,
# the same file pmconfigd's server stub comes from; pass its directory as
# MIGOUT. Apple's sources compile with pointer/integer conversions that
# clang 19 makes errors by default; Apple's toolchain warns.
SRCS+=		IOPowerSources.c IOPowerSourcesPrivate.c
SRCS+=		IOPMAssertions.c IOPMLibPrivate.c IOPMEnergyPrefs.c
SRCS+=		IOPMAutoWake.c IOPMRepeatingPower.c IOPMPowerNotifications.c
CFLAGS.IOPowerSources.c+=		-Wno-int-conversion
CFLAGS.IOPowerSourcesPrivate.c+=	-Wno-int-conversion
CFLAGS.IOPMAssertions.c+=		-Wno-int-conversion
CFLAGS.IOPMLibPrivate.c+=		-Wno-int-conversion
CFLAGS.IOPMEnergyPrefs.c+=		-Wno-int-conversion
CFLAGS.IOPMAutoWake.c+=			-Wno-int-conversion
CFLAGS.IOPMRepeatingPower.c+=		-Wno-int-conversion
MIGOUT?=
.if empty(MIGOUT)
.error MIGOUT must name the directory holding powermanagementUser.c (build-userland.sh)
.endif
.PATH:		${MIGOUT}
SRCS+=		powermanagementUser.c
CFLAGS.powermanagementUser.c+=	-w
CFLAGS+=	-I${MIGOUT}

# WARNS=0: the public header pulls the whole CoreFoundation public
# header tree (for later iters' CFDictionary / CFString surfaces).
# At WARNS>0 the FreeBSD warning machinery adds -Wsystem-headers
# -Werror, which promotes warnings *inside* the vendored CF headers
# (-Wexpansion-to-defined; CFLocking.h's "CF is not thread-safe"
# #warning) into hard errors. libxpc / libCoreFoundation /
# libSystemConfiguration build at WARNS=0 for the same reason.
WARNS=		0

# Man page vendored from apple-oss-distributions/IOKitTools @ IOKitTools-125
# (ioreg.tproj/ioreg.8) — documents the ioreg(8) tool build.sh links from
# ioreg.c. This lib's `make install` (run by the driver) gzips + installs it
# into MANDIR via bsd.lib.mk.
MAN=		ioreg.8

# IOHIDUserDevice(3): the virtual-HID API (nextbsd-userland#521).
MAN+=		IOHIDUserDevice.3
MLINKS=		IOHIDUserDevice.3 IOHIDUserDeviceCreateWithProperties.3 \
		IOHIDUserDevice.3 IOHIDUserDeviceActivate.3 \
		IOHIDUserDevice.3 IOHIDUserDeviceCancel.3 \
		IOHIDUserDevice.3 IOHIDUserDeviceHandleReportWithTimeStamp.3

# -fblocks: the public header pulls CoreFoundation, and CoreFoundation's
# ForSwiftFoundationOnly.h / CFCalendarPriv.h / CFStreamPriv.h declare
# block-typed function pointers (`void (^block)(...)`). Without -fblocks
# clang errors out with "blocks support disabled" on every include
# site. libSystemConfiguration / libxpc carry -fblocks for the same
# reason.
CFLAGS+=	-O2 -pipe -fblocks

# -I${.CURDIR} MUST come first so <IOKit/IOKitLib.h> resolves to this
# library's public header (src/libIOKit/IOKit/IOKitLib.h) rather than
# the small launchctl stub at src/launchd/freebsd-shims/IOKit/
# IOKitLib.h that the freebsd-shims -I below also covers. Search
# order matches first; that umbrella -I also picks up IOKitInternal.h.
CFLAGS+=	-I${.CURDIR}

# liblaunch + launchd/freebsd-shims supply Apple shim headers
# (AvailabilityMacros.h etc.) pulled transitively by the CoreFoundation
# public headers.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
# <notify.h> includes <os/base.h>, which only Libnotify's shims carry.
# Searched after the sysroot, so it adds that header and shadows none.
CFLAGS+=	-idirafter ${.CURDIR}/../Libnotify/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDADD+=		-L${SYSROOT}/usr/lib/system
.endif

INCS=		IOKit/IOKitLib.h IOKit/IOMessage.h IOKit/IOReturn.h
INCSDIR=	${PREFIX}/include/IOKit

# <IOKit/hid/...>: IOHIDUserDevice (nextbsd-userland#521). A separate group
# so the headers keep their hid/ directory, as in IOKit.framework.
INCSGROUPS=	INCS HIDINCS PMINCS PSINCS
HIDINCS=	IOKit/hid/IOHIDKeys.h IOKit/hid/IOHIDUserDevice.h
HIDINCSDIR=	${PREFIX}/include/IOKit/hid

# <IOKit/pwr_mgt/...> (nextbsd-userland#233): IOKitUser's power management
# headers (IOPMLib.h, IOPMLibPrivate.h, IOPMKeys.h, IOPMAssertionCategories.h,
# the MIG description and its types) and the xnu ones they include
# (IOPM.h, IOPMPrivate.h, IOPMLibDefs.h; xnu-12377.121.6), as IOKit.framework
# carries them.
PMINCS=		IOKit/pwr_mgt/IOPM.h IOKit/pwr_mgt/IOPMAssertionCategories.h \
		IOKit/pwr_mgt/IOPMKeys.h IOKit/pwr_mgt/IOPMLib.h \
		IOKit/pwr_mgt/IOPMLibDefs.h IOKit/pwr_mgt/IOPMLibPrivate.h \
		IOKit/pwr_mgt/IOPMPrivate.h IOKit/pwr_mgt/powermanagement.defs \
		IOKit/pwr_mgt/powermanagement_mig.h
PMINCSDIR=	${PREFIX}/include/IOKit/pwr_mgt

# <IOKit/ps/...> (nextbsd-userland#232): IOKitUser's power source headers,
# as IOKit.framework carries them.
PSINCS=		IOKit/ps/IOPowerSources.h IOKit/ps/IOPowerSourcesPrivate.h \
		IOKit/ps/IOPSKeys.h IOKit/ps/IOPSKeysPrivate.h
PSINCSDIR=	${PREFIX}/include/IOKit/ps
LIBDIR=		${PREFIX}/lib/system

# -lCoreFoundation: the CFDictionary / CFString / CFNumber object
# model behind the iter-2 property and matching surface (the public
# header pulls CoreFoundation too). -lsystem_dispatch +
# -lsystem_blocks: the iter-4 IONotificationPortSetDispatchQueue +
# dispatch_async_f path (the public header pulls <dispatch/dispatch.h>
# too). -lsystem_kernel: mach_msg + the Mach port syscalls behind
# the IONotificationPort receive loop. -lpthread: pthread_once for the
# lazy /dev/ioregistry fd + the IONotificationPort receive thread.
LDADD+=		-lCoreFoundation
# The power source and power management clients: notify(3) for pmconfigd's
# state and the prefs-change notification, XPC for its powerdxpc service,
# SystemConfiguration for the low battery warning level and the sleep/wake
# failure record. libnotify is built ahead of libIOKit for this.
LDADD+=		-lnotify -lxpc -lSystemConfiguration
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel -llaunch
LDADD+=		-Wl,--allow-shlib-undefined
LDADD+=		-Wl,-rpath,${PREFIX}/lib/system

PREFIX?=	/usr

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
# nv: base FreeBSD libnv (<sys/nv.h>), the same nvlist code the kernel uses to
# pack the /dev/ioregistry IOREGIOCPROPS property bag (iokit_registry.c), so
# the wire format matches. IOKitMatching.c decodes the bag with it. libxpc's
# private nvlist copy has a different wire format and is not used here.
LIBADD+=	pthread nv
# execinfo: backtrace() for an assertion's creator backtrace, which
# IOPMAssertions.c records when pmset asks for it (collectbt).
LIBADD+=	execinfo
# m: lround() in IOPMAutoWake.c's tellClockController (libSystem on macOS).
LIBADD+=	m

# The kext API (nextbsd-userland#438): Apple's OSKext engine and KextManager
# live in IOKit.framework, so libkext (src/kext_tools/kext.subproj, built
# first by build-userland.sh) is linked whole into libIOKit. KextManager's
# load requests go to kextd's org.nextbsd.KernelExtensionServer.
LIBKEXT?=
.if !empty(LIBKEXT)
LDADD+=		-Wl,--whole-archive ${LIBKEXT} -Wl,--no-whole-archive
DPADD+=		${LIBKEXT}
LIBADD+=	z
.endif
# IOHIDUserDevice.c loads IOHIDUserDevice.kext with
# KextManagerLoadKextWithIdentifier, so it needs KextManager.h (from
# kext.subproj) and the libkern/OSReturn.h it includes. The compat tree is
# searched last, so the sysroot's own headers still win.
CFLAGS.IOHIDUserDevice.c+=	-I${.CURDIR}/../kext_tools/kext.subproj
CFLAGS.IOHIDUserDevice.c+=	-idirafter ${.CURDIR}/../kext_tools/compat
.include <bsd.lib.mk>
