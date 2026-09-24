# pw(8) — account and group maintenance (nextbsd/nextbsd-userland#255, E18 U9).
#
# Not setuid: pw is root's tool and has no self-service case.
#
# Anything belonging in master.passwd is handed to the base copy at
# /usr/libexec/bsd/pw with the original argv, which is how the packaging
# contract with ports is kept: every account a port creates is a system
# account, so it is served by the same binary that always served it.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		pw
MAN=		pw.8
BINDIR=		/usr/sbin

LIBDSDIR=	${.CURDIR}/../../libds
COMMONDIR=	${.CURDIR}/../common
.PATH:		${LIBDSDIR} ${COMMONDIR}
SRCS=		pw.c acct.c libds.c
CFLAGS+=	-I${LIBDSDIR} -I${COMMONDIR}

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
