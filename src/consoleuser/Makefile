# consoleuser(8) — publish the console user in the SCDynamicStore
# (State:/Users/ConsoleUser, nextbsd-userland#199).
#
# What Apple's loginwindow does with SCDynamicStoreSetConsoleInformation,
# as a small root-only tool a login program (Gershwin's LoginWindow) or
# an admin runs: "set <user>" after a login, "clear" after the logout.
# diskarbitrationd watches the key.
#
# Build:   cd src/consoleuser && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		consoleuser
MAN=		consoleuser.8
SRCS=		consoleuser.c

# WARNS=0: the CoreFoundation + SystemConfiguration header tree (same
# carry as scutil / hostnamed).
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

# -lSystemConfiguration: SCDynamicStoreSetConsoleInformation and
# SCDynamicStoreCopyConsoleUser (SCDConsoleUser.c).
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel

BINDIR=		/usr/libexec

LIBADD+=	pthread
.include <bsd.prog.mk>
