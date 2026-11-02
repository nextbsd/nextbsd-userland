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
# iokit_watch.c: IOBluetoothHostController first-match / terminated, and
# IOUSBHostDevice first-match for controllers that need firmware.
SRCS+=		iokit_watch.c
# The controller lifecycle (#207): kexts and netgraph graph (ngstack.c), the
# raw HCI socket (hci.c), bring-up and power (adapter.c), the discovery cache
# (discovery.c) and the pairing prompts (pairing.c).
SRCS+=		adapter.c ngstack.c hci.c discovery.c pairing.c
# The engine contract (engine.h) and the two engines behind it; the Classic
# engine's security manager (#209) is security.c, its ACL links (#213)
# links.c.
SRCS+=		engine.c classic_engine.c security.c links.c
# The LE engine (#205): GAP (le_engine.c), the ATT bearer (att.c), the GATT
# client (gatt.c), the Security Manager (smp.c) and its crypto toolbox
# (le_crypto.c, libcrypto); HID over GATT into IOHIDUserDevice (hogp.c, #217,
# a CoreFoundation module like the store's).
SRCS+=		le_engine.c att.c gatt.c smp.c le_crypto.c hogp.c
# Connections, reconnect at power-up and State:/Bluetooth/Device (#213); the
# Classic HID host, HIDP into IOHIDUserDevice (#524).
SRCS+=		connections.c hid_host.c
# Audio (#218, #219, #220): the policy and the mixer (audio.c) run through
# the child supervisor (child.c); AVRCP into IOHIDUserDevice media keys
# (avrcp.c); the A2DP sink with its AVDTP acceptor and SBC decoder.
SRCS+=		audio.c child.c avrcp.c a2dp_sink.c avdtp.c sbc_decode.c
# The hands-free gateway (#525): the AG's RFCOMM and AT layer, SCO setup and
# routing (hfp.c); the SCO link as /dev/btscoN for the mixer, served through
# cuse (sco_bridge.c); the mSBC encoder (msbc.c; the decoder is
# sbc_decode.c's).
SRCS+=		hfp.c sco_bridge.c msbc.c
# Firmware download for bootloader-mode controllers (#202): the device half
# (firmware.c), the id tables (fw_ids.c) and the iwmbtfw / rtlbtfw ports.
SRCS+=		firmware.c fw_ids.c fw_intel.c fw_rtl.c
# CoreFoundation modules: the pairing store and policy (btstore.c), the
# secrets (keystore.c), the SCDynamicStore publication (btstate.c), and their
# shared helpers (btcf.c).
SRCS+=		btcf.c btstore.c keystore.c btstate.c
# SDP: the server on L2CAP PSM 1 with the local records, the client that reads
# a peer's HID, PnP and audio records at pair time, and their shared wire format.
SRCS+=		sdp_proto.c sdp_server.c sdp_client.c
# Diagnostics for bluetooth(8) info / hci / ping (#211): controller details,
# node statistics and links, raw HCI commands and the L2CAP echo.
SRCS+=		diag.c

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

# <IOKit/kext/KextManager.h> (installed into the sysroot by the libIOKit
# step) needs <libkern/OSReturn.h>; the compat copy is searched last, so the
# sysroot's own wins. Same as libIOKit's IOHIDUserDevice.c.
CFLAGS+=	-idirafter ${.CURDIR}/../kext_tools/compat

# <servers/bootstrap.h> from liblaunch, with its Apple shim headers.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
# <notify.h> includes os/base.h from the libnotify shims, as ipconfigd's.
CFLAGS+=	-I${.CURDIR}/../Libnotify/freebsd-shims

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
#                     IOBluetoothHostController arrivals and departures;
#                     IOHIDUserDevice for the Classic HID host
#                     (hid_host.c) and LE keyboards and mice (hogp.c)
#   -lCoreFoundation  the dictionaries both of the above take
#   -lsystem_dispatch, -l:libsystem_blocks.so  the serial queue the IOKit
#                     callbacks run on, and what the SC header pulls
#   -lnotify          notify_post(org.nextbsd.bluetooth.*) after a change
#                     to State:/Bluetooth
#   (libIOKit also carries KextManagerLoadKextWithIdentifier: the upper
#   Bluetooth kexts and NgUbt are loaded through kextd.)
#
# NOT -ldispatch and NOT -lBlocksRuntime: those names belong to Gershwin's
# separate copies, and linking them caused the rtld collision of #378.
LDADD+=		-llaunch -lsystem_kernel
LDADD+=		-lSystemConfiguration -lIOKit -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lnotify

# Daemons go in /usr/sbin; /System/Library holds data only.
BINDIR=		/usr/sbin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD.
#   netgraph  libnetgraph: the control socket the graph is built through
#   usb       libusb: firmware download over the ugen node, as iwmbtfw and
#             rtlbtfw do
#   crypto    libcrypto: AES-128, AES-CMAC and P-256 ECDH for LE pairing
#   m         cosf: the SBC decoder's synthesis tables (sbc_decode.c), and
#             the mSBC encoder's and the SCO bridge's filters
#   cuse      libcuse: the SCO bridge's /dev/btscoN (sco_bridge.c), as
#             virtual_oss serves its own devices
LIBADD+=	pthread netgraph usb crypto m cuse
.include <bsd.prog.mk>
