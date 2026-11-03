# reboot(8), halt(8) -- Apple's, from system_cmds (nextbsd/nextbsd-userland#237).
#
# Vendored from apple-oss-distributions/system_cmds @ system_cmds-1042.120.1
# (reboot/reboot.c, reboot/reboot.8). Built with the file's own !__APPLE__
# code; reboot.c lists the NextBSD changes. reboot and halt hand the request
# to launchd(8), PID 1, with the BSD init(8) signal it honours.
#
# Plain libc. Builds /sbin/reboot, with /sbin/halt as a link.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		reboot
SRCS=		reboot.c
MAN=		reboot.8
MLINKS=		reboot.8 halt.8
LINKS=		${BINDIR}/reboot ${BINDIR}/halt
BINDIR=		/sbin
BINMODE=	0555
BINOWN=		root
BINGRP=		wheel

WARNS?=		6

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

.include <bsd.prog.mk>
