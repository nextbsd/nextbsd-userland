# pam_launchd — Apple's launchd PAM session module (pam_modules-126, the
# OS X 10.9 / launchd-842 era), built for NextBSD's OpenPAM
# (nextbsd/nextbsd-userland#354). See VENDORED.md.
#
# Built like the PAM modules inherited from FreeBSD: one shared object, no
# static library, no profile build, installed as /usr/lib/pam_launchd.so.6
# (SHLIB_MAJOR 6, the OpenPAM module ABI) plus the pam_launchd.so symlink
# bsd.lib.mk adds for any *.so.N name, the same pair every base module has.
#
# Build:   make SYSROOT=/path/to/base MIGOUT=/path/to/mig-out
# Install: make DESTDIR=/path/to/stage install
#
# MIGOUT holds pam_launchd_probe.h and pam_launchd_probeUser.c, the client
# stub mig generates from pam_launchd_probe.defs (build-userland.sh runs it).

LIB=		pam_launchd
SHLIB_MAJOR=	6
SHLIB_NAME=	${LIB}.so.${SHLIB_MAJOR}
LIBDIR=		/usr/lib
MK_INSTALLLIB=	no
MK_PROFILE=	no
MAN=		pam_launchd.8

SRCS=		pam_launchd.c

MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		pam_launchd_probeUser.c
CFLAGS+=	-I${MIGOUT}			# pam_launchd_probe.h
.endif

# Vendored Apple source; don't gate on its warnings.
WARNS?=		0

# vproc.h, vproc_priv.h, bootstrap_priv.h, servers/bootstrap.h come from
# liblaunch's own tree, as launchctl gets them; mach/* from libmach.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../libmach/include
# <AvailabilityMacros.h> (pulled in by liblaunch's bootstrap.h) from launchd's
# shims. -idirafter, not -I, so a shim never shadows a real system header.
CFLAGS+=	-idirafter ${.CURDIR}/../launchd/freebsd-shims
# launchd-842's private vproc/bootstrap surface.
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks			# vproc_priv.h includes <dispatch/dispatch.h>

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
CFLAGS+=	-I${SYSROOT}/usr/include	# security/ (OpenPAM), dispatch/, uuid/, launch.h
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
# liblaunch and libsystem_kernel carry their own undefined references that
# resolve at run time (as for launchctl).
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# liblaunch: bootstrap_*, _vprocmgr_switch_to_session, _vproc_post_fork_ping,
# the bootstrap_port global. libsystem_kernel: mach_msg, mach_port_*,
# task_set_special_port, mig_get_reply_port and the rest of the MIG runtime.
LDADD+=		-llaunch -lsystem_kernel
# openpam_get_option, openpam_log, pam_get_item, pam_getenv.
LIBADD+=	pam

.include <bsd.lib.mk>
