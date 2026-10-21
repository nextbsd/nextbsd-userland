# launchctl — Apple's launchd user-side control utility, ported.
#
# Vendored from apple-oss-distributions/launchd @ launchd-842.92.1.
# Single 4549-LOC translation unit (launchctl.c) plus shims under
# ../freebsd-shims for macOS-specific headers (IOKit, NSSystemDirectories,
# mach-o/getsect, libinfo, asl, os, ...).
#
# Install path: /bin/launchctl, matching Apple's shipping layout
# (/bin/launchctl on macOS too).
#
# Build:   cd src/launchd/support && make
# Install: make DESTDIR=/path/to/rootfs install
#
# Depends at build time on:
#   - liblaunch (built by the prior step, installed in SYSROOT)
#   - libCoreFoundation + lib_FoundationICU (ICU.x epic, installed in SYSROOT)
#   - libxpc / libdispatch / libsystem_kernel (installed in SYSROOT)
#   - the freebsd-shims headers in ../freebsd-shims
#   - launchd's own liblaunch/ public headers (launch.h, vproc.h, ...)

PROG=		launchctl
# Man page vendored from apple-oss-distributions/launchd @ launchd-842.92.1
# man/launchctl.1 (unmodified mdoc). bsd.prog.mk gzips + installs it.
MAN=		launchctl.1

SRCS=		launchctl.c

# Vendored Apple source — don't gate on its warnings.
WARNS?=		0

CFLAGS+=	-I${.CURDIR}/../liblaunch	# launch.h, vproc.h, bootstrap.h, ...
CFLAGS+=	-I${.CURDIR}/../src		# config.h shared with the daemon
CFLAGS+=	-I${.CURDIR}/../freebsd-shims	# IOKit/, mach-o/, NSSystemDirectories.h, ...
CFLAGS+=	-I${.CURDIR}/../../libmach/include	# mach/*
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include	# CoreFoundation/, _foundation_unicode/, dispatch/, xpc/, ...
# libedit's readline-compatible API. FreeBSD base ships libedit with a
# <readline/readline.h> header at /usr/include/edit/readline/readline.h.
# launchctl.c uses readline only for the interactive `launchctl` REPL;
# linking libedit's compat layer satisfies the symbols at link time.
CFLAGS+=	-I${SYSROOT}/usr/include/edit
LDADD+=		-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
# The vproc_mig_* client stubs that liblaunch's libvproc.c calls are
# no longer unresolved: liblaunch/Makefile links the MIG-generated
# jobUser.c into liblaunch.so. --allow-shlib-undefined is kept so
# launchctl's link matches the launchd daemon's permissive default for
# undefined symbols in the shared libraries it links against.
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# launchd-842 expects these Apple build-time defines.
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1

# Force-include FreeBSD compat shim that defines macOS-only
# identifiers launchctl.c references (_PATH_UTMPX, KERN_SAFEBOOT/
# NETBOOT/OSVERSION, SO_EXECPATH). Each define is documented in
# the header. Avoids in-source #ifdef sprinkles.
CFLAGS+=	-include ${.CURDIR}/../freebsd-shims/launchctl_freebsd_compat.h
# CF + ICU consumer flags. MUST match what libicu and libCoreFoundation
# were built with — see icu_complete_corefoundation_ok memory entry:
#   __HAS_APPLE_ICU__=1   activate Apple-private ICU code paths
#   U_DISABLE_RENAMING=1  match libicu's symbol export scheme
#   APPLE_ICU_CHANGES=1   expose Apple-private ICU function decls
CFLAGS+=	-D__HAS_APPLE_ICU__=1
CFLAGS+=	-DU_DISABLE_RENAMING=1
CFLAGS+=	-DAPPLE_ICU_CHANGES=1
CFLAGS+=	-fblocks			# CF + libdispatch block syntax
CFLAGS+=	-fconstant-cfstrings		# CFSTR() literal -> CFStringRef

# launchctl's link line. -lCoreFoundation pulls in the CF runtime.
# -l_FoundationICU is the swift-foundation-icu unified library
# (libicucore.so symlink would also work).
LDADD+=		-llaunch -lxpc -lsystem_dispatch -lsystem_kernel \
		-lCoreFoundation -l_FoundationICU \
	 -l:libsystem_blocks.so \
			# readline-compat layer for the interactive REPL

BINDIR=		/bin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
LIBADD+=	pthread util edit
.include <bsd.prog.mk>
