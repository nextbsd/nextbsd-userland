# dscli — NextBSD's DirectoryServices tool: local users and groups in the
# plists nss_directory_services reads, and promote / demote / join / leave
# (nextbsd/nextbsd-userland#253, E18 U7).
#
# libds (../libds) is compiled in; libdns_sd (userland's mDNSResponder)
# is linked for `dscli join`'s Bonjour browse, so this builds after the
# mDNSResponder section of build-userland.sh has synced the sysroot.
#
# Build:   make SYSROOT=/path/to/sysroot
# Install: make DESTDIR=/path/to/stage install

PROG=		dscli
MAN=		dscli.8
SRCS=		dscli.c
BINDIR=		/usr/sbin

WARNS?=		6
CFLAGS+=	-O2 -pipe

.include "${.CURDIR}/../libds/libds.mk"

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system -L${SYSROOT}/usr/lib
.endif
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
LDADD+=		-ldns_sd

.include <bsd.prog.mk>
