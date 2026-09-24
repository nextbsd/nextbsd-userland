# passwd(1) — change a password, in the DirectoryServices plists or in
# master.passwd (nextbsd/nextbsd-userland#255, E18 U9).
#
# Setuid root. Unlike Darwin's, which is 0755 because it hands the write to
# opendirectoryd over IPC, ours has no daemon to hand it to: the plists are
# 0644 owned by root, so a user can read their own hash and cannot rewrite
# the file. Every authorisation decision uses the real uid; the effective uid
# matters only for the write.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		passwd
MAN=		passwd.1
BINDIR=		/usr/bin
BINMODE=	4555
BINOWN=		root
BINGRP=		wheel

LIBDSDIR=	${.CURDIR}/../../libds
COMMONDIR=	${.CURDIR}/../common
.PATH:		${LIBDSDIR} ${COMMONDIR}
SRCS=		passwd.c acct.c libds.c
CFLAGS+=	-I${LIBDSDIR} -I${COMMONDIR}

# WARNS=0 then re-add what we want: at WARNS>0 FreeBSD adds -Wsystem-headers
# -Werror, which turns warnings inside the vendored CoreFoundation public
# headers into hard errors. The carry every CF consumer here uses.
WARNS=		0
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith
CFLAGS+=	-O2 -pipe -fblocks

CFLAGS+=	-I${.CURDIR}/../../Libnotify/freebsd-shims

# libutil for the pw_* family, which is the only correct way to edit
# master.passwd here: it locks, writes through a temporary file and runs
# pwd_mkdb, so /etc/pwd.db and /etc/spwd.db are regenerated. Editing the text
# file alone would leave libc answering from a stale database.
# libcrypt for crypt(3); it is in libc on Darwin and separate here.
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
