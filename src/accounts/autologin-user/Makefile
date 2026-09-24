# autologin-user -- name the account the console logs in automatically
# (nextbsd/nextbsd-userland#278, E18 U11).
#
# Not a user-facing command: the getty job runs it, so it lives in
# /usr/libexec/nextbsd with the other job helpers and has no man page.
# The XML plist reader is shared with nss_directory_services rather than
# copied, so the two cannot drift; libds absorbs it later.
#
# The source is autologin.c, not autologin-user.c: the repository ignores
# *User.c for MIG-generated stubs, and on a case-insensitive filesystem that
# pattern swallows autologin-user.c without a word.

PROG=		autologin-user
MAN=
BINDIR=		/usr/libexec/nextbsd

NSSDIR=		${.CURDIR}/../../nss_directory_services
.PATH:		${NSSDIR}
SRCS=		autologin.c plist.c
CFLAGS+=	-I${NSSDIR}

WARNS?=		6
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

.include <bsd.prog.mk>
