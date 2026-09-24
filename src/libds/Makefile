# libds — read and write the DirectoryServices plists, through
# CoreFoundation (nextbsd/nextbsd-userland#287, E18 U15).
#
# A static internal library. Nothing is installed: the account tools and
# the ds* commands link it in, and there is no ABI to keep. The header is
# installed so those tools (and out-of-tree consumers) can find it.
#
# This is deliberately NOT used by nss_directory_services, which links
# nothing but libc because it is dlopened into every process that calls
# getpwnam(3), and keeps its own read-only parser.
#
# Build: make SYSROOT=/path/to/base
# Tests: cd tests && make check   (host build, no root, no CF beyond the
#        system's own — runs on Darwin or FreeBSD)

LIB=		ds
INTERNALLIB=
MAN=
NO_MAN=		yes

SRCS=		libds.c
INCS=		libds.h
INCSDIR=	/usr/include

# WARNS=0 then re-add the warnings we want, the carry hostnamed,
# configd, IPConfiguration and libSystemConfiguration all use: at
# WARNS>0 FreeBSD adds -Wsystem-headers -Werror, which turns warnings
# inside the vendored CoreFoundation public headers into hard errors.
WARNS=		0
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# -fblocks: the CoreFoundation public headers declare block-typed APIs
# and will not compile without it.
CFLAGS+=	-O2 -pipe -fblocks

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
.endif

# os/base.h, which the CF umbrella reaches for, the same -I path
# notifyd, syslogd and hostnamed take.
CFLAGS+=	-I${.CURDIR}/../Libnotify/freebsd-shims

.include <bsd.lib.mk>
