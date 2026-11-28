# sshd-mdns-register — Remote Login's Bonjour advertisements (_ssh._tcp and
# Server.app's _nextbsd-admin._tcp), alive exactly as long as sshd
# (E42, nextbsd/nextbsd-userland#693). Started by
# overlay/usr/libexec/sshd-keygen-wrapper, which referred to it already.
#
# Build:   cd src/sshd-mdns-register && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		sshd-mdns-register
MAN=
SRCS=		sshd-mdns-register.c
BINDIR=		/usr/libexec

WARNS=		0
CFLAGS+=	-O2 -pipe -Wall -Wextra

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif
LDFLAGS+=	-L/usr/lib/system -Wl,-rpath,/usr/lib/system
LDADD+=		-ldns_sd

.include <bsd.prog.mk>
