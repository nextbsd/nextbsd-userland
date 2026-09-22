# pw(8) for NextBSD: FreeBSD's command line, routed to the DirectoryServices
# plists for proper users and their groups and to FreeBSD's pw (kept at
# /usr/libexec/bsd/pw) for root, the system users and /etc/group (E18 U9,
# nextbsd/nextbsd-userland#255). Root only, like FreeBSD's.

PROG=		pw
MAN=		pw.8
SRCS=		pw.c
BINDIR=		/usr/sbin

WARNS?=		6
CFLAGS+=	-O2 -pipe

.include "${.CURDIR}/../libds/libds.mk"
.include "${.CURDIR}/../acct/acct.mk"

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib
.endif

.include <bsd.prog.mk>
