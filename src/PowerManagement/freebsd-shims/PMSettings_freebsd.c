/*
 * PMSettings_freebsd.c — builds Apple's pmconfigd/PMSettings.m as C.
 *
 * PMSettings.m is plain C in an Objective-C file: on Intel Macs its only
 * Objective-C is the Apple silicon Lockdown Mode check, which its
 * __FreeBSD__ blocks leave out with the other frameworks NextBSD lacks.
 * NextBSD's base has no Objective-C runtime, and bsd.prog.mk would compile
 * a .m source as Objective-C, so this file includes it as C instead.
 *
 * When pmconfigd's Objective-C sources build, PMSettings.m goes into SRCS
 * directly and this file is deleted.
 */
#include "../pmconfigd/PMSettings.m"
