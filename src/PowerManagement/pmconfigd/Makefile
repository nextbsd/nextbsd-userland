# pmconfigd — Apple's power management daemon (macOS ships it as powerd),
# ported to NextBSD.
#
# Builds /usr/sbin/pmconfigd; runs as the launchd job org.nextbsd.powerd
# (overlay/System/Library/LaunchDaemons/org.nextbsd.powerd.plist), vending
# Apple's two MachServices, com.apple.PowerManagement.control (MIG) and
# com.apple.iokit.powerdxpc (XPC).
#
# This Makefile is NextBSD's. Apple's drop shipped a Makefile here that
# predates the Xcode project (i386/ppc, files that no longer exist); the
# Xcode project's powerd target is the source of truth for what the daemon
# is made of, and SRCS below grows toward it. See ../VENDORED.md.
#
# The binary is pmconfigd, not powerd: /usr/sbin/powerd is still FreeBSD's
# CPU-frequency daemon until pmconfigd owns CPU policy (nextbsd-userland#231).
#
# Build:   make SYSROOT=/path/to/rootfs MIGOUT=/path/to/migout
# Install: make DESTDIR=/path/to/rootfs install

PROG=		pmconfigd
# NextBSD-original page: Apple ships no page for powerd.
MAN=		pmconfigd.8
BINDIR=		/usr/sbin

# NextBSD sources live in ../freebsd-shims, Apple's in this directory and
# ../common. As each Apple file is ported it joins SRCS and the matching part
# of freebsd-shims goes away:
#   pmconfigd_freebsd.c     main() and powerd_init() until pmconfigd.m and
#                           PrivateLib.m build
#   powermanagement_stubs.c the powermanagement.defs routines whose Apple
#                           files are not built yet
#   os_log_shim.c           os_log() for every file (os/log.h)
#   PrivateLib_freebsd.c    the PrivateLib.m functions the ported files call,
#                           and interim versions of the functions they call
#                           in Apple files not built yet (PrivateLib_freebsd.h)
#   RootDomain_freebsd.c    the kernel shim: IOServiceOpen/IOConnectCallMethod
#                           on the IOPMrootDomain as /dev/iopmroot ioctls,
#                           the root domain's general interest, and the lid,
#                           power-button and low-battery sleep policy
#   PMConnection_freebsd.c  PMConnection.m's sleep/wake state machine and
#                           the IOPMConnection routines, until PMConnection.m
#                           builds
#   RootDomainIdle_freebsd.c the idle sleep and display sleep timers xnu's
#                           IOPMrootDomain and IODisplayWrangler run in the
#                           kernel, until NextBSD's root domain has them
#   IOPMEnergyPrefs_freebsd.c IOKitUser's IOPMEnergyPrefs.c, read side: the
#                           Energy Saver defaults and saved settings, until
#                           libIOKit carries it (nextbsd-userland#233)
#   PMSettings_freebsd.c    PMSettings.m, compiled as C
#   PMStore_freebsd.c       Apple's PMStore.c, unmodified, compiled with the
#                           PrivateLib.h declarations it needs
#   BatteryTimeRemaining_freebsd.c
#                           the power-source core of BatteryTimeRemaining.m
#                           (IOPMPowerSource entries -> kIOPS dictionaries,
#                           notify state, the io_ps_* routines)
.PATH:		${.CURDIR}/../freebsd-shims ${.CURDIR}/../common
SRCS=		pmconfigd_freebsd.c
SRCS+=		powermanagement_stubs.c
SRCS+=		os_log_shim.c
SRCS+=		PrivateLib_freebsd.c
SRCS+=		RootDomain_freebsd.c
SRCS+=		PMConnection_freebsd.c
SRCS+=		RootDomainIdle_freebsd.c
SRCS+=		IOPMEnergyPrefs_freebsd.c

# Apple's files, built as C under __FreeBSD__ (see ../VENDORED.md):
#   AutoWakeScheduler.c     pmset schedule: one-shot power events
#   RepeatingAutoWake.c     pmset repeat: repeating power events
#   PMAssertions.c          IOPMAssertions: the assertion engine
#   PMAssertionLog.c        assertion logging (pmset -g log)
#   PMSettings.m            Energy Saver settings -> the root domain
#                           (via PMSettings_freebsd.c, as C)
#   TTYKeepAwake.c          active remote logins hold off idle sleep
SRCS+=		AutoWakeScheduler.c
SRCS+=		RepeatingAutoWake.c
SRCS+=		PMAssertions.c
SRCS+=		PMAssertionLog.c
SRCS+=		PMSettings_freebsd.c
SRCS+=		TTYKeepAwake.c

# Apple's assertion timeout and category lists, in powerd's bundle as the
# Xcode project installs them (initSystemAssertionTimeoutList(),
# initAssertionCategories()).
FILESGROUPS+=	PMRES
PMRES=		Resources/com.apple.powerd.assertiontimeouts.plist
PMRES+=		Resources/com.apple.powerd.assertioncategories.plist
PMRESDIR=	/System/Library/CoreServices/powerd.bundle
PMRESMODE=	0444

# The power-source core of BatteryTimeRemaining.m, and Apple's PMStore.c:
SRCS+=		PMStore_freebsd.c
SRCS+=		BatteryTimeRemaining_freebsd.c

# powermanagementServer.c — MIG server stub for powermanagement.defs.
# build-userland.sh runs the host mig and passes MIGOUT=; a standalone build
# without it fails at the generated #include "powermanagementServer.h", the
# same way wland / ipconfigd do.
#   -w on the generated TU: MIG output is not WARNS-clean.
#   -Wno-macro-redefined: the MIG stubs pull <mach/notify.h>, which redefines
#     the MACH_NOTIFY_* macros <mach/message.h> also defines (identical
#     values), the same benign clash wland and configd document.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:				${MIGOUT}
SRCS+=				powermanagementServer.c
CFLAGS+=			-I${MIGOUT}
CFLAGS.powermanagementServer.c+= -w
CFLAGS+=			-Wno-macro-redefined
.endif

# Vendored Apple source joins this build ticket by ticket; don't gate on its
# warnings (same as Libnotify, syslog and configd).
WARNS?=		0

CFLAGS+=	-O2 -pipe -fblocks

# Header search order matters:
#   1. ../freebsd-shims first, so <os/log.h>, <os/signpost.h> and
#      <IOKit/pwr_mgt/powermanagement*.h> resolve to pmconfigd's shims ahead
#      of launchd's freebsd-shims/os and the sysroot;
#   2. this directory and ../common for Apple's own headers;
#   3. libIOKit's source headers (<IOKit/IOKitLib.h>, the real one, and its
#      vendored <sys/iopmroot.h>, "iopmroot.h"), ahead of
#      launchd/freebsd-shims' IOKit stub, as wland and DiskArbitration do;
#   4. syslog's <asl.h> (PMAssertionLog.c logs assertions to ASL, as pmset
#      -g log reads them), ahead of launchd/freebsd-shims' empty asl.h;
#   5. liblaunch for <servers/bootstrap.h> and its launchd/freebsd-shims pair.
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR} -I${.CURDIR}/../common
CFLAGS+=	-I${.CURDIR}/../../libIOKit
CFLAGS+=	-I${.CURDIR}/../../syslog/libsystem_asl.tproj/include
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../../launchd/freebsd-shims
#   6. <notify.h> includes <os/base.h>, which only Libnotify's shims carry.
#      Searched after the sysroot, so it adds that header and shadows none.
CFLAGS+=	-idirafter ${.CURDIR}/../../Libnotify/freebsd-shims

# pmconfigd's MIG listener is Apple's: dispatch_mach_create_f() and
# dispatch_mach_mig_demux(), libdispatch SPI that NextBSD's libdispatch
# carries (notifyd uses the same pair). <dispatch/private.h> exposes it only
# with DISPATCH_MACH_SPI.
CFLAGS+=	-DDISPATCH_MACH_SPI=1

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

#   -llaunch          bootstrap_check_in + the bootstrap_port global
#   -lxpc             the com.apple.iokit.powerdxpc listener
#   -lsystem_dispatch the main queue and the dispatch_mach MIG channel
#   -lCoreFoundation  %@ in os_log_shim.c (and, later, everything)
#   -lsystem_kernel   mach_msg, vm_deallocate, the Mach port calls
#   -l:libsystem_blocks.so  the XPC event-handler blocks
#   -lSystemConfiguration  SCPreferences: com.apple.AutoWake.plist; PMStore's
#                     SCDynamicStore session
#   -lnotify          notify_post() of the scheduled-event notifications and
#                     the com.apple.system.powersources.* state
#   -lIOKit           the root domain: registry lookup, interest
#                     notifications, IOPMLib, /dev/iopmroot; the
#                     IOPMPowerSource notifications (BatteryTimeRemaining_freebsd.c)
#   -lsystem_asl      PMAssertionLog.c's ASL messages
# Use the system_* names and the exact-file -l: form for blocks, never
# -ldispatch / -lBlocksRuntime (#378).
LDADD+=		-llaunch -lxpc -lsystem_dispatch -lCoreFoundation
LDADD+=		-lsystem_kernel -l:libsystem_blocks.so
LDADD+=		-lSystemConfiguration -lnotify -lIOKit -lsystem_asl

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
#   m                 ceil() in AutoWakeScheduler.c (libSystem has it on macOS),
#                     lround() in BatteryTimeRemaining_freebsd.c
LIBADD+=	pthread m
.include <bsd.prog.mk>
