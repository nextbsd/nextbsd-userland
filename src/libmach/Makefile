# libsystem_kernel — userland C shim that turns the mach.ko-registered
# syscalls into named functions (mach_reply_port, mach_task_self, ...).
#
# Naming: matches Apple's libsystem_kernel.dylib on macOS so Apple-source
# code that links -lsystem_kernel works unchanged. See spike doc §3.
#
# Build:   cd src/libmach && make
# Install: make DESTDIR=/path/to/rootfs install
#   produces:
#     ${DESTDIR}/usr/lib/system/libsystem_kernel.so
#     ${DESTDIR}/usr/lib/system/libsystem_kernel.so.0
#     ${DESTDIR}/usr/include/mach/mach_traps.h
#     ${DESTDIR}/usr/include/mach/mach_port.h
#     ${DESTDIR}/usr/include/mach/message.h
#     ${DESTDIR}/usr/include/mach/task_special_ports.h
#     ${DESTDIR}/usr/include/mach/host_special_ports.h
#     ${DESTDIR}/usr/include/default_pager/default_pager_types.h
#     ${DESTDIR}/usr/libdata/pkgconfig/libsystem_kernel.pc

LIB=		system_kernel
SHLIB_MAJOR=	0
SRCS=		mach_traps.c \
		dispatch_kevent.c

# MIGOUT — directory holding the mig-generated MIG clients, produced by
# build-userland.sh from the kernel's .defs: mach_portUser.c
# (nextbsd-kernel#125), and taskUser.c, host_privUser.c and mach_hostUser.c
# (#84, #93). All four are generated with UserPrefix _kernelrpc_.
#
# Same shape configd and Libnotify use: .PATH plus a BARE filename. An
# absolute path in SRCS is rejected outright by bsd.dep.mk ("$OBJS absolute
# path not allowed"), which is why it goes through .PATH rather than being
# spelled ${MIGOUT}/mach_portUser.c.
#
# Optional, so a bare `make -C src/libmach` still works for header installs
# and quick builds; without it the _kernelrpc_* stubs are absent and the
# entry points in mach_traps.c keep their pre-MIG behaviour (see the
# LIBMACH_HAVE_MIG_CLIENT guard there).
#
# mig_client_types.h (private, not installed) supplies the types the task /
# host_priv / mach_host clients name that the public headers do not; it is
# force-included so the generated sources need no edits. See its header.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		mach_portUser.c \
		taskUser.c \
		host_privUser.c \
		mach_hostUser.c
CFLAGS+=	-I${MIGOUT} -DLIBMACH_HAVE_MIG_CLIENT=1 \
		-include ${.CURDIR}/mig_client_types.h
# Generated code: migcom walks reply messages with variable-size inline
# arrays by adding to a pointer_t, which is void * here (std_types.h), and
# -Wpointer-arith (WARNS >= 3) rejects that.  mach_portUser.c has no such
# routine; task and host_priv do.
CWARNFLAGS.taskUser.c+=		-Wno-pointer-arith
CWARNFLAGS.host_privUser.c+=	-Wno-pointer-arith
.endif

CFLAGS+=	-I${.CURDIR}/include
NO_MAN=		yes
WARNS?=		6

# backtrace()/backtrace_symbols_fd() for the task #39 Bug-B
# MACH_DEBUG_BADSEND diagnostic in mach_traps.c.
LIBADD+=	execinfo

# Task #39 update: <mach/mach.h> + <mach/clock.h> are now installed
# alongside the rest of the libmach headers. libdispatch's HAVE_MACH
# path (build.sh:380, -DHAVE_MACH:BOOL=ON) wants both: internal.h
# includes <mach/clock.h> and <mach/mach.h> unconditionally on the
# HAVE_MACH branch. Apple-internal types referenced from those
# headers (mach_voucher_t, etc.) are kept minimal or stubbed.
INCS=		include/mach/mach.h \
		include/mach/mach_traps.h \
		include/mach/mach_port.h \
		include/mach/message.h \
		include/mach/task_special_ports.h \
		include/mach/host_special_ports.h \
		include/mach/host_notify.h \
		include/mach/boolean.h \
		include/mach/kern_return.h \
		include/mach/std_types.h \
		include/mach/ndr.h \
		include/mach/machine.h \
		include/mach/vm_map.h \
		include/mach/vm_param.h \
		include/mach/error.h \
		include/mach/clock.h \
		include/mach/clock_types.h \
		include/mach/mach_vm.h \
		include/mach/sync_policy.h \
		include/mach/dispatch_kevent.h \
		include/mach/port.h \
		include/mach/notify.h \
		include/mach/mach_types.h \
		include/mach/mig_errors.h \
		include/mach/mig.h \
		include/mach/thread_status.h \
		include/mach/mach_error.h \
		include/mach/mach_interface.h \
		include/mach/host_info.h \
		include/mach/mach_host.h \
		include/mach/mach_time.h \
		include/mach/exception.h \
		include/mach/host_reboot.h \
		include/mach/task_policy.h \
		include/mach/std_types.defs \
		include/mach/mach_types.defs \
		include/mach/machine_types.defs
INCSGROUPS=	INCS MACHDEBUGINCS DEFPAGERINCS
MACHDEBUGINCS=	include/mach_debug/ipc_info.h
MACHDEBUGINCSDIR=	${PREFIX}/include/mach_debug
# <default_pager/default_pager_types.h>: the macx_triggers() flags swapd
# passes (nextbsd/nextbsd-userland#178), where Darwin installs them.
DEFPAGERINCS=	include/default_pager/default_pager_types.h
DEFPAGERINCSDIR=	${PREFIX}/include/default_pager
INCSDIR=	${PREFIX}/include/mach
LIBDIR=		${PREFIX}/lib/system

# pkg-config file: consumers do `pkg-config --libs libsystem_kernel`
# and get the right -L and -l flags for /usr/lib/system/.
FILES=		libsystem_kernel.pc
FILESDIR=	${PREFIX}/libdata/pkgconfig

PREFIX?=	/usr

.include <bsd.lib.mk>
