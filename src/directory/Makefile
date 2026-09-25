# dspromote(8), and dsdemote/dsjoin/dsleave/dsstatus as links to it
# (nextbsd/nextbsd-userland#253, E18 U7).
#
# One binary behind five names, dispatching on argv[0], the way chpass(1)
# carries chfn and chsh. Plain libc: it writes small plists by hand and moves
# nothing, and the one value it reads back -- the bound server --
# comes out of the binding plist with the same small reader acct.c uses. So
# this links neither libds nor CoreFoundation.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		dspromote
SRCS=		ds.c
MAN=		dspromote.8
BINDIR=		/usr/sbin
BINMODE=	0555
BINOWN=		root
BINGRP=		wheel

SYMLINKS=	dspromote ${BINDIR}/dsdemote
SYMLINKS+=	dspromote ${BINDIR}/dsjoin
SYMLINKS+=	dspromote ${BINDIR}/dsleave
SYMLINKS+=	dspromote ${BINDIR}/dsstatus
MLINKS=		dspromote.8 dsdemote.8
MLINKS+=	dspromote.8 dsjoin.8
MLINKS+=	dspromote.8 dsleave.8
MLINKS+=	dspromote.8 dsstatus.8

WARNS?=		6

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

.include <bsd.prog.mk>
