# sw_vers(1) — print the NextBSD product name, version and build
# (nextbsd/nextbsd#536).
#
# Ported from Apple's DarwinTools-1 (sw_vers.c, BSD-3-Clause, Finlay Dobbie),
# the sw_vers Darwin shipped through OS X 10.9; sw_vers.c lists the changes.
# It reads /System/Library/CoreServices/SystemVersion.plist, which nextbsd-pkg
# writes into NextBSD-everything. DarwinTools carries no man page, so
# sw_vers.1 is NextBSD's.
#
# Builds /usr/bin/sw_vers. Links libCoreFoundation only.
#
# Build:   cd src/sw_vers && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		sw_vers
MAN=		sw_vers.1
SRCS=		sw_vers.c

# WARNS=0: the CoreFoundation public header tree. At WARNS>0 FreeBSD adds
# -Wsystem-headers -Werror, which promotes warnings inside the vendored CF
# headers into hard errors. Same carry caffeinate / scutil use.
WARNS=		0

# -fblocks: the CoreFoundation public headers declare block-typed APIs and
# refuse to compile without it.
CFLAGS+=	-O2 -pipe -fblocks

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

LDADD+=		-lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel

BINDIR=		/usr/bin

LIBADD+=	pthread
.include <bsd.prog.mk>
