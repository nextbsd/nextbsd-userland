# caffeinate(8) — prevent the system from sleeping on behalf of a utility.
#
# Vendored from apple-oss-distributions/PowerManagement @ PowerManagement-1049
# (caffeinate/caffeinate.c, caffeinate.8).
#
# Builds /usr/bin/caffeinate. Links libIOKit (IOPMAssertion*), libCoreFoundation,
# and libdispatch.
#
# Build:   cd src/caffeinate && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		caffeinate
MAN=		caffeinate.8
SRCS=		caffeinate.c

WARNS=		0

CFLAGS+=	-O2 -pipe -fblocks

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# Local IOKit headers before sysroot if building directly in tree
CFLAGS+=	-I${.CURDIR}/../libIOKit

LDADD+=		-lIOKit -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel

BINDIR=		/usr/bin

LIBADD+=	pthread
.include <bsd.prog.mk>
