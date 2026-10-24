/*
 * fficonfig.h -- NextBSD's hand-maintained libffi configuration
 * (nextbsd/nextbsd-userland#443).
 *
 * Apple builds libffi from hand-maintained headers (darwin/include,
 * see darwin/README) rather than running configure, and so does
 * NextBSD: there is no generated configure in Apple's tree and the
 * cross build has no autotools. This file holds what configure.ac
 * would have found for the two NextBSD targets, amd64
 * (x86_64-unknown-freebsd) and arm64 (aarch64-unknown-freebsd), built
 * with clang and linked with lld. It is not installed.
 *
 * Keep it in step with configure.ac / acinclude.m4 when the vendored
 * source moves.
 */

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "NextBSD builds libffi for amd64 and arm64 only"
#endif

/* Define to the flags needed for the .section .eh_frame directive. */
#define EH_FRAME_FLAGS "a"

/* Define this if you want extra debugging. */
/* #undef FFI_DEBUG */

/* Static trampolines are Linux-only in this release (configure.ac). */
/* #undef FFI_EXEC_STATIC_TRAMP */

/* Apple's trampoline tables (vm_remap) are Mach-O only. */
/* #undef FFI_EXEC_TRAMPOLINE_TABLE */

/* PaX emulated trampolines: Linux only. */
/* #undef FFI_MMAP_EXEC_EMUTRAMP_PAX */

/* configure.ac sets this for *-*-freebsd*: closures come from libffi's
   own mmap-backed allocator, with a double-mapped temporary file as the
   fallback when a W|X mapping is refused. */
#define FFI_MMAP_EXEC_WRIT 1

/* Keep the deprecated ffi_prep_closure(): upstream exports it, and
   Apple's prep_cif.c only builds it when this is set. */
#define FFI_LEGACY_CLOSURE_API 1

/* Define this if you do not want support for the raw API. */
/* #undef FFI_NO_RAW_API */

/* Define this if you do not want support for aggregate types. */
/* #undef FFI_NO_STRUCTS */

/* Define to 1 if you have 'alloca', as a function or macro. */
#define HAVE_ALLOCA 1

/* There is no <alloca.h>; ffi_common.h falls back to __builtin_alloca. */
/* #undef HAVE_ALLOCA_H */

/* clang's integrated assembler understands .cfi_* directives. */
#define HAVE_AS_CFI_PSEUDO_OP 1

#if defined(__x86_64__)
/* Define if your assembler supports unwind section type. */
#define HAVE_AS_X86_64_UNWIND_SECTION_TYPE 1

/* Define if your assembler supports PC relative relocs. */
#define HAVE_AS_X86_PCREL 1
#endif

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define if __attribute__((visibility("hidden"))) is supported. */
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* long double is wider than double on both targets: x87 extended on
   amd64, IEEE binary128 on arm64. */
#define HAVE_LONG_DOUBLE 1

/* Define if you support more than one size of the long double type */
/* #undef HAVE_LONG_DOUBLE_VARIANT */

/* Define to 1 if you have the `memcpy' function. */
#define HAVE_MEMCPY 1

/* memfd_create(2) and MFD_CLOEXEC are in <sys/mman.h>. */
#define HAVE_MEMFD_CREATE 1

/* Define to 1 if you have the `mkostemp' function. */
#define HAVE_MKOSTEMP 1

/* Define to 1 if you have the `mkstemp' function. */
#define HAVE_MKSTEMP 1

/* Define to 1 if you have the `mmap' function. */
#define HAVE_MMAP 1

/* Define if mmap with MAP_ANON(YMOUS) works. */
#define HAVE_MMAP_ANON 1

/* Define if mmap of /dev/zero works. */
#define HAVE_MMAP_DEV_ZERO 1

/* Define if read-only mmap of a plain file works. */
#define HAVE_MMAP_FILE 1

/* Pointer authentication is arm64e (Apple) only. */
/* #undef HAVE_PTRAUTH */

/* Define if .eh_frame sections should be read-only. */
#define HAVE_RO_EH_FRAME 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdio.h> header file. */
#define HAVE_STDIO_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/memfd.h> header file. */
/* #undef HAVE_SYS_MEMFD_H */

/* Define to 1 if you have the <sys/mman.h> header file. */
#define HAVE_SYS_MMAN_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* The library is linked with the GNU-style version script libffi.map. */
#define LIBFFI_GNU_SYMBOL_VERSIONING 1

/* Name of package */
#define PACKAGE "libffi"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "http://github.com/libffi/libffi/issues"

/* Define to the full name of this package. */
#define PACKAGE_NAME "libffi"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "libffi 3.4.0-rc2"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "libffi"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "3.4.0-rc2"

/* The size of `double', as computed by sizeof. */
#define SIZEOF_DOUBLE 8

/* The size of `long double', as computed by sizeof. */
#define SIZEOF_LONG_DOUBLE 16

/* The size of `size_t', as computed by sizeof. */
#define SIZEOF_SIZE_T 8

/* Define to 1 if all of the C90 standard headers exist. */
#define STDC_HEADERS 1

/* ELF: symbols are not underscored. */
/* #undef SYMBOL_UNDERSCORE */

/* Define this if you are using Purify and want to suppress spurious messages. */
/* #undef USING_PURIFY */

/* Version number of package */
#define VERSION "3.4.0-rc2"

/* Both targets are little-endian. */
/* #undef WORDS_BIGENDIAN */

/* From configure.ac's AH_BOTTOM. */
#ifdef HAVE_HIDDEN_VISIBILITY_ATTRIBUTE
#ifdef LIBFFI_ASM
#ifdef __APPLE__
#define FFI_HIDDEN(name) .private_extern name
#else
#define FFI_HIDDEN(name) .hidden name
#endif
#else
#define FFI_HIDDEN __attribute__ ((visibility ("hidden")))
#endif
#else
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN
#endif
#endif
