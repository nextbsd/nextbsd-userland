# adduser(8) — create a user account in the DirectoryServices plists
# (nextbsd/nextbsd-userland#286, E18 U14).
#
# NextBSD's own, at FreeBSD's path. libds reads and writes the plists
# through CoreFoundation; acct.c holds the rules more than one account tool
# needs, so they cannot drift between adduser, passwd and pw.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		adduser
MAN=		adduser.8
BINDIR=		/usr/sbin

LIBDSDIR=	${.CURDIR}/../../libds
COMMONDIR=	${.CURDIR}/../common
.PATH:		${LIBDSDIR} ${COMMONDIR}
SRCS=		adduser.c acct.c libds.c
CFLAGS+=	-I${LIBDSDIR} -I${COMMONDIR}

# WARNS=0 then re-add what we want, the carry every CoreFoundation consumer
# in this tree uses: at WARNS>0 FreeBSD adds -Wsystem-headers -Werror, which
# turns warnings inside the vendored CF public headers into hard errors.
WARNS=		0
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# -fblocks: the CoreFoundation public headers declare block-typed APIs.
CFLAGS+=	-O2 -pipe -fblocks

# os/base.h, which the CF umbrella reaches for.
CFLAGS+=	-I${.CURDIR}/../../Libnotify/freebsd-shims

# -lCoreFoundation for the property lists, via libds.
# libutil for login_cap: acct.c reads passwd_format from login.conf so the
# hashing policy is stated in one place rather than compiled in.
LDADD+=		-lCoreFoundation -lsystem_dispatch -lsystem_blocks
LDADD+=		-lsystem_kernel
LIBADD+=	util

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

.include <bsd.prog.mk>
