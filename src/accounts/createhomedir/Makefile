# createhomedir(8) -- build a user's home directory from the user template
# (nextbsd/nextbsd-userland#277, E18 U10).
#
# The XML plist reader is shared with nss_directory_services rather than
# copied: both read the same kind of file, and a second copy would drift.
# When libds lands it absorbs plist.c and this .PATH goes away.

PROG=		createhomedir
MAN=		createhomedir.8
BINDIR=		/usr/sbin

NSSDIR=		${.CURDIR}/../../nss_directory_services
.PATH:		${NSSDIR}
SRCS=		createhomedir.c plist.c
CFLAGS+=	-I${NSSDIR}

WARNS?=		6
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

.include <bsd.prog.mk>
