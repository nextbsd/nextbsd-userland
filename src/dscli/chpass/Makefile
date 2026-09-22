# chpass(1), chfn(1), chsh(1) for NextBSD: -f and -s on a directory user's
# record in Users.plist; FreeBSD's chpass (at /usr/libexec/bsd/chpass) for
# root and the system users (E18 U9, nextbsd/nextbsd-userland#255).
# Setuid root like FreeBSD's, so a user can edit their own record.

PROG=		chpass
MAN=		chpass.1
SRCS=		chpass.c
BINDIR=		/usr/bin
BINOWN=		root
BINMODE=	4555
LINKS=		${BINDIR}/chpass ${BINDIR}/chfn \
		${BINDIR}/chpass ${BINDIR}/chsh \
		${BINDIR}/chpass ${BINDIR}/ypchpass \
		${BINDIR}/chpass ${BINDIR}/ypchfn \
		${BINDIR}/chpass ${BINDIR}/ypchsh
MLINKS=		chpass.1 chfn.1 chpass.1 chsh.1 \
		chpass.1 ypchpass.1 chpass.1 ypchfn.1 chpass.1 ypchsh.1

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
