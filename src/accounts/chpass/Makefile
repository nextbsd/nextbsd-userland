# chpass(1), and chfn/chsh/ypch* as links to it
# (nextbsd/nextbsd-userland#255, E18 U9).
#
# Setuid root for the same reason passwd is: the plists are 0644 owned by
# root, so a user can read their own record and cannot rewrite it, and there
# is no daemon here to hand the write to. Authorisation is decided from the
# real uid. ci/assemble-image.sh re-applies the bit after its chown.
#
# The yp* names exist only to say NIS is unsupported, rather than leaving the
# shell to report a missing command that exists on every other BSD.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		chpass
MAN=		chpass.1
BINDIR=		/usr/bin
BINMODE=	4555
BINOWN=		root
BINGRP=		wheel

SYMLINKS=	chpass ${BINDIR}/chfn
SYMLINKS+=	chpass ${BINDIR}/chsh
SYMLINKS+=	chpass ${BINDIR}/ypchpass
SYMLINKS+=	chpass ${BINDIR}/ypchfn
SYMLINKS+=	chpass ${BINDIR}/ypchsh
MLINKS=		chpass.1 chfn.1
MLINKS+=	chpass.1 chsh.1
MLINKS+=	chpass.1 ypchpass.1
MLINKS+=	chpass.1 ypchfn.1
MLINKS+=	chpass.1 ypchsh.1

LIBDSDIR=	${.CURDIR}/../../libds
COMMONDIR=	${.CURDIR}/../common
.PATH:		${LIBDSDIR} ${COMMONDIR}
SRCS=		chpass.c acct.c libds.c
CFLAGS+=	-I${LIBDSDIR} -I${COMMONDIR}

# WARNS=0 then re-add what we want: the CoreFoundation public headers do not
# survive the -Wsystem-headers -Werror that WARNS>0 turns on.
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
