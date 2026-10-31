# aslmanager — Apple's ASL log rotator. Phase J4 of the ASL port.
# Invoked on demand by launchd (org.nextbsd.aslmanager.plist) to age
# out /var/log/asl/* into Archive/. Installed at /usr/sbin/.

PROG=		aslmanager
# Man page(s) vendored from apple-oss-distributions/syslog @ 69d8586
# (aslmanager.tproj/). bsd.prog.mk gzips + installs into MANDIR.
MAN=		aslmanager.8
WARNS?=		0

SRCS=		aslmanager.c daemon.c

# MIG-generated asl_ipc.h (referenced via "asl_ipc.h" from daemon.c)
MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		asl_ipcUser.c
CFLAGS+=	-I${MIGOUT}
.endif

# Pull aslcommon sources (static dep — same pattern as syslogd)
.PATH:		${.CURDIR}/../aslcommon
SRCS+=		asl_common.c asl_memory.c

# Re-use notifyd's stub file — covers audit_token, sandbox,
# mig_*, voucher_*, vm_alloc/dealloc, _malloc_no_asl_log, copyfile
.PATH:		${.CURDIR}/../../Libnotify/freebsd-shims
SRCS+=		notifyd_stubs.c

# Headers
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/../aslcommon
CFLAGS+=	-I${.CURDIR}/../libsystem_asl.tproj/include
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch

# Apple build-time defines
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks
CFLAGS+=	-Wno-error=int-conversion -Wno-int-conversion

# Force-include compat shim (LOG_INSTALL, OSSpinLock, IOPOL_*, etc.)
CFLAGS+=	-include ${.CURDIR}/../freebsd-shims/libasl_freebsd_compat.h

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

LDADD+=		-lsystem_asl -lnotify -llaunch -lsystem_dispatch \
		-lsystem_kernel -l:libsystem_blocks.so

BINDIR=		/usr/sbin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
LIBADD+=	pthread bsm z
.include <bsd.prog.mk>
