# scutil(8) — command-line access to SCDynamicStore and SCPreferences.
#
# Vendored from apple-oss-distributions/configd @ configd-1405.120.5
# (scutil.tproj/), trimmed to what our libSystemConfiguration backs:
# the interactive dynamic-store session, -w, --prefs, --get / --set
# (ComputerName, HostName, LocalHostName) and --error. Each source file
# notes what it drops and why; the --net / --dns / --proxy / --nwi /
# --renew / -r sources (net*.c, nc.c, most of tests.c) are not carried.
#
#   scutil.c, commands.c, dictionary.c, session.c,
#   cache.c, notifications.c, prefs.c, tests.c	— Apple, trimmed
#   scutil_shim.c					— SCPrint + SC formatting
#							  (Apple SCD.c), which our
#							  libSystemConfiguration
#							  does not export
#
# Build:   cd src/scutil && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		scutil
# Man page vendored from the same tag (scutil.tproj/scutil.8), trimmed to
# the carried options. bsd.prog.mk gzips + installs into MANDIR.
MAN=		scutil.8
SRCS=		scutil.c commands.c dictionary.c session.c cache.c \
		notifications.c prefs.c tests.c scutil_shim.c

# WARNS=0: scutil pulls the CoreFoundation + SystemConfiguration public
# header tree. At WARNS>0 FreeBSD adds -Wsystem-headers -Werror, which
# promotes warnings inside the vendored CF headers into hard errors.
# Same carry hostnamed / caffeinate use.
WARNS=		0

# -fblocks: the CoreFoundation public headers declare block-typed APIs
# and refuse to compile without it.
CFLAGS+=	-O2 -pipe -fblocks

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# The CF umbrella wants the same shim availability macros hostnamed
# picks up from liblaunch / launchd-shims.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

# -lSystemConfiguration: SCDynamicStore* / SCPreferences* and the
# SCPreferencesSet* name setters (SCDHostName.c).
# -lCoreFoundation: CF object model, run loop (-w, n.watch) and the
# _CFStringCreateWithFormatAndArgumentsAux formatter SCPrint uses.
# -lsystem_dispatch -lsystem_blocks: pulled by the SC umbrella
# (SCDynamicStoreSetDispatchQueue, "-D").
# -lsystem_kernel: Mach syscall trampolines behind the SC MIG stubs.
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
LDADD+=		-lsystem_kernel

BINDIR=		/usr/sbin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
# edit: libedit (histedit.h) for the interactive prompt and history.
LIBADD+=	edit pthread
.include <bsd.prog.mk>
