# shutdown(8), poweroff(8) -- Apple's, from system_cmds
# (nextbsd/nextbsd-userland#237).
#
# Vendored from apple-oss-distributions/system_cmds @ system_cmds-1042.120.1
# (shutdown/shutdown.c, shutdown/shutdown.8). Built with the file's own
# !__APPLE__ code, which signals launchd(8), PID 1, with the BSD init(8)
# protocol it honours; shutdown.c lists the NextBSD changes. Darwin's
# kextmanager.defs and pathnames.h belong to the __APPLE__ code and are not
# vendored.
#
# Plain libc. Builds /sbin/shutdown, with /sbin/poweroff as a link
# (`shutdown -p now`). Root only, as on Darwin: not setuid.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		shutdown
SRCS=		shutdown.c
MAN=		shutdown.8
MLINKS=		shutdown.8 poweroff.8
LINKS=		${BINDIR}/shutdown ${BINDIR}/poweroff
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
