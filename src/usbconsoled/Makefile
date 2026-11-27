# usbconsoled(8) — gives the console user the USB devices no kernel driver
# claims (scanners, cameras: what programs drive with libusb), and gives
# them back to root at logout (nextbsd/nextbsd#717). Watches configd's
# State:/Users/ConsoleUser as diskarbitrationd does.
#
# Build:   cd src/usbconsoled && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		usbconsoled
MAN=		usbconsoled.8
SRCS=		usbconsoled.c

# WARNS=0: the CoreFoundation + SystemConfiguration header tree (same
# carry as consoleuser / scutil / hostnamed).
WARNS=		0

CFLAGS+=	-O2 -pipe -fblocks

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

# -lSystemConfiguration: SCDynamicStoreCopyConsoleUser and the
# notification key; -lsystem_dispatch: the queue and the timer.
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel

BINDIR=		/usr/libexec

# libusb20 (base libusb): which interfaces have a kernel driver.
LIBADD+=	usb pthread
.include <bsd.prog.mk>
