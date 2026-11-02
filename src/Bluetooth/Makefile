# bluetoothd — NextBSD Bluetooth daemon.
#
# Builds /usr/sbin/bluetoothd; runs as a launchd-managed daemon under
# overlay/System/Library/LaunchDaemons/org.nextbsd.bluetoothd.plist.
# The `bluetooth` CLI is linked separately by build-userland.sh, exactly the
# way wlan(8) is (bsd.prog.mk builds one PROG per directory).
#
# bsd.prog.mk, NOT CMake, as every daemon here.
#
# Build:   cd src/Bluetooth && make SYSROOT=/path/to/rootfs MIGOUT=/path/to/migout
# Install: make DESTDIR=/path/to/rootfs install

PROG=		bluetoothd
# Both pages are NextBSD originals: Apple's bluetoothd is closed and
# undocumented. bluetooth.8 is listed here although the CLI is hand-linked by
# build-userland.sh: MAN= installs the pages independently of which binary
# bsd.prog.mk built.
MAN=		bluetoothd.8 bluetooth.8
SRCS=		bluetoothd.c mach_service.c bluetoothd_mig.c
# iokit_watch.c: IOBluetoothHostController first-match / terminated.
SRCS+=		iokit_watch.c
# The engine contract (engine.h) and the two engines behind it.
SRCS+=		engine.c classic_engine.c le_engine.c
# CoreFoundation modules: the pairing store and policy (btstore.c), the
# secrets (keystore.c), the SCDynamicStore publication (btstate.c), and their
# shared helpers (btcf.c).
SRCS+=		btcf.c btstore.c keystore.c btstate.c
# SDP: the server on L2CAP PSM 1 with the local records, the client that reads
# a peer's HID, PnP and audio records at pair time, and their shared wire format.
SRCS+=		sdp_proto.c sdp_server.c sdp_client.c

# bluetoothServer.c — MIG-generated server stub for bluetooth.defs.
# build-userland.sh runs host mig and passes MIGOUT=; a standalone build
# without it fails at the generated #include "bluetoothServer.h", as wland's.
#   -w on the generated TU: MIG output is not WARNS-clean.
#   -Wno-macro-redefined: the MIG stubs pull <mach/notify.h>, which redefines
#     the MACH_NOTIFY_* macros <mach/message.h> also defines (identical
#     values), the same benign clash wland / ipconfigd document.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:				${MIGOUT}
SRCS+=				bluetoothServer.c
CFLAGS+=			-I${MIGOUT}
CFLAGS+=			-I${.CURDIR}	# stubs #include "bluetooth_mig_types.h"
CFLAGS.bluetoothServer.c+=	-w
CFLAGS+=			-Wno-macro-redefined
.endif

# WARNS=0: the CF modules pull the CoreFoundation public header tree, and at
# WARNS>0 the FreeBSD warning machinery promotes warnings inside the vendored
# CF headers into errors. Same carry as wland.
WARNS=		0

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# <IOKit/IOKitLib.h> straight from libIOKit's real source header, ahead of the
# freebsd-shims IOKit stub, which lacks IONotificationPortRef. Same as wland.
CFLAGS+=	-I${.CURDIR}/../libIOKit

# <servers/bootstrap.h> from liblaunch, with its Apple shim headers.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# The link line, as wland's:
#   -llaunch          bootstrap_check_in + the bootstrap_port global
#   -lsystem_kernel   mach_msg and the Mach port syscalls behind it
#   -lSystemConfiguration  SCPreferences (the pairing store) and
#                     SCDynamicStore (State:/Bluetooth)
#   -lIOKit           IONotificationPort / IOServiceAddMatchingNotification:
#                     IOBluetoothHostController arrivals and departures
#   -lCoreFoundation  the dictionaries both of the above take
#   -lsystem_dispatch, -l:libsystem_blocks.so  the serial queue the IOKit
#                     callbacks run on, and what the SC header pulls
#
# NOT -ldispatch and NOT -lBlocksRuntime: those names belong to Gershwin's
# separate copies, and linking them caused the rtld collision of #378.
LDADD+=		-llaunch -lsystem_kernel
LDADD+=		-lSystemConfiguration -lIOKit -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so

# Daemons go in /usr/sbin; /System/Library holds data only.
BINDIR=		/usr/sbin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD.
LIBADD+=	pthread
.include <bsd.prog.mk>
