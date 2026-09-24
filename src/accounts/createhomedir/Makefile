# createhomedir(8) -- build a user's home directory from the user template
# (E18 U10).
#
# The XML plist reader is shared with nss_directory_services rather than
# copied: both read the same kind of file, and a second copy would drift.
# When libds lands it absorbs plist.c and this .PATH goes away.

PROG=		createhomedir
MAN=		createhomedir.8
BINDIR=		/usr/sbin

SRCS=		createhomedir.c

WARNS?=		6
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

.include <bsd.prog.mk>
