# rmuser(8) — remove a user account from the DirectoryServices plists
# (nextbsd/nextbsd-userland#255, E18 U9).
#
# Not setuid: removing an account is root's work, and there is no
# self-service case to support.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		rmuser
MAN=		rmuser.8
BINDIR=		/usr/sbin

LIBDSDIR=	${.CURDIR}/../../libds
COMMONDIR=	${.CURDIR}/../common
.PATH:		${LIBDSDIR} ${COMMONDIR}
SRCS=		rmuser.c acct.c libds.c
CFLAGS+=	-I${LIBDSDIR} -I${COMMONDIR}

# WARNS=0 then re-add what we want: the CoreFoundation public headers do not
# survive -Wsystem-headers -Werror, which WARNS>0 turns on.
WARNS=		0
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith
CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-I${.CURDIR}/../../Libnotify/freebsd-shims

LDADD+=		-lCoreFoundation -lsystem_dispatch -lsystem_blocks
LDADD+=		-lsystem_kernel
LIBADD+=	util crypt

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

.include <bsd.prog.mk>
