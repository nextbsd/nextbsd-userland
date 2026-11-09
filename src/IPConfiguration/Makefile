# ipconfigd — freebsd-launchd-mach IPConfiguration daemon.
#
# Mach-IPC track port of Apple's IPConfiguration. iter 1 ships the
# daemon skeleton: getifaddrs interface enumeration +
# bootstrap_check_in(com.apple.IPConfiguration) + a sleep loop.
# Builds /usr/sbin/ipconfigd; runs as a launchd-managed daemon under
# the com.apple.IPConfiguration.plist in overlays/.
#
# Build:   cd src/IPConfiguration && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		ipconfigd
# Man page for the ipconfig CLI, vendored from apple-oss-distributions/bootp
# @ bootp-268.1 (ipconfig.tproj/ipconfig.8) and trimmed to the commands that
# exist. bsd.prog.mk gzips + installs into MANDIR.
MAN=		ipconfig.8
SRCS=		ipconfigd.c dhcp_discover.c apply_lease.c
SRCS+=		sc_publish.c lease_loop.c
SRCS+=		bound_state.c mach_service.c ipconfigd_mig.c
SRCS+=		arp_probe.c linklocal.c
SRCS+=		ra_listen.c apply_lease_v6.c ra_router.c v6_service.c
SRCS+=		sc_link_watch.c
SRCS+=		ipconfig_prefs.c
SRCS+=		dns_config.c dns_configuration.c

# The dnsinfo server: configd-596.15 dnsinfo/ and libSystemConfiguration/.
# Apple builds these into configd's IPMonitor plugin; here ipconfigd owns the
# election. The client half (dnsinfo_copy.c) is in libSystemConfiguration.
.PATH:		${.CURDIR}/../libSystemConfiguration/dnsinfo
.PATH:		${.CURDIR}/../libSystemConfiguration/libSC_info
SRCS+=		dnsinfo_create.c dnsinfo_server.c
SRCS+=		libSystemConfiguration_server.c
CFLAGS+=	-I${.CURDIR}/../libSystemConfiguration/dnsinfo
CFLAGS+=	-I${.CURDIR}/../libSystemConfiguration/libSC_info

# Apple's DHCP option list and tag table, bootp-268.1 bootplib, trimmed
# (vendored/): answers ipconfig getoption from the saved packet. The
# ipconfig CLI links these too; see build-userland.sh.
.PATH:		${.CURDIR}/vendored
SRCS+=		dhcp_options.c ptrlist.c DNSNameList.c
CFLAGS+=	-I${.CURDIR}/vendored

# ipconfigServer.c — MIG-generated server stub for ipconfig.defs
# (iter 5a's two read-only routines). build.sh runs mig and passes
# MIGOUT=; a bare standalone build without it fails at the generated
# #include "ipconfigServer.h", the same way liblaunch / hwregd do.
# -w on the generated TU: MIG output is not WARNS-clean and must
# not gate the build. -Wno-macro-redefined: the MIG stubs pull
# <mach/notify.h>, which redefines the MACH_NOTIFY_* macros
# <mach/message.h> also defines (identical values, whitespace
# differs) — same benign clash hwregd / configd document.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:				${MIGOUT}
SRCS+=				ipconfigServer.c
CFLAGS+=			-I${MIGOUT}
CFLAGS+=			-I${.CURDIR}	# generated stubs #include "ipconfig_mig_types.h"
CFLAGS.ipconfigServer.c+=	-w
CFLAGS+=			-Wno-macro-redefined
.endif

# WARNS=0: sc_publish.c pulls the CoreFoundation public header tree
# (CFRuntime.h, CFBundle.h, CFLocking.h ...). At WARNS>0 the FreeBSD
# warning machinery adds -Wsystem-headers -Werror which promotes
# warnings inside the vendored CF headers into hard errors. Same
# carry libSystemConfiguration / libxpc / libCoreFoundation use.
WARNS=		0

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# <servers/bootstrap.h> (bootstrap_check_in + bootstrap_port global)
# comes from liblaunch; its bootstrap.h pulls in Apple shim headers
# (AvailabilityMacros.h, ...) from launchd/freebsd-shims. Same -I
# pair hwregd / configd use.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
# <notify.h> includes os/base.h from the libnotify shims; hostnamed takes
# the same -I.
CFLAGS+=	-I${.CURDIR}/../Libnotify/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -llaunch: bootstrap_check_in + the bootstrap_port global.
# -lsystem_kernel: mach_msg + the Mach port syscalls behind it.
# -lSystemConfiguration: SCDynamicStoreSetValue — the configd publish
# that sc_publish.c wraps. -lCoreFoundation: CFDictionary / CFString
# / CFArray, the dictionary shape sc_publish.c builds for State:/...
# -lsystem_dispatch -lsystem_blocks: pulled transitively by the SC umbrella
# header (SCDynamicStoreSetDispatchQueue ABI surface).
LDADD+=		-llaunch -lsystem_kernel
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-lsystem_dispatch -l:libsystem_blocks.so
# -lnotify: notify_post(com.apple.system.config.network_change) after a
# primary election and a resolv.conf rewrite, as Darwin's IPMonitor does.
LDADD+=		-lnotify
# -lxpc: the dnsinfo XPC service (org.nextbsd.SystemConfiguration.DNSConfiguration).
LDADD+=		-lxpc

BINDIR=		/usr/sbin

# FreeBSD base libs: releng/15.1 src.libnames.mk requires LIBADD, not LDADD+=-lfoo.
# md: SHA-1 for the dnsinfo signature (CommonCrypto shim).
LIBADD+=	pthread md
.include <bsd.prog.mk>
