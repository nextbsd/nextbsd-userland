# serveradmin(8) — what Server.app runs on each NextBSD computer, as root
# over SSH (E42, nextbsd/nextbsd#654; nextbsd/nextbsd-userland#690).
#
# Typed subcommands only, answers in JSON. Updates go through
# SettingsHelper's existing Update operations (root is an accepted
# caller), so its MIG client stubs are generated here from
# ../SettingsHelper/settingshelper.defs into MIGOUT.
#
# Build:   cd src/serveradmin && make SYSROOT=/path/to/rootfs MIGOUT=/path/to/migout
# Install: make DESTDIR=/path/to/rootfs install

PROG=		serveradmin
MAN=		serveradmin.8
SRCS=		serveradmin.c json.c run.c check.c status.c pair.c update.c accounts.c storage.c jail.c crash.c logs.c mdns.c
BINDIR=		/usr/sbin

WARNS=		0
CFLAGS+=	-O2 -pipe -std=gnu11
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes -Wno-unused-parameter
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith -Wno-macro-redefined

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif
LDFLAGS+=	-L/usr/lib/system -Wl,-rpath,/usr/lib/system

# The SettingsHelper client stubs, from its own .defs (MIGOUT holds them
# when build-userland.sh makes them; otherwise they're made here).
SHDIR=		${.CURDIR}/../SettingsHelper
MIGOUT?=	${.OBJDIR}
MIG?=		mig
SRCS+=		settingshelperUser.c
CLEANFILES+=	settingshelperUser.c settingshelperUser.h
CFLAGS+=	-I${MIGOUT} -I${SHDIR}
CFLAGS.settingshelperUser.c+= -w
.PATH:		${MIGOUT}
.if !exists(${MIGOUT}/settingshelperUser.c)
settingshelperUser.c settingshelperUser.h: ${SHDIR}/settingshelper.defs ${SHDIR}/settingshelper_types.h
	cd ${MIGOUT} && ${MIG} -I${SHDIR} -user settingshelperUser.c \
	    -header settingshelperUser.h -server /dev/null -sheader /dev/null \
	    ${SHDIR}/settingshelper.defs
.endif
update.o update.pieo: settingshelperUser.h

# libdns_sd for the Bonjour lookups; liblaunch and libsystem_kernel for
# the bootstrap look-up and the Mach calls to SettingsHelper; libjail.
LDADD+=		-ldns_sd -llaunch -lsystem_kernel -ljail

.include <bsd.prog.mk>
