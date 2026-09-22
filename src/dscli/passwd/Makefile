# passwd(1) for NextBSD: a directory user's passwordHash in Users.plist,
# or root's and the system users' hash in master.passwd (E18 U9,
# nextbsd/nextbsd-userland#255). Setuid root like FreeBSD's. yppasswd is
# a link that explains NIS is not supported.

PROG=		passwd
MAN=		passwd.1
SRCS=		passwd.c
BINDIR=		/usr/bin
BINOWN=		root
BINMODE=	4555
LINKS=		${BINDIR}/passwd ${BINDIR}/yppasswd
MLINKS=		passwd.1 yppasswd.1

WARNS?=		6
CFLAGS+=	-O2 -pipe

.include "${.CURDIR}/../libds/libds.mk"
.include "${.CURDIR}/../acct/acct.mk"

LIBADD+=	util

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib
.endif

.include <bsd.prog.mk>
