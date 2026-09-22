# libds — the DirectoryServices account database for the tools that edit
# it. Include this from a bsd.prog.mk Makefile (dscli, and the passwd,
# chpass and pw wrappers of E18 U9) to compile the library's sources into
# the program: an object-level library, which keeps the cross build free
# of objdir-relative static-archive paths.
#
#   .include "${.CURDIR}/../libds/libds.mk"
#
# Needs only libc and libcrypt.

LIBDS_DIR:=	${.PARSEDIR}
.PATH:		${LIBDS_DIR}
CFLAGS+=	-I${LIBDS_DIR}
SRCS+=		dsrec.c dscrypt.c dsio.c dsnet.c dsroute.c plist.c
LIBADD+=	crypt
