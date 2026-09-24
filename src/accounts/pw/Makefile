# pw(8) — account and group maintenance (nextbsd/nextbsd-userland#255, E18 U9).
#
# Not setuid: pw is root's tool and has no self-service case.
#
# Anything belonging in master.passwd is handed to the base copy at
# the vendored pw under bsd/ with the original argv, which is how the packaging
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
BSDDIR=		${.CURDIR}/bsd
.PATH:		${LIBDSDIR} ${COMMONDIR} ${BSDDIR}
SRCS=		pw.c acct.c libds.c
CFLAGS+=	-I${LIBDSDIR} -I${COMMONDIR}

# The pw FreeBSD ships, vendored under bsd/ and entered as a function instead
# of exec'd as a second binary. Every account a port creates is a system
# account, so this half is the packaging contract: it has to behave exactly as
# it always has, which is best achieved by being the same code. bsd/README
# records the upstream revision and how to re-sync.
#
# pw_nis.c and pw_vpw.c come along because pw_user.c and pw_group.c reference
# them; NIS itself is unreachable, as nothing here configures it.
SRCS+=		bsd_main.c pw_conf.c pw_user.c pw_group.c pw_log.c \
		pw_nis.c pw_vpw.c grupd.c pwupd.c psdate.c bitmap.c \
		cpdir.c rm_r.c strtounum.c pw_utils.c
CFLAGS+=	-I${BSDDIR}

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
