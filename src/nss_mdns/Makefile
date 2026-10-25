# nss_mdns — the `mdns` source for hosts in nsswitch.conf(5): names under
# local. (and IPv4 link-local reverse lookups) answered by mDNSResponder
# through dns_sd(3), the way Libinfo's mdns module does on macOS
# (nextbsd/nextbsd-userland#461, #435).
#
# Built like nss_directory_services: one shared object, no static library,
# no profile build, installed as /usr/lib/nss_mdns.so.1, the name libc's
# nsdispatch(3) dlopens for a source called mdns. It links libc and
# libdns_sd only, since it is loaded into every process that resolves a
# host name.
#
# Build:   make SYSROOT=/path/to/base   (after libdns_sd is installed there)
# Install: make DESTDIR=/path/to/stage install

LIB=		nss_mdns
SHLIB_MAJOR=	1
SHLIB_NAME=	${LIB}.so.${SHLIB_MAJOR}
LIBDIR=		/usr/lib
MK_INSTALLLIB=	no
MK_PROFILE=	no
MAN=

SRCS=		nss_mdns.c

WARNS?=		6

# <dns_sd.h> is installed by src/mDNSResponder/libdns_sd; the source-tree
# copy keeps a plain `make` working before that install.
CFLAGS+=	-I${.CURDIR}/../mDNSResponder/mDNSShared

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
.endif
LDFLAGS+=	-L${DESTDIR}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDADD+=		-ldns_sd

.include <bsd.lib.mk>
