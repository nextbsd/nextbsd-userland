# libffi -- Apple's libffi-40 (libffi 3.4.0-rc2), built for NextBSD
# (nextbsd/nextbsd-userland#443). See VENDORED.md.
#
# Apple's tree ships no generated configure and NextBSD's cross build
# has no autotools, so, as Apple does with darwin/include, the
# configuration is a hand-maintained header (nextbsd/fficonfig.h) and
# this bsd.lib.mk Makefile stands in for Makefile.am. The NextBSD
# patches in patches/ must be applied first; build-userland.sh builds a
# patched copy of this directory, never the vendored tree itself.
#
# Installs:
#   /usr/lib/libffi.so.7, libffi.so, libffi.a
#   /usr/include/ffi.h, ffitarget.h
#   /usr/libdata/pkgconfig/libffi.pc
#   /usr/share/man/man3/ffi*.3
#
# Build:   make SYSROOT=/path/to/base
# Install: make DESTDIR=/path/to/stage install

PACKAGE_VERSION=	3.4.0-rc2

LIB=		ffi
# libtool-version is 8:0:1 (CURRENT:REVISION:AGE), which libtool turns
# into the ELF soname libffi.so.7 (CURRENT - AGE).
SHLIB_MAJOR=	7
PREFIX?=	/usr
LIBDIR=		${PREFIX}/lib
MK_PROFILE=	no

.if ${MACHINE_CPUARCH} == "amd64"
FFI_TARGET=	X86_64
FFI_TARGETDIR=	x86
FFI_SRCS=	ffi64.c unix64.S ffiw64.c win64.S
.elif ${MACHINE_CPUARCH} == "aarch64"
FFI_TARGET=	AARCH64
FFI_TARGETDIR=	aarch64
FFI_SRCS=	ffi.c sysv.S
.else
.error libffi: NextBSD builds amd64 and arm64 only (MACHINE_CPUARCH=${MACHINE_CPUARCH})
.endif

.PATH:		${.CURDIR}/src ${.CURDIR}/src/${FFI_TARGETDIR} ${.CURDIR}/man

# Same set as Makefile.am's libffi_la_SOURCES plus configure.host's
# SOURCES for the target. tramp.c compiles to nothing without
# FFI_EXEC_STATIC_TRAMP but is kept, as upstream does.
SRCS=		ffi.h \
		prep_cif.c types.c raw_api.c java_raw_api.c closures.c \
		tramp.c \
		${FFI_SRCS}

CFLAGS+=	-I${.OBJDIR} \
		-I${.CURDIR}/nextbsd \
		-I${.CURDIR}/include \
		-I${.CURDIR}/src/${FFI_TARGETDIR}
CFLAGS+=	-fexceptions
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	--sysroot=${SYSROOT}
.endif

# Third-party code: build it the way upstream does, without -Werror.
WARNS?=		0
NO_WERROR=

# ffi.h, from include/ffi.h.in as configure would write it.
ffi.h: ffi.h.in
	sed -e 's/@VERSION@/${PACKAGE_VERSION}/' \
	    -e 's/@TARGET@/${FFI_TARGET}/' \
	    -e 's/@HAVE_LONG_DOUBLE@/1/' \
	    -e 's/@FFI_EXEC_TRAMPOLINE_TABLE@/0/' \
	    ${.ALLSRC} > ${.TARGET}
.PATH:		${.CURDIR}/include

# The symbol version script, preprocessed from libffi.map.in exactly as
# Makefile.am does (LIBFFI_BASE_8.0 and friends).
VERSION_MAP=	libffi.map
libffi.map: libffi.map.in
	${CC} ${CFLAGS} -D${FFI_TARGET} -DGENERATE_LIBFFI_MAP \
	    -E -P -x assembler-with-cpp -o ${.TARGET} ${.CURDIR}/libffi.map.in

# pkg-config(1) file; gnustep-base finds libffi through it.
libffi.pc: libffi.pc.in
	sed -e 's|@prefix@|${PREFIX}|' \
	    -e 's|@exec_prefix@|$${prefix}|' \
	    -e 's|@libdir@|$${exec_prefix}/lib|' \
	    -e 's|@toolexeclibdir@|$${libdir}|' \
	    -e 's|@includedir@|$${prefix}/include|' \
	    -e 's|@PACKAGE_NAME@|libffi|' \
	    -e 's|@PACKAGE_VERSION@|${PACKAGE_VERSION}|' \
	    ${.ALLSRC} > ${.TARGET}
.PATH:		${.CURDIR}

CLEANFILES+=	ffi.h libffi.map libffi.pc

INCS=		ffi.h ${.CURDIR}/src/${FFI_TARGETDIR}/ffitarget.h
INCSDIR=	${PREFIX}/include

FILES=		libffi.pc
FILESDIR=	${PREFIX}/libdata/pkgconfig

MAN=		ffi.3 ffi_call.3 ffi_prep_cif.3 ffi_prep_cif_var.3

.include <bsd.lib.mk>
