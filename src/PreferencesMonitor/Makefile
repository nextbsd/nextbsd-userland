# PreferencesMonitor — org.nextbsd.PreferencesMonitor (#480).
#
# Sources:
#   prefsmon.c — first-boot "Automatic" set, default service for a new
#                interface, NetworkInterfaces.plist, and the flatten of the
#                current set's Network/ tree into Setup:.
#
# Darwin runs the same work as configd plugins and SCMonitor; NextBSD's
# configd has no plugin loader, so it is a small standalone daemon.
#
# Build:   make -C src/PreferencesMonitor SYSROOT=/path/to/rootfs
# Install: make -C src/PreferencesMonitor DESTDIR=/path/to/rootfs install

PROG=		PreferencesMonitor
SRCS=		prefsmon.c
# Suppress bsd.prog.mk's default man page, as hostnamed and ipconfigd do.
MAN=
BINDIR=		/usr/libexec

# WARNS=0: the CoreFoundation and SystemConfiguration public headers do not
# build under WARNS>0's -Wsystem-headers -Werror.
WARNS=		0
NO_MAN=		yes

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

LDADD+=		-lsystem_kernel
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
LIBADD+=	pthread

.include <bsd.prog.mk>
