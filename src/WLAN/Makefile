# wland — NextBSD 802.11 management daemon.
#
# Builds /usr/sbin/wland; runs as a launchd-managed daemon under
# overlay/System/Library/LaunchDaemons/org.nextbsd.wland.plist.
# The `wlan` CLI is linked separately by build-userland.sh, exactly the way
# ipconfig(8) is (bsd.prog.mk builds one PROG per directory).
#
# bsd.prog.mk, NOT CMake: CMake is used by exactly two components in this tree
# and the cross-toolchain file says so explicitly. Every daemon here is
# bsd.prog.mk.
#
# Build:   cd src/WLAN && make SYSROOT=/path/to/rootfs MIGOUT=/path/to/migout
# Install: make DESTDIR=/path/to/rootfs install

PROG=		wland
# Both pages are NextBSD-originals — there is no upstream to vendor (Apple's
# wifid is closed and undocumented). bsd.prog.mk gzips + installs them into
# MANDIR. wlan.8 is listed here even though the wlan CLI is hand-linked by
# build-userland.sh: MAN= installs the pages, independently of which binary
# bsd.prog.mk built.
MAN=		wland.8 wlan.8
SRCS=		wland.c vap.c wpa_ctrl.c supplicant.c
SRCS+=		airport.c mach_service.c wland_mig.c
# iokit_watch.c: IO80211Controller first-match / terminated notifications
# through libIOKit (nextbsd-userland#502).
SRCS+=		iokit_watch.c

# wlanServer.c — MIG-generated server stub for wlan.defs. build-userland.sh runs
# host mig and passes MIGOUT=; a bare standalone build without it fails at the
# generated #include "wlanServer.h", the same way ipconfigd / configd do.
#   -w on the generated TU: MIG output is not WARNS-clean and must not gate the
#     build.
#   -Wno-macro-redefined: the MIG stubs pull <mach/notify.h>, which redefines the
#     MACH_NOTIFY_* macros <mach/message.h> also defines (identical values, only
#     whitespace differs) — the same benign clash ipconfigd / configd document.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:				${MIGOUT}
SRCS+=				wlanServer.c
CFLAGS+=			-I${MIGOUT}
CFLAGS+=			-I${.CURDIR}	# stubs #include "wlan_mig_types.h"
CFLAGS.wlanServer.c+=		-w
CFLAGS+=			-Wno-macro-redefined
.endif

# WARNS=0: airport.c pulls the CoreFoundation public header tree (CFRuntime.h,
# CFBundle.h, CFLocking.h ...). At WARNS>0 the FreeBSD warning machinery adds
# -Wsystem-headers -Werror, which promotes warnings inside the vendored CF
# headers into hard errors. Same carry ipconfigd / libSystemConfiguration use.
WARNS=		0

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# Resolve <IOKit/IOKitLib.h> straight to libIOKit's REAL source header, ahead
# of the freebsd-shims IOKit stub (IOKitWaitQuiet-only, for launchctl), which
# lacks IONotificationPortRef / IOServiceAddMatchingNotification. The matching
# libIOKit.so is still linked from the sysroot (-lIOKit). Same as
# DiskArbitration.
CFLAGS+=	-I${.CURDIR}/../libIOKit

# <servers/bootstrap.h> (bootstrap_check_in + the bootstrap_port global) comes
# from liblaunch; its bootstrap.h pulls Apple shim headers from
# launchd/freebsd-shims. Same -I pair ipconfigd / configd use.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# The link line matters and is easy to get wrong after the #378 rename.
#   -llaunch          bootstrap_check_in + the bootstrap_port global
#   -lsystem_kernel   mach_msg and the Mach port syscalls behind it
#   -lSystemConfiguration  SCDynamicStoreSetValue + SCPreferences — the configd
#                     publish and the known-networks store (airport.c)
#   -lCoreFoundation  CFDictionary / CFString / CFNumber, the dict shape
#                     airport.c builds for State:/.../AirPort
#   -lsystem_dispatch, -l:libsystem_blocks.so  pulled transitively by the SC
#                     umbrella header, and the serial queue iokit_watch.c
#                     takes its IONotificationPort callbacks on
#   -lIOKit           IONotificationPort / IOServiceAddMatchingNotification:
#                     IO80211Controller arrivals and departures (iokit_watch.c).
#                     Built before WLAN in build-userland.sh, as for
#                     DiskArbitration.
#
# NOT -ldispatch and NOT -lBlocksRuntime: those names no longer exist in base.
# They now belong to Gershwin's separate stock copies, and linking them is what
# caused the rtld collision that crashed launchctl / dbus / sshd (#378). Use the
# system_* names, and the exact-file -l: form for blocks.
LDADD+=		-llaunch -lsystem_kernel
LDADD+=		-lSystemConfiguration -lIOKit -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so

# /System/Library holds DATA only (plists, kexts) — never executables. Daemons
# go in /usr/sbin, same as ipconfigd / configd.
BINDIR=		/usr/sbin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
LIBADD+=	pthread
.include <bsd.prog.mk>
