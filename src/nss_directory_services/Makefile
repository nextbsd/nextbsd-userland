# nss_directory_services — the `directory_services` source for passwd and
# group in nsswitch.conf(5): users and groups straight from the
# DirectoryServices plists (nextbsd/nextbsd-userland#249, E18 U3).
#
# Built like a PAM module: one shared object, no static library, no
# profile build, installed as /usr/lib/nss_directory_services.so.1, the
# name libc's nsdispatch(3) dlopens for a source called directory_services.
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install
# Tests:   cd tests && make check   (runs on the build host, no nsswitch)

LIB=		nss_directory_services
SHLIB_MAJOR=	1
SHLIB_NAME=	${LIB}.so.${SHLIB_MAJOR}
LIBDIR=		/usr/lib
MK_INSTALLLIB=	no
MK_PROFILE=	no
MAN=

SRCS=		nss_directory_services.c dsdb.c plist.c

WARNS?=		6
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

.include <bsd.lib.mk>
