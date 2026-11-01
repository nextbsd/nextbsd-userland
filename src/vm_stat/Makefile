# vm_stat(1) -- Mach-style virtual memory statistics (nextbsd/nextbsd-userland#180).
#
# Plain libc. Darwin's vm_stat reads host_statistics64(); NextBSD has no
# Mach VM, so this reads FreeBSD's vm.stats.* sysctls and prints them in
# Darwin's format. See the mapping table at the top of vm_stat.c.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PROG=		vm_stat
SRCS=		vm_stat.c
MAN=
BINDIR=		/usr/bin
BINMODE=	0555
BINOWN=		root
BINGRP=		wheel

WARNS?=		6

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

.include <bsd.prog.mk>
