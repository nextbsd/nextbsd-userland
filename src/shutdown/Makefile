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
# Builds /sbin/shutdown, with /sbin/poweroff as a link (`shutdown -p now`).
# Root only, as on Darwin: not setuid. Links libIOKit for -s (sleep), which
# calls IOPMSleepSystem() as Darwin's does (nextbsd/nextbsd-userland#229).
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

# <IOKit/pwr_mgt/IOPMLib.h> pulls in the CoreFoundation headers, which are
# not WARNS-clean (CFLocking.h's #warning alone fails -Werror); caffeinate(8)
# builds at 0 for the same reason.
WARNS?=		0

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# libIOKit's source headers ahead of the sysroot's, as caffeinate(8) does;
# the CoreFoundation headers they include need blocks.
CFLAGS+=	-I${.CURDIR}/../libIOKit
CFLAGS+=	-fblocks

LDADD+=		-lIOKit

.include <bsd.prog.mk>
