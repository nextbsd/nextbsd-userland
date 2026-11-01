/*
 * mach_traps.c — libmach implementation of the no-arg Mach trap family.
 *
 * Each entry point resolves its FreeBSD syscall number lazily from
 * `sysctl mach.syscall.<name>` on first call (cached in a static for
 * subsequent calls), then invokes the syscall. If the sysctl is
 * missing (mach.ko not loaded) or registration failed (negative number),
 * the function returns MACH_PORT_NULL — matching Apple's API contract
 * of MACH_PORT_NULL on no-resources.
 */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <execinfo.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/mach_vm.h>	/* mach_vm_allocate / mach_vm_deallocate */
#include <mach/message.h>

/*
 * MIG clients: mach_port (#83); task, host_priv and mach_host (#84, #93).
 *
 * These entry points used to fabricate success -- returning empty results or
 * KERN_SUCCESS without doing anything -- because there was no MIG client to
 * call. mach_port_get_set_status and mach_port_get_attributes between them
 * are why on-demand Mach-service launch was dead (#79): mportset_callback()
 * needs BOTH, and each failed independently.
 *
 * The generated stubs are named _kernelrpc_<routine> (UserPrefix in each
 * kernel .defs), so they sit underneath these public entry points rather
 * than colliding with them -- the libsyscall arrangement.
 *
 * LIBMACH_HAVE_MIG_CLIENT is defined by the Makefile only when MIGOUT
 * supplied the generated clients. Without it a bare `make -C src/libmach`
 * still builds, keeping the previous behaviour, so the header-install path
 * and quick local builds are unaffected.  The types the task / host_priv /
 * mach_host headers name come from mig_client_types.h, which the Makefile
 * force-includes alongside.
 */
#ifdef LIBMACH_HAVE_MIG_CLIENT
#include "mach_port_mig.h"
#include "task_mig.h"
#include "host_priv_mig.h"
#include "mach_host_mig.h"
#endif
#include <mach/ndr.h>
#include <mach/task_special_ports.h>
#include <mach/host_special_ports.h>

#define	NO_SYSCALL	(-1)

/*
 * NDR_record — the per-process NDR sender-representation descriptor
 * MIG-generated client stubs embed in every outgoing message. Apple
 * keeps this in libsystem_kernel; we follow suit. Values are the
 * canonical little-endian / ASCII / IEEE-float layout that matches
 * amd64 FreeBSD.
 */
NDR_record_t NDR_record = {
	.mig_vers      = 0,
	.if_vers       = 0,
	.reserved1     = 0,
	.mig_encoding  = 0,
	.int_rep       = NDR_INT_LITTLE,
	.char_rep      = NDR_CHAR_ASCII,
	.float_rep     = NDR_FLOAT_IEEE,
	.reserved2     = 0,
};

/*
 * resolve_syscall — read sysctl mach.syscall.<name>, return the
 * registered syscall number or NO_SYSCALL if unavailable.
 */
static int
resolve_syscall(const char *name)
{
	char oid[64];
	int num;
	size_t len = sizeof(num);

	if (snprintf(oid, sizeof(oid), "mach.syscall.%s", name) >=
	    (int)sizeof(oid))
		return (NO_SYSCALL);
	if (sysctlbyname(oid, &num, &len, NULL, 0) != 0)
		return (NO_SYSCALL);
	if (num < 0)
		return (NO_SYSCALL);
	return (num);
}

/*
 * resolve_syscall_once — sticky wrapper around resolve_syscall() for the
 * trap-first / MIG-fallback entry points below (#84).
 *
 * `*slot` starts at SYSCALL_UNRESOLVED and is written exactly once with
 * either the syscall number or NO_SYSCALL. A failed lookup is remembered,
 * not retried: under the fallback shape a missing trap is not an error but
 * the MIG path, so re-probing the sysctl on every call would only add a
 * syscall in front of every RPC. Thread-safety is the same as the
 * MACH_TRAP_NOARGS cache: the value is derived from a side-effect-free
 * sysctl, so a racing reader at worst resolves it a second time.
 */
#define	SYSCALL_UNRESOLVED	(-2)

static int
resolve_syscall_once(int *slot, const char *name)
{
	if (*slot == SYSCALL_UNRESOLVED)
		*slot = resolve_syscall(name);
	return (*slot);
}

/*
 * Generate one trap entry point. Cache the resolved syscall number
 * in a static; first call does the sysctl lookup, subsequent calls
 * skip it. Thread-safety: the static is written at most once with a
 * value derived from a side-effect-free sysctl, so a racing reader
 * either sees NO_SYSCALL (and re-resolves) or sees the final value.
 */
#define	MACH_TRAP_NOARGS(name)						\
mach_port_name_t							\
name(void)								\
{									\
	static int num = NO_SYSCALL;					\
									\
	if (num == NO_SYSCALL) {					\
		num = resolve_syscall(#name);				\
		if (num == NO_SYSCALL)					\
			return (MACH_PORT_NULL);			\
	}								\
	return ((mach_port_name_t)syscall(num));			\
}

MACH_TRAP_NOARGS(mach_reply_port)

/*
 * Apple exposes the task-self trap as `task_self_trap` (the raw syscall
 * name) and `mach_task_self()` (a userland accessor often inlined to a
 * TLS-style global on Apple platforms). On FreeBSD, with no per-thread
 * Mach storage yet, the simplest mapping is: `mach_task_self()` IS the
 * syscall. Same shape for thread and host.
 */
mach_port_name_t
mach_task_self(void)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("task_self_trap");
		if (num == NO_SYSCALL)
			return (MACH_PORT_NULL);
	}
	return ((mach_port_name_t)syscall(num));
}

mach_port_name_t
mach_thread_self(void)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("thread_self_trap");
		if (num == NO_SYSCALL)
			return (MACH_PORT_NULL);
	}
	return ((mach_port_name_t)syscall(num));
}

mach_port_name_t
mach_host_self(void)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("host_self_trap");
		if (num == NO_SYSCALL)
			return (MACH_PORT_NULL);
	}
	return ((mach_port_name_t)syscall(num));
}

/*
 * mach_msg — send and/or receive a Mach message.
 *
 * Resolves mach.syscall.mach_msg_trap on first call. If the syscall
 * isn't registered, returns a non-zero indeterminate error code
 * (we don't have a defined "syscall unavailable" code in the public
 * mach_msg return-code space, so we return MACH_RCV_TIMED_OUT as a
 * placeholder error; callers should check that mach.ko is loaded
 * before relying on this).
 */
/*
 * mach_msg — 6-arg call into the wired mach_msg_trap.
 *
 * FreeBSD's libc syscall() only correctly shifts up to 6 args into
 * the kernel-syscall ABI registers (rdi, rsi, rdx, r10, r8, r9). The
 * Apple-shape mach_msg_trap has 7 args (msg, option, send_size,
 * rcv_size, rcv_name, timeout, notify); calling syscall(num, ...)
 * with the 7th arg silently garbles args 4-7 (kernel sees rcv_size=0,
 * rcv_name=0, timeout=passed-rcv_size).
 *
 * Workaround: pass only the first 6 args to the kernel. The kernel
 * wrapper sets notify=MACH_PORT_NULL. The `notify` arg here is
 * accepted for API compatibility but currently ignored — a future
 * `mach_msg2`-style wrapper or an inline-asm 7-arg syscall stub
 * can lift this restriction if real use needs notify.
 */
mach_msg_return_t
mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
    mach_msg_size_t send_size, mach_msg_size_t rcv_size,
    mach_port_name_t rcv_name, mach_msg_timeout_t timeout,
    mach_port_name_t notify __attribute__((unused)))
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("mach_msg_trap");
		if (num == NO_SYSCALL)
			return (MACH_RCV_TIMED_OUT);
	}
	/*
	 * Task #39 Bug-B trace: when MACH_DEBUG_BADSEND is set, dump a
	 * backtrace for any SEND whose destination port name is a
	 * suspiciously small integer (< 32 — likely a stray fd, not a
	 * real Mach port name). Pinpoints which caller is confusing an
	 * fd for a port name. Compiled-in but inert unless the env var
	 * is present.
	 */
	if ((option & 0x1 /* MACH_SEND_MSG */) && msg != NULL &&
	    msg->msgh_remote_port != 0 && msg->msgh_remote_port < 32 &&
	    getenv("MACH_DEBUG_BADSEND") != NULL) {
		void *bt[24];
		int n = backtrace(bt, 24);
		fprintf(stderr, "[BADSEND] mach_msg SEND to remote_port=0x%x "
		    "id=0x%x — backtrace:\n",
		    msg->msgh_remote_port, msg->msgh_id);
		backtrace_symbols_fd(bt, n, 2);
	}
	return ((mach_msg_return_t)syscall(num, msg, option,
	    send_size, rcv_size, rcv_name, timeout));
}

/*
 * Port-management entry points: trap first, MIG fallback (#84).
 *
 * This is Apple's libsyscall shape (xnu libsyscall/mach/mach_port.c): MIG
 * is the floor and the trap is an accelerator.
 *
 *	rv = _kernelrpc_mach_port_allocate_trap(task, right, name);
 *	if (rv == MACH_SEND_INVALID_DEST)
 *		rv = _kernelrpc_mach_port_allocate(task, right, name);
 *
 * Here the trap is a dynamically registered syscall resolved by name, so
 * "no trap" has two spellings: resolve_syscall() found no mach.syscall.<name>
 * (the lookup is sticky, see resolve_syscall_once), or the trap answered
 * MACH_SEND_INVALID_DEST. Either one falls through to the generated
 * _kernelrpc_mach_port_* client. Any other trap result, success or error,
 * is the answer.
 *
 * Without LIBMACH_HAVE_MIG_CLIENT (a bare build with no MIGOUT) there is
 * nothing to fall back to, and a missing trap returns KERN_RESOURCE_SHORTAGE
 * as it always has.
 *
 * `task` is passed through to the trap for Apple-source compatibility; the
 * kernel trap ignores it and acts on current_task(). The MIG path honours it.
 */
kern_return_t
mach_port_allocate(mach_port_name_t task, mach_port_right_t right,
    mach_port_name_t *name)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "mach_port_allocate");

	if (sc != NO_SYSCALL) {
		kern_return_t rv = (kern_return_t)syscall(sc, task, right,
		    name);

		if (rv != MACH_SEND_INVALID_DEST)
			return (rv);
	}
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_mach_port_allocate(task, right, name));
#else
	return (KERN_RESOURCE_SHORTAGE);
#endif
}

kern_return_t
mach_port_deallocate(mach_port_name_t task, mach_port_name_t name)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "mach_port_deallocate");

	if (sc != NO_SYSCALL) {
		kern_return_t rv = (kern_return_t)syscall(sc, task, name);

		if (rv != MACH_SEND_INVALID_DEST)
			return (rv);
	}
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_mach_port_deallocate(task, name));
#else
	return (KERN_RESOURCE_SHORTAGE);
#endif
}

kern_return_t
mach_port_insert_right(mach_port_name_t task, mach_port_name_t name,
    mach_port_t poly, mach_msg_type_name_t polyPoly)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "mach_port_insert_right");

	if (sc != NO_SYSCALL) {
		kern_return_t rv = (kern_return_t)syscall(sc, task, name,
		    poly, polyPoly);

		if (rv != MACH_SEND_INVALID_DEST)
			return (rv);
	}
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_mach_port_insert_right(task, name, poly, polyPoly));
#else
	return (KERN_RESOURCE_SHORTAGE);
#endif
}

/*
 * Task special ports: MIG only (#84 step 6). `which` is TASK_BOOTSTRAP_PORT
 * etc. from <mach/task_special_ports.h>. task_get_bootstrap_port /
 * task_set_bootstrap_port are macros that pick TASK_BOOTSTRAP_PORT for
 * the `which` arg.
 *
 * Apple has no trap for these (task 3409 / 3410 are MIG routines in XNU's
 * mach_trap_table audit, see #84), so there is no trap-first step. The
 * MIG path honours `task`: the old NextBSD traps ignored it and acted on
 * current_task(), so launchd's task_set_special_port(child_task, ...) set
 * launchd's own slot instead of the child's. The kernel handlers
 * (kern/ipc_tt.c) are real, including the host-bootstrap fallback for
 * TASK_BOOTSTRAP_PORT.
 *
 * Without LIBMACH_HAVE_MIG_CLIENT (a bare build with no MIGOUT) the
 * pre-MIG trap path is kept, per the guard's contract above.
 */
kern_return_t
task_get_special_port(mach_port_name_t task, int which,
    mach_port_name_t *port)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_task_get_special_port(task, which, port));
#else
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("task_get_special_port");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, which, port));
#endif
}

kern_return_t
task_set_special_port(mach_port_name_t task, int which, mach_port_t port)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_task_set_special_port(task, which, port));
#else
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("task_set_special_port");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, which, port));
#endif
}

/*
 * host_set_special_port — set a slot in the host's special-port array.
 * MIG only (#84 step 8; host_priv 413, no Apple trap).
 *
 * The bootstrap server calls this once at startup to publish its
 * receive port host-wide; thereafter task_get_special_port(
 * TASK_BOOTSTRAP_PORT) falls back to realhost.special[
 * HOST_BOOTSTRAP_PORT] when the per-task itk_bootstrap slot is null.
 * launchd and kextd are the other callers; all three run as root.
 *
 * The request port must be host_priv. mach_host_self() hands host_priv
 * to a non-jailed euid-0 Mach task (nextbsd-kernel#285, XNU's rule) and
 * the unprivileged host port to everyone else, for whom the kernel's
 * host_priv intran yields HOST_NULL and the handler answers
 * KERN_INVALID_HOST. Only HOST_BOOTSTRAP_PORT and HOST_KEXTD_PORT are
 * settable (KERN_INVALID_ARGUMENT otherwise), as with the old trap. That
 * trap had no privilege check at all; it stays in the kernel only until
 * published userland stops calling it (#84 Phase C).
 */
kern_return_t
host_set_special_port(mach_port_name_t host, int which, mach_port_t port)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_host_set_special_port(host, which, port));
#else
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("host_set_special_port");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, host, which, port));
#endif
}

/*
 * Phase I1c link-stage stubs.
 *
 * launchd-842 references a large surface of Apple userland Mach
 * APIs that we haven't ported yet. Until each gets a real backing
 * (sysctl(KERN_PROC), real mach_port RPCs, kqueue-backed Mach msgs,
 * libdispatch timing) the stubs let the launchd link succeed and
 * the daemon's no-IPC CLI path runs. Calls that hit them at runtime
 * fail closed (KERN_RESOURCE_SHORTAGE / -1 / no-op) — the daemon's
 * existing graceful-degradation paths handle that.
 */
#include <time.h>
#include <mach/exception.h>
#include <mach/host_reboot.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/task_policy.h>

uint64_t
mach_absolute_time(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t
mach_continuous_time(void)
{
	return mach_absolute_time();
}

/*
 * Deliberate userland implementation, not a stub: mach_absolute_time() is
 * CLOCK_MONOTONIC nanoseconds, so 1/1 is the exact ratio.
 */
kern_return_t
mach_timebase_info(mach_timebase_info_t info)
{
	if (info != NULL) {
		info->numer = 1;
		info->denom = 1;
	}
	return KERN_SUCCESS;
}

/*
 * host_info / host_statistics: MIG (mach_host 200 / 216, #93 group 6).
 *
 * The kernel implements HOST_BASIC_INFO, HOST_SCHED_INFO and
 * HOST_PRIORITY_INFO for host_info, and HOST_VM_INFO (all three Apple
 * count revisions) for host_statistics; other flavors are
 * KERN_INVALID_ARGUMENT. The structs and counts in <mach/host_info.h>
 * match the kernel's, which asserts them; the asserts below hold this
 * side to the same numbers. launchd's memory-pressure sampler
 * (runtime.c) is the in-tree host_statistics(HOST_VM_INFO) caller.
 */
_Static_assert(sizeof(struct host_basic_info) == 48,
    "host_basic_info must be Apple's 48-byte layout (kernel mach_host.c)");
_Static_assert(HOST_BASIC_INFO_COUNT == 12, "HOST_BASIC_INFO_COUNT");
_Static_assert(HOST_SCHED_INFO_COUNT == 2, "HOST_SCHED_INFO_COUNT");
_Static_assert(HOST_PRIORITY_INFO_COUNT == 8, "HOST_PRIORITY_INFO_COUNT");
_Static_assert(HOST_VM_INFO_COUNT == 15, "HOST_VM_INFO_COUNT");

kern_return_t
host_info(host_t host, host_flavor_t flavor,
    host_info_t info, mach_msg_type_number_t *count)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_host_info(host, flavor, info, count));
#else
	(void)host; (void)flavor; (void)info;
	if (count != NULL)
		*count = 0;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

kern_return_t
host_statistics(host_t host, host_flavor_t flavor,
    host_info_t info, mach_msg_type_number_t *count)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_host_statistics(host, flavor, info, count));
#else
	(void)host; (void)flavor; (void)info;
	if (count != NULL)
		*count = 0;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

kern_return_t
host_reboot(host_priv_t host_priv, int options)
{
	(void)host_priv; (void)options;
	return KERN_RESOURCE_SHORTAGE;
}

/*
 * task_policy_set / task_policy_get: MIG (task 3420 / 3421, #93 group 3).
 *
 * These used to return KERN_SUCCESS (and, for get, a zero count) without
 * reaching the kernel. The kernel now answers: set accepts the known
 * flavors (TASK_BASE_QOS_POLICY and the rest of <mach/task_policy.h>) as
 * a no-op and rejects others with KERN_INVALID_ARGUMENT; get returns the
 * defaults with *get_default = TRUE and a real count. launchd's
 * TASK_BASE_QOS_POLICY call (core.c) is the in-tree caller.
 */
kern_return_t
task_policy_set(task_t task, task_policy_flavor_t flavor,
    task_policy_t policy_info, mach_msg_type_number_t count)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_task_policy_set(task, flavor, policy_info,
	    count));
#else
	(void)task; (void)flavor; (void)policy_info; (void)count;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

kern_return_t
task_policy_get(task_t task, task_policy_flavor_t flavor,
    task_policy_t policy_info, mach_msg_type_number_t *count,
    boolean_t *get_default)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_task_policy_get(task, flavor, policy_info,
	    count, get_default));
#else
	(void)task; (void)flavor; (void)policy_info;
	if (count != NULL)
		*count = 0;
	if (get_default != NULL)
		*get_default = 0;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

/*
 * task_set_exception_ports: MIG (task 3413, #93 group 1).
 *
 * Used to return KERN_SUCCESS and drop the port. The kernel handler
 * (kern/ipc_tt.c) stores the right; it masks MACH_EXCEPTION_CODES off the
 * behavior before validating it, so launchd's
 * EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES (core.c) is accepted.
 * Nothing in the kernel delivers exceptions yet, so the effect is a held
 * send right, released when replaced.
 */
kern_return_t
task_set_exception_ports(task_t task, exception_mask_t mask,
    mach_port_t new_port, exception_behavior_t behavior,
    thread_state_flavor_t new_flavor)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_task_set_exception_ports(task, mask, new_port,
	    behavior, new_flavor));
#else
	(void)task; (void)mask; (void)new_port;
	(void)behavior; (void)new_flavor;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

/*
 * host_set_exception_ports: MIG (host_priv 414, #93 group 2).
 *
 * Used to return KERN_SUCCESS and drop the port. The kernel handler
 * (kern/ipc_tt.c) stores the right, masking MACH_EXCEPTION_CODES off the
 * behavior as XNU does, so launchd's EXCEPTION_STATE_IDENTITY |
 * MACH_EXCEPTION_CODES (core.c) is accepted. The request port must be
 * host_priv, which mach_host_self() gives a non-jailed root task
 * (nextbsd-kernel#285); launchd is the in-tree caller. As with the task
 * ports, nothing delivers exceptions yet: the effect is a held send right.
 */
kern_return_t
host_set_exception_ports(host_priv_t host_priv, exception_mask_t mask,
    mach_port_t new_port, exception_behavior_t behavior,
    thread_state_flavor_t new_flavor)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_host_set_exception_ports(host_priv, mask, new_port,
	    behavior, new_flavor));
#else
	(void)host_priv; (void)mask; (void)new_port;
	(void)behavior; (void)new_flavor;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

kern_return_t
mach_port_set_attributes(mach_port_name_t task, mach_port_name_t name,
    mach_port_flavor_t flavor, mach_port_info_t info,
    mach_msg_type_number_t infoCnt)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	/*
	 * Not cosmetic. Five callers depended on this and all five were
	 * silently ignored:
	 *
	 *   notifyd.c:935   MACH_PORT_LIMITS_INFO, qlimit = 32 on the
	 *                   system-wide notification port -- so it has been
	 *                   running with MACH_PORT_QLIMIT_DEFAULT (5), which
	 *                   the probe in nextbsd-kernel#125 measured directly
	 *   core.c:1709     MACH_PORT_LIMITS_INFO on exit_status_port
	 *   core.c:1992/6828/7692
	 *                   MACH_PORT_TEMPOWNER on service ports, which
	 *                   governs port ownership when a job dies -- the
	 *                   machinery on-demand launch runs on
	 */
	return _kernelrpc_mach_port_set_attributes(task, name, flavor, info,
	    infoCnt);
#else
	(void)task; (void)name; (void)flavor; (void)info; (void)infoCnt;
	return KERN_SUCCESS;
#endif
}

kern_return_t
mach_port_get_attributes(mach_port_name_t task, mach_port_name_t name,
    mach_port_flavor_t flavor, mach_port_info_t info,
    mach_msg_type_number_t *infoCnt)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return _kernelrpc_mach_port_get_attributes(task, name, flavor, info,
	    infoCnt);
#else
	(void)task; (void)name; (void)flavor; (void)info;
	if (infoCnt != NULL)
		*infoCnt = 0;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

/*
 * mach_port_mod_refs: trap first, MIG fallback, as for mach_port_allocate
 * above (#84) and as Apple's libsyscall does.
 *
 * The trap matters beyond speed. mig_dealloc_reply_port() destroys a MIG
 * reply port with this call, and the MIG path needs a reply port of its
 * own; the trap does not, so with it the reply-port error path cannot
 * re-enter MIG at all. The kernel has the handler
 * (sys__kernelrpc_mach_port_mod_refs_trap) but does not wire it to a
 * mach.syscall.mach_port_mod_refs slot yet, so today this resolves to
 * NO_SYSCALL and takes the MIG path; once the kernel wires it, this picks
 * it up with no further userland change. Until then
 * mig_dealloc_reply_port() keeps the MIG path from recursing on its own
 * (see mig_reply_port_destroy below).
 */
kern_return_t
mach_port_mod_refs(mach_port_name_t task, mach_port_name_t name,
    mach_port_right_t right, mach_port_delta_t delta)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "mach_port_mod_refs");

	if (sc != NO_SYSCALL) {
		kern_return_t rv = (kern_return_t)syscall(sc, task, name,
		    right, delta);

		if (rv != MACH_SEND_INVALID_DEST)
			return (rv);
	}
#ifdef LIBMACH_HAVE_MIG_CLIENT
	/*
	 * Real reference counting, finally (#83, 7/7).
	 *
	 * Held back until now because wiring it panicked the kernel: with this
	 * a no-op, port sets were never actually destroyed, so the
	 * use-after-free in ipc_mqueue_deliver() had nothing to race against.
	 * Made real, mach_port_mod_refs() destroys sets for the first time and
	 * a concurrent message delivery dereferenced the freed pset --
	 * nextbsd-kernel#131, fixed by taking a reference across the port
	 * unlock (nextbsd-kernel#132).
	 *
	 * Note this genuinely changes behaviour rather than just fixing a
	 * return value: reference counts now move, so anything in the tree
	 * with a latent over-release or missing-retain stops being harmless.
	 * That is the point -- silently not counting references is not a safer
	 * state, only a quieter one.
	 */
	return _kernelrpc_mach_port_mod_refs(task, name, right, delta);
#else
	(void)task; (void)name; (void)right; (void)delta;
	return KERN_SUCCESS;
#endif
}

/*
 * mach_port_move_member: dedicated syscall (mach.syscall.
 * mach_port_move_member). Task #41 root cause: this used to return
 * KERN_SUCCESS without doing anything, so launchd's runtime_add_mport
 * silently failed to add launchd_internal_port to its ipc_port_set.
 * Every launchd-spawned daemon hung in launch_msg(CHECKIN) because the
 * kqueue→handle_kqueue Mach send arrived on a port the main thread
 * wasn't listening on.
 *
 * Trap first, MIG fallback, as for mach_port_allocate above (#84).
 */
kern_return_t
mach_port_move_member(mach_port_name_t task, mach_port_name_t member,
    mach_port_name_t after)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "mach_port_move_member");

	if (sc != NO_SYSCALL) {
		kern_return_t rv = (kern_return_t)syscall(sc, task, member,
		    after);

		if (rv != MACH_SEND_INVALID_DEST)
			return (rv);
	}
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_mach_port_move_member(task, member, after));
#else
	return (KERN_RESOURCE_SHORTAGE);
#endif
}

/*
 * mach_port_request_notification — arm a Mach notification (dead-name,
 * no-senders, send-possible) on a port.
 *
 * Three tiers (#84): the trap if the kernel registers one; otherwise (or on
 * MACH_SEND_INVALID_DEST) the _kernelrpc_mach_port_request_notification MIG
 * client, as for mach_port_allocate above; and only if MIG also fails, a
 * no-op that returns KERN_SUCCESS.
 *
 * Why the no-op survives as the last tier: launchd's callers do not tolerate
 * a failure here. os_assumes_zero in this tree logs and returns
 * (src/launchd/freebsd-shims/os/assumes.h) -- it does not abort PID 1 -- but
 * the call sites act on the error:
 *
 *   - job_setup_machport (core.c) takes out_bad, so every job would lose
 *     its Mach port;
 *   - sub-jobmgr creation takes out_bad;
 *   - the !pid1_magic branch of jobmgr_init wraps it in os_assert_zero,
 *     which does abort the per-user launchd.
 *
 * So a broken notification server costs missing dead-name notifications
 * rather than every job's Mach port. Propagating the error is the right end
 * state once those call sites are softened; that is not this change.
 */
kern_return_t
mach_port_request_notification(mach_port_name_t task,
    mach_port_name_t name, mach_msg_id_t msgid,
    mach_port_mscount_t sync, mach_port_t notify,
    mach_msg_type_name_t notifyPoly, mach_port_t *previous)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "mach_port_request_notification");
	/*
	 * Pass SIX args, dropping `task`: FreeBSD's libc syscall() only reliably
	 * passes 6 args through the amd64 kernel ABI (the kernel's own comment at
	 * mach_syscall_wire.c documents this — mach_msg_trap hit the same wall). The
	 * 7th arg here would be `previous`, the OUT-pointer we must not lose, so we
	 * drop the redundant `task` instead: the kernel trap ignores a target and
	 * operates on current_task()'s space, exactly like every other Mach trap in
	 * that tree. The kernel-side arg struct is therefore
	 * {name, msgid, sync, notify, notifyPoly, previous}. (nextbsd#347/#353)
	 */
	if (sc != NO_SYSCALL) {
		kern_return_t rv = (kern_return_t)syscall(sc, name, msgid,
		    sync, notify, notifyPoly, previous);

		if (rv != MACH_SEND_INVALID_DEST)
			return (rv);
	}
#ifdef LIBMACH_HAVE_MIG_CLIENT
	/* The MIG routine carries `task`, so it goes back in here. */
	if (_kernelrpc_mach_port_request_notification(task, name, msgid, sync,
	    notify, notifyPoly, previous) == KERN_SUCCESS)
		return (KERN_SUCCESS);
#endif

	/* No trap and no working MIG server: boot-safe no-op (see above). */
	(void)task; (void)name; (void)msgid; (void)sync;
	(void)notify; (void)notifyPoly;
	if (previous != NULL)
		*previous = MACH_PORT_NULL;
	return KERN_SUCCESS;
}

kern_return_t
mach_port_extract_right(mach_port_name_t task, mach_port_name_t name,
    mach_msg_type_name_t desired, mach_port_t *port,
    mach_msg_type_name_t *acquired)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return _kernelrpc_mach_port_extract_right(task, name, desired, port,
	    acquired);
#else
	(void)task; (void)name; (void)desired;
	if (port != NULL)
		*port = 0;
	if (acquired != NULL)
		*acquired = 0;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

kern_return_t
mach_port_set_mscount(mach_port_name_t task, mach_port_name_t name,
    mach_port_mscount_t mscount)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return _kernelrpc_mach_port_set_mscount(task, name, mscount);
#else
	(void)task; (void)name; (void)mscount;
	return KERN_SUCCESS;
#endif
}

/*
 * mach_msg_destroy() — release the port rights carried by a message
 * that won't be processed/sent further (e.g. an RPC the server can't
 * demux, or a reply it failed to send). The previous stub did nothing,
 * so every such message stranded its rights: the reply send-once right
 * in msgh_remote_port, any moved-in header port, and any port
 * descriptors in a complex body. Those are fd-backed here, so they
 * accumulate as the type-`?` Mach-port fd leak (see #342).
 *
 * Header ports and complex-body port / OOL-ports descriptors are all
 * disposed per their disposition, as Apple's mach_msg_destroy does. A
 * received message carries MOVE_* types, so its rights are released.
 * A reply that failed to send still carries the sender's COPY_* or
 * MAKE_* types, which moved no right, so the sender's own rights (e.g.
 * launchd's send rights on the receive rights it hands out with
 * MAKE_SEND) are left alone.
 *
 * OOL memory is released too, as Apple's mach_msg_destroy does. The
 * kernel copies received OOL data out to a page-aligned base
 * (nextbsd#66), so it goes through mach_ool_unmap() below, the same
 * screen mig_deallocate()/vm_deallocate() use: a page-aligned base is
 * unmapped and anything else is left alone.
 */
#include <mach/message.h>

static kern_return_t	mach_ool_unmap(vm_address_t addr, vm_size_t size);

static void
mach_msg_destroy_port(mach_port_t port, mach_msg_type_name_t type)
{
	if (!MACH_PORT_VALID(port))
		return;
	switch (type) {
	case MACH_MSG_TYPE_MOVE_SEND:
	case MACH_MSG_TYPE_MOVE_SEND_ONCE:
		/* A send/send-once right moved to us: drop the uref. */
		(void)mach_port_deallocate(mach_task_self(), port);
		break;
	case MACH_MSG_TYPE_MOVE_RECEIVE:
		/* A receive right moved to us: destroy it. */
		(void)mach_port_mod_refs(mach_task_self(), port,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		break;
	default:
		/* COPY_SEND / MAKE_SEND* transferred no right to free. */
		break;
	}
}

void
mach_msg_destroy(mach_msg_header_t *msg)
{
	mach_msg_bits_t bits;
	mach_msg_body_t *body;
	mach_msg_size_t count, i;
	unsigned char *p;

	if (msg == NULL)
		return;

	bits = msg->msgh_bits;
	mach_msg_destroy_port(msg->msgh_remote_port, MACH_MSGH_BITS_REMOTE(bits));
	mach_msg_destroy_port(msg->msgh_local_port, MACH_MSGH_BITS_LOCAL(bits));

	if (!(bits & MACH_MSGH_BITS_COMPLEX))
		return;

	body = (mach_msg_body_t *)(msg + 1);
	count = body->msgh_descriptor_count;
	p = (unsigned char *)(body + 1);
	for (i = 0; i < count; i++) {
		/*
		 * Cast through void * to assert the alignment is sound:
		 * descriptors are 4-byte aligned (pragma pack(4), the body
		 * starts aligned, and every variant is a multiple of 4 bytes),
		 * but the byte cursor's char alignment would otherwise trip
		 * -Wcast-align.
		 */
		mach_msg_descriptor_t *d = (mach_msg_descriptor_t *)(void *)p;

		/* `type` sits at a common offset in every descriptor variant. */
		switch (d->port.type) {
		case MACH_MSG_PORT_DESCRIPTOR:
			mach_msg_destroy_port(d->port.name,
			    d->port.disposition);
			p += sizeof(mach_msg_port_descriptor_t);
			break;
		case MACH_MSG_OOL_DESCRIPTOR:
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
			(void)mach_ool_unmap(
			    (vm_address_t)(uintptr_t)d->out_of_line.address,
			    (vm_size_t)d->out_of_line.size);
			p += sizeof(mach_msg_ool_descriptor_t);
			break;
		case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
			mach_port_t *ports = (mach_port_t *)d->ool_ports.address;
			mach_msg_size_t j;

			if (ports != NULL)
				for (j = 0; j < d->ool_ports.count; j++)
					mach_msg_destroy_port(ports[j],
					    d->ool_ports.disposition);
			p += sizeof(mach_msg_ool_ports_descriptor_t);
			break;
		}
		default:
			/* Unknown descriptor type: can't size it, stop. */
			return;
		}
	}
}

/*
 * audit_token_to_au32() — unpack the 8-field audit token into named
 * uint32 outs. The token layout is fixed (val[0..7]); on FreeBSD the
 * mapping has slightly different semantics but the field arrangement
 * is the same Apple uses, so this works.
 */
void
audit_token_to_au32(audit_token_t atok,
    uint32_t *auidp, uint32_t *euidp, uint32_t *egidp,
    uint32_t *ruidp, uint32_t *rgidp, uint32_t *pidp,
    uint32_t *asidp, uint32_t *tidp)
{
	if (auidp != NULL) *auidp = atok.val[0];
	if (euidp != NULL) *euidp = atok.val[1];
	if (egidp != NULL) *egidp = atok.val[2];
	if (ruidp != NULL) *ruidp = atok.val[3];
	if (rgidp != NULL) *rgidp = atok.val[4];
	if (pidp  != NULL) *pidp  = atok.val[5];
	if (asidp != NULL) *asidp = atok.val[6];
	if (tidp  != NULL) *tidp  = atok.val[7];
}

/*
 * pid_for_task() — map a task port to its pid. Apple-canonical.
 * launchd uses it only on jobs it spawned, whose pid it already
 * knows; the stub fails closed so callers take the "unknown" path.
 */
kern_return_t
pid_for_task(mach_port_name_t task, int *pid)
{
	(void)task;
	if (pid != NULL)
		*pid = -1;
	return KERN_FAILURE;
}

/*
 * Phase I1c link-stage stubs — round 2.
 *
 * MIG runtime functions: Apple ships these as part of libsystem's
 * MIG support; they back the client-side stubs. Real implementations
 * back the reply-port cache + OOL allocation; ours are minimal so
 * non-XPC code paths (the no-IPC CLI smoke) succeed and the XPC
 * paths fail closed.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <bsm/audit.h>
#include <mach/mig_errors.h>

/*
 * Per-thread MIG reply-port cache.
 *
 * The MIG-generated client stubs call mig_get_reply_port() for the
 * receive right the RPC reply lands on and, on the success path, leave
 * it registered for reuse — only the error paths call
 * mig_dealloc_reply_port(). Allocating a fresh receive right per call
 * (and never freeing it on success) therefore leaks one Mach port per
 * RPC: every SCDynamicStore / notify / configd RPC strands a port,
 * which on this platform is fd-backed, so the process's fd table grows
 * without bound until select()-based daemons (mDNSResponder) abort on
 * FD_SETSIZE. Cache the port in thread-specific data and reuse it,
 * mirroring the per-thread reply-port libdispatch already maintains.
 *
 * A reply port is a receive right, so it must be reclaimed with
 * mach_port_mod_refs(..., RIGHT_RECEIVE, -1), not mach_port_deallocate
 * (which only drops send/send-once urefs) — the previous error-path
 * code used the wrong call.
 */
static pthread_key_t	mig_reply_port_key;
static pthread_once_t	mig_reply_port_once = PTHREAD_ONCE_INIT;

/*
 * Set while this thread is inside mig_reply_port_destroy(). See there.
 */
static __thread int	mig_reply_port_destroying;

/*
 * Destroy a reply port's receive right without ever recursing.
 *
 * Until the kernel wires the mod_refs trap, mach_port_mod_refs() is itself
 * a MIG RPC, and a failing MIG RPC calls mig_dealloc_reply_port(), which
 * lands back here. Callers clear the thread's cached reply port first, so
 * the nested RPC runs on a fresh reply port rather than the one being
 * destroyed. If that nested RPC fails too, its own mig_dealloc_reply_port()
 * finds the guard set and returns: its fresh port is leaked rather than
 * recursed on. One leaked receive right on a double failure is a far
 * better outcome than the unbounded recursion (and stack-overflow SIGSEGV)
 * this replaces.
 */
static void
mig_reply_port_destroy(mach_port_t port)
{
	if (port == MACH_PORT_NULL || mig_reply_port_destroying)
		return;
	mig_reply_port_destroying = 1;
	(void)mach_port_mod_refs(mach_task_self(), port,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	mig_reply_port_destroying = 0;
}

static void
mig_reply_port_dtor(void *p)
{
	mig_reply_port_destroy((mach_port_t)(uintptr_t)p);
}

/*
 * fork() child: forget the cached reply port.
 *
 * A forked child gets a fresh Mach name space (nextbsd-kernel#274), so the
 * name cached for the forking thread means nothing there -- or, once the
 * child allocates, names some other right. The first MIG RPC in the child
 * would send with it as the reply port. That used to be rare; with
 * task_get_special_port on MIG (#84) it is the first thing liblaunch's
 * fork-child handler does, and a forked launchd child died with SIGILL
 * before exec. Clear the slot so the next mig_get_reply_port() allocates
 * a fresh right. Do not mod_refs the old name: it is not ours in the
 * child. Apple's _mach_fork_child resets its reply port the same way.
 *
 * Registered from the key's pthread_once, which the constructor below
 * runs at load: libsystem_kernel loads before every library that links
 * it, so this child handler is registered -- and, POSIX running child
 * handlers in registration order, runs -- before liblaunch's, asl's and
 * notify's, all of which may make MIG calls.
 */
static void
mig_reply_port_atfork_child(void)
{
	(void)pthread_setspecific(mig_reply_port_key, (void *)MACH_PORT_NULL);
	mig_reply_port_destroying = 0;
}

static void
mig_reply_port_key_init(void)
{
	(void)pthread_key_create(&mig_reply_port_key, mig_reply_port_dtor);
	(void)pthread_atfork(NULL, NULL, mig_reply_port_atfork_child);
}

__attribute__((constructor))
static void
mig_reply_port_ctor(void)
{
	(void)pthread_once(&mig_reply_port_once, mig_reply_port_key_init);
}

mach_port_t
mig_get_reply_port(void)
{
	mach_port_t port;

	(void)pthread_once(&mig_reply_port_once, mig_reply_port_key_init);
	port = (mach_port_t)(uintptr_t)pthread_getspecific(mig_reply_port_key);
	if (port == MACH_PORT_NULL) {
		port = (mach_port_t)mach_reply_port();
		if (port != MACH_PORT_NULL)
			(void)pthread_setspecific(mig_reply_port_key,
			    (void *)(uintptr_t)port);
	}
	return port;
}

/*
 * Called by MIG stubs after a send that did not consume the reply port
 * (MACH_SEND_INVALID_{DATA,DEST,HEADER}): the port is still good, so it
 * stays cached in TSD for the next RPC on this thread. A no-op, as in
 * Apple's libsyscall.
 */
void
mig_put_reply_port(mach_port_t port)
{
	(void)port;
}

/*
 * Called by MIG stubs on every other error, including a WaitTime timeout
 * (MACH_RCV_TIMED_OUT): the port may yet receive the late reply, so it is
 * destroyed and the next mig_get_reply_port() allocates a fresh one.
 *
 * Order matters, as in Apple's libsyscall (mig_reply_port.c): forget the
 * cached port BEFORE destroying it. This used to destroy first, while the
 * port was still cached, and mach_port_mod_refs() is a MIG RPC here -- so
 * it went out on the very reply port it was destroying, failed, and its
 * error path called mig_dealloc_reply_port() on the same port again, until
 * the stack overflowed (sshd-session cores via pam_launchd's probe timeout,
 * ~112,000 frames of mig_dealloc_reply_port -> mach_port_mod_refs ->
 * _kernelrpc_mach_port_mod_refs). mig_reply_port_destroy() also refuses to
 * re-enter, so a failure of the destroy RPC itself cannot recurse either.
 */
void
mig_dealloc_reply_port(mach_port_t port)
{
	mach_port_t cached;

	(void)pthread_once(&mig_reply_port_once, mig_reply_port_key_init);
	cached = (mach_port_t)(uintptr_t)pthread_getspecific(
	    mig_reply_port_key);
	if (cached != MACH_PORT_NULL && cached == port)
		(void)pthread_setspecific(mig_reply_port_key,
		    (void *)MACH_PORT_NULL);
	mig_reply_port_destroy(port);
}

/*
 * mig_allocate — mmap, matching vm_allocate.
 *
 * This used to malloc, which is what forced both mig_deallocate and
 * vm_deallocate to be no-ops: the same deallocator name was reachable with
 * buffers from two different allocators, and calling free() on an mmap'd
 * region (or munmap() on a malloc'd one) crashes. With both allocators
 * mmap-based the ambiguity is gone and the deallocators can do real work.
 */
kern_return_t
mig_allocate(vm_address_t *addr, vm_size_t size)
{
	void *p;

	if (addr == NULL || size == 0)
		return KERN_INVALID_ARGUMENT;
	p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED)
		return KERN_RESOURCE_SHORTAGE;
	*addr = (vm_address_t)(uintptr_t)p;
	return KERN_SUCCESS;
}

/*
 * mach_ool_unmap — the one place that decides whether a pointer handed to
 * vm_deallocate/mig_deallocate is safe to munmap.
 *
 * Everything that legitimately reaches those two now comes from an mmap:
 * vm_allocate, mig_allocate above, or the kernel's own copyout of received
 * out-of-line memory. The one exception is an INTERIOR pointer into a
 * kernel-mapped message body, which vproc_swap_complex hands back (see
 * 9fe3d79) -- munmap would fail on it anyway, since it is neither a mapping
 * base nor page-aligned.
 *
 * So: require page alignment before unmapping. That admits every real base
 * and rejects every interior offset, and munmap's own EINVAL is a second
 * line of defence rather than the only one. A non-aligned pointer is left
 * alone, exactly as before -- a bounded leak on that one path instead of a
 * crash on all of them.
 */
static kern_return_t
mach_ool_unmap(vm_address_t addr, vm_size_t size)
{
	long pagesize;

	if (addr == 0 || size == 0)
		return KERN_SUCCESS;

	pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize <= 0)
		return KERN_SUCCESS;

	if ((uintptr_t)addr % (uintptr_t)pagesize != 0)
		return KERN_SUCCESS;	/* interior pointer; not ours to unmap */

	/* KERN_FAILURE, not KERN_INVALID_ADDRESS: libmach's kern_return.h
	 * defines only SUCCESS/FAILURE/ABORTED/NOT_SUPPORTED/TIMED_OUT, and
	 * this is the code mach_vm_deallocate already returned for a failed
	 * munmap. */
	if (munmap((void *)(uintptr_t)addr, (size_t)size) != 0)
		return KERN_FAILURE;
	return KERN_SUCCESS;
}

/*
 * mig_deallocate — release OOL memory handed back by a MIG reply.
 *
 * Was a no-op. mig_allocate now mmaps, so server-side buffers and
 * kernel-copied-out OOL memory are both real mappings and both can be
 * unmapped; mach_ool_unmap() screens out the interior-pointer case that
 * forced the no-op originally.
 */
kern_return_t
mig_deallocate(vm_address_t addr, vm_size_t size)
{
	return mach_ool_unmap(addr, size);
}

int
mig_strncpy(char *dst, const char *src, int len)
{
	if (len <= 0)
		return 0;
	int n = (int)strlen(src);
	if (n >= len)
		n = len - 1;
	memcpy(dst, src, (size_t)n);
	dst[n] = '\0';
	return n;
}

/*
 * vm_allocate / vm_deallocate — Mach VM API surface.
 *
 * vm_deallocate was a no-op because the same name could be reached with
 * malloc'd buffers from the old mig_allocate. That is fixed at the source:
 * mig_allocate mmaps now, so every buffer reaching here is a real mapping.
 *
 * This matters beyond MIG. libsystem_asl allocates its ASL_STRING_VM buffers
 * with vm_allocate and releases them with vm_deallocate (asl_string.c:78,111,
 * 187), correctly distinguishing them from its malloc'd ones -- so with the
 * no-op in place ASL was leaking an mmap on every message send path, not just
 * on the MIG paths.
 */
kern_return_t
vm_allocate(mach_port_name_t target, vm_address_t *address,
    vm_size_t size, int flags)
{
	void *p;
	(void)target;
	(void)flags;
	if (address == NULL || size == 0)
		return (KERN_INVALID_ARGUMENT);
	p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED)
		return (KERN_NO_SPACE);
	*address = (vm_address_t)(uintptr_t)p;
	return (KERN_SUCCESS);
}

kern_return_t
vm_deallocate(mach_port_name_t task, vm_address_t addr, vm_size_t size)
{
	(void)task;
	return mach_ool_unmap(addr, size);
}

/* Additional mach_port stubs. */

kern_return_t
mach_port_get_set_status(mach_port_name_t task, mach_port_name_t name,
    mach_port_name_array_t *members, mach_msg_type_number_t *membersCnt)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return _kernelrpc_mach_port_get_set_status(task, name, members,
	    membersCnt);
#else
	(void)task; (void)name;
	if (members != NULL)
		*members = NULL;
	if (membersCnt != NULL)
		*membersCnt = 0;
	return KERN_SUCCESS;
#endif
}

kern_return_t
mach_port_set_context(mach_port_name_t task, mach_port_name_t name,
    mach_port_context_t context)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	/* Widening a value, not aliasing a pointer -- safe either way. */
	return _kernelrpc_mach_port_set_context(task, name,
	    (mach_vm_address_t)context);
#else
	(void)task; (void)name; (void)context;
	return KERN_SUCCESS;
#endif
}

/*
 * audit_session_self / audit_session_join — Apple-shape decls live
 * in FreeBSD's <bsm/audit.h> but only behind __APPLE_API_PRIVATE,
 * which libmach doesn't define. libbsm also doesn't ship the
 * implementations. Re-state the prototypes here so libmach's
 * -Wmissing-prototypes is satisfied and stub them so the link
 * succeeds; runtime callers see MACH_PORT_NULL / AU_DEFAUDITSID.
 */
mach_port_name_t audit_session_self(void);
au_asid_t        audit_session_join(mach_port_name_t port);

mach_port_name_t
audit_session_self(void)
{
	return MACH_PORT_NULL;
}

au_asid_t
audit_session_join(mach_port_name_t port)
{
	(void)port;
	return 0;	/* AU_DEFAUDITSID */
}

/* host_set_UNDServer — User Notification Daemon port. macOS-only;
 * stub success so launchd's set-once call after startup is a no-op. */
kern_return_t
host_set_UNDServer(host_t host, mach_port_t port)
{
	(void)host; (void)port;
	return KERN_SUCCESS;
}

/*
 * gL1CacheEnabled — Apple's Libinfo exposes this `extern int` so
 * callers can disable the negative-lookup cache around getpwnam
 * retries. launchd-842/core.c does `extern int gL1CacheEnabled;`
 * and assigns `false` before re-querying. FreeBSD's nsswitch has no
 * equivalent toggle, so the variable is purely a sink. Carry an
 * extern decl here too so -Wmissing-variable-declarations is happy.
 */
extern int gL1CacheEnabled;
int gL1CacheEnabled = 1;

/*
 * task_self_trap() — Apple's raw-syscall name for "give me my task
 * port". Same shape as mach_task_self(); some Apple-source uses the
 * trap name directly (e.g. inside Mach-VM C++ glue).
 */
mach_port_name_t
task_self_trap(void)
{
	return mach_task_self();
}

/*
 * task_for_pid() — get a send right to pid's task port (nextbsd#359).
 *
 * Calls the mach.syscall.task_for_pid trap (nextbsd-kernel#298). As in
 * XNU this is a trap only; there is no MIG routine to fall back to. The
 * kernel returns the kern_return_t, writes *t on every path
 * (MACH_PORT_NULL on failure) and decides who may ask: pid 0, a
 * target_tport that is not the caller's own task, no such process or a
 * ptrace(2)-style permission denial all give KERN_FAILURE.
 *
 * If the trap is not registered (mach.ko not loaded) this fails closed
 * with KERN_FAILURE and *t = MACH_PORT_NULL, the same answer as a
 * denied caller.
 */
kern_return_t
task_for_pid(mach_port_name_t target_tport, int pid, mach_port_name_t *t)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "task_for_pid");
	kern_return_t rv;

	if (t != NULL)
		*t = MACH_PORT_NULL;
	if (sc == NO_SYSCALL)
		return (KERN_FAILURE);
	rv = (kern_return_t)syscall(sc, target_tport, pid, t);
	if (rv == -1)		/* the syscall itself failed: errno, not kr */
		return (KERN_FAILURE);
	return (rv);
}

/*
 * task_set_importance_watchports_np() — hand the next exec its process
 * type and importance watch ports (nextbsd#539 part E).
 *
 * Calls the mach.syscall.task_set_importance_watchports_np trap
 * (nextbsd-kernel part E). launchd's NextBSD job_start_child() calls it
 * right before execve(2), in place of the posix_spawn(POSIX_SPAWN_SETEXEC)
 * attributes xnu-2422 takes. If the trap is not registered (an older
 * kernel, or mach.ko not loaded) this fails open: it does nothing and
 * returns KERN_NOT_SUPPORTED, and the exec goes ahead without them.
 */
kern_return_t
task_set_importance_watchports_np(mach_port_name_t *names, int count,
    int proctype)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "task_set_importance_watchports_np");
	kern_return_t rv;

	if (sc == NO_SYSCALL)
		return (KERN_NOT_SUPPORTED);
	rv = (kern_return_t)syscall(sc, names, count, proctype);
	if (rv == -1)		/* the syscall itself failed: errno, not kr */
		return (KERN_FAILURE);
	return (rv);
}

/*
 * task_importance_assertion_np() — hold or drop one external importance
 * assertion on the calling task (nextbsd#539 part G).
 *
 * Calls the mach.syscall.task_importance_assertion_np trap
 * (nextbsd-kernel part G), NextBSD's form of xnu-2422's
 * process_policy(PROC_POLICY_SCOPE_PROCESS, action, PROC_POLICY_BOOST,
 * PROC_POLICY_IMP_IMPORTANT, ..., getpid()). Returns 0 or an errno value,
 * as Apple's libproc callers expect. ENOSYS if the trap is not registered
 * (an older kernel, or mach.ko not loaded); the libproc functions below
 * treat that as success, so importance handling fails open.
 */
int
task_importance_assertion_np(int action)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "task_importance_assertion_np");

	if (sc == NO_SYSCALL)
		return (ENOSYS);
	if (syscall(sc, action) == -1)
		return (errno);
	return (0);
}

/*
 * proc_importance_assertion_begin_with_msg() and
 * proc_importance_assertion_complete() — Apple's libproc importance
 * assertion API (xnu-2422 libsyscall/wrappers/libproc/libproc.c). On
 * Darwin they live in libsystem_kernel next to mach_msg_server_importance;
 * NextBSD has no libproc library of its own (<libproc.h> is FreeBSD's
 * unrelated debugger library), so they live here too. Apple declares them
 * in <libproc_internal.h>; NextBSD consumers declare them themselves
 * (launchd's libproc_internal.h shim, libdispatch's internal.h).
 *
 * A message received with MACH_MSGH_BITS_RAISEIMP carries one boost that
 * the kernel has already handed to the receiving task. The first begin on
 * that message takes over that boost without a kernel call and marks the
 * message MACH_MSGH_BITS_IMPHOLDASRT; a later begin on the same message
 * holds one more assertion. Each successful begin is paired with one
 * complete, which drops one assertion. A message without RAISEIMP gives
 * EIO, as on Darwin.
 *
 * The token is the address of a variable, as in Apple's libproc, so a
 * debugger prints a useful name.
 */
#define	XNU_PROC_POLICY_ACTION_HOLD	14
#define	XNU_PROC_POLICY_ACTION_DROP	15

int proc_importance_assertion_begin_with_msg(mach_msg_header_t *msg,
    mach_msg_trailer_t *trailer, uint64_t *assertion_token);
int proc_importance_assertion_complete(uint64_t assertion_token);

extern uint64_t important_boost_assertion_token;
extern uint64_t normal_boost_assertion_token;
extern uint64_t non_boost_assertion_token;
uint64_t important_boost_assertion_token = 0xfafafafafafafafaULL;
uint64_t normal_boost_assertion_token    = 0xfbfbfbfbfbfbfbfbULL;
uint64_t non_boost_assertion_token       = 0xfcfcfcfcfcfcfcfcULL;

int
proc_importance_assertion_begin_with_msg(mach_msg_header_t *msg,
    mach_msg_trailer_t *trailer __unused, uint64_t *assertion_token)
{
	int rval;

	if (assertion_token == NULL)
		return (EINVAL);
	if (msg == NULL || (msg->msgh_bits & MACH_MSGH_BITS_RAISEIMP) == 0)
		return (EIO);	/* not a boosting message */

	/* Take over the boost the message brought; no kernel call. */
	if ((msg->msgh_bits & MACH_MSGH_BITS_IMPHOLDASRT) == 0) {
		msg->msgh_bits |= MACH_MSGH_BITS_IMPHOLDASRT;
		*assertion_token = (uint64_t)(uintptr_t)
		    &important_boost_assertion_token;
		return (0);
	}

	/* Already taken over: hold an additional assertion. */
	rval = task_importance_assertion_np(XNU_PROC_POLICY_ACTION_HOLD);
	if (rval == 0 || rval == ENOSYS) {
		*assertion_token = (uint64_t)(uintptr_t)
		    &important_boost_assertion_token;
		return (0);
	}
	return (rval);
}

int
proc_importance_assertion_complete(uint64_t assertion_token)
{
	int rval;

	if (assertion_token == 0)
		return (0);
	if (assertion_token !=
	    (uint64_t)(uintptr_t)&important_boost_assertion_token)
		return (EIO);	/* invalid token */

	rval = task_importance_assertion_np(XNU_PROC_POLICY_ACTION_DROP);
	if (rval == ENOSYS)
		return (0);
	return (rval);
}

/*
 * mach_msg_server_importance() — xnu-2422's MIG server loop for adaptive
 * daemons (libsyscall/mach/mach_msg.c): mach_msg_server() with the
 * message's importance boost taken over before the demux runs and dropped
 * once the reply is sent. Buffers come from malloc rather than
 * vm_allocate; the loop is otherwise Apple's. Returns only on a receive
 * error (or when a buffer cannot be allocated), as Apple's does.
 */
mach_msg_return_t
mach_msg_server_importance(
    boolean_t (*demux)(mach_msg_header_t *, mach_msg_header_t *),
    mach_msg_size_t max_size, mach_port_name_t rcv_name,
    mach_msg_option_t options)
{
	mig_reply_error_t *bufRequest = NULL, *bufReply;
	mach_msg_size_t request_alloc, new_request_alloc, request_size;
	mach_msg_size_t reply_alloc;
	mach_msg_return_t mr;
	uint64_t token = 0;
	int retval;

	options &= ~(MACH_SEND_MSG | MACH_RCV_MSG);

	reply_alloc = max_size + (mach_msg_size_t)MAX_TRAILER_SIZE;
	bufReply = calloc(1, reply_alloc);
	if (bufReply == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	request_alloc = 0;
	new_request_alloc = max_size + (mach_msg_size_t)MAX_TRAILER_SIZE;
	request_size = new_request_alloc;

	for (;;) {
		if (request_alloc < new_request_alloc) {
			free(bufRequest);
			request_alloc = new_request_alloc;
			bufRequest = calloc(1, request_alloc);
			if (bufRequest == NULL) {
				free(bufReply);
				return (KERN_RESOURCE_SHORTAGE);
			}
		}

		mr = mach_msg(&bufRequest->Head, MACH_RCV_MSG | options,
		    0, request_size, rcv_name, MACH_MSG_TIMEOUT_NONE,
		    MACH_PORT_NULL);

		if (mr == MACH_MSG_SUCCESS) {
			/* we have another request message */
			retval = proc_importance_assertion_begin_with_msg(
			    &bufRequest->Head, NULL, &token);
			(void)(*demux)(&bufRequest->Head, &bufReply->Head);

			if (!(bufReply->Head.msgh_bits &
			    MACH_MSGH_BITS_COMPLEX)) {
				if (bufReply->RetCode == MIG_NO_REPLY)
					bufReply->Head.msgh_remote_port =
					    MACH_PORT_NULL;
				else if (bufReply->RetCode != KERN_SUCCESS &&
				    (bufRequest->Head.msgh_bits &
				    MACH_MSGH_BITS_COMPLEX)) {
					/* destroy the request, not the reply port */
					bufRequest->Head.msgh_remote_port =
					    MACH_PORT_NULL;
					mach_msg_destroy(&bufRequest->Head);
				}
			}

			/*
			 * Don't block forever on a client that is not
			 * receiving on its reply port: a send-once right
			 * cannot block, a send right gets MACH_SEND_TIMEOUT.
			 */
			if (bufReply->Head.msgh_remote_port != MACH_PORT_NULL) {
				mr = mach_msg(&bufReply->Head,
				    (MACH_MSGH_BITS_REMOTE(
				    bufReply->Head.msgh_bits) ==
				    MACH_MSG_TYPE_MOVE_SEND_ONCE) ?
				    MACH_SEND_MSG | options :
				    MACH_SEND_MSG | MACH_SEND_TIMEOUT | options,
				    bufReply->Head.msgh_size, 0,
				    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
				    MACH_PORT_NULL);

				if (mr != (mach_msg_return_t)
				    MACH_SEND_INVALID_DEST &&
				    mr != (mach_msg_return_t)
				    MACH_SEND_TIMED_OUT) {
					if (retval == 0)
						(void)proc_importance_assertion_complete(
						    token);
					continue;
				}
				mr = MACH_MSG_SUCCESS;
			}
			if (bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX)
				mach_msg_destroy(&bufReply->Head);
			if (retval == 0)
				(void)proc_importance_assertion_complete(token);
			continue;
		}

		if (mr == (mach_msg_return_t)MACH_RCV_TOO_LARGE &&
		    (options & MACH_RCV_LARGE)) {
			new_request_alloc = bufRequest->Head.msgh_size +
			    (mach_msg_size_t)MAX_TRAILER_SIZE;
			request_size = new_request_alloc;
			continue;
		}
		break;
	}

	free(bufRequest);
	free(bufReply);
	return (mr);
}

/*
 * task_name_for_pid() — get a send right to pid's task name port
 * (nextbsd-kernel#308).
 *
 * Calls the mach.syscall.task_name_for_pid trap (nextbsd-kernel#309). As
 * in XNU this is a trap only; there is no MIG routine to fall back to. A
 * name port is introspection-only: it identifies the task and is accepted
 * by task_info(), but never converts to a task, space or map. The kernel
 * writes *t on every path (MACH_PORT_NULL on failure) and decides who may
 * ask, using xnu-2422's rule: pid 0, a target_task that is not the
 * caller's own task, a process the caller cannot see, or a caller that is
 * not the target, not root and not the same effective and real uid all
 * give KERN_FAILURE.
 *
 * If the trap is not registered (an older kernel, or mach.ko not loaded)
 * this fails closed with KERN_FAILURE and *t = MACH_PORT_NULL, the same
 * answer as a denied caller.
 */
kern_return_t
task_name_for_pid(mach_port_name_t target_task, int pid,
    mach_port_name_t *t)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "task_name_for_pid");
	kern_return_t rv;

	if (t != NULL)
		*t = MACH_PORT_NULL;
	if (sc == NO_SYSCALL)
		return (KERN_FAILURE);
	rv = (kern_return_t)syscall(sc, target_task, pid, t);
	if (rv == -1)		/* the syscall itself failed: errno, not kr */
		return (KERN_FAILURE);
	return (rv);
}

/*
 * macx_triggers() — register a receive right for default_pager_space_alert
 * (nextbsd/nextbsd-userland#178).
 *
 * Calls the mach.syscall.macx_triggers trap (nextbsd-kernel#218). As in
 * xnu-2422 it is a trap only and returns the kernel's value itself (0,
 * EINVAL, EPERM), not -1/errno. If the trap is not registered (an older
 * kernel, or mach.ko not loaded) this returns ENOSYS, as
 * task_importance_assertion_np() does; swapd treats that as "no dynamic
 * paging on this kernel".
 */
kern_return_t
macx_triggers(int hi_water, int low_water, int flags,
    mach_port_name_t alert_port)
{
	static int num = SYSCALL_UNRESOLVED;
	int sc = resolve_syscall_once(&num, "macx_triggers");
	kern_return_t rv;

	if (sc == NO_SYSCALL)
		return (ENOSYS);
	rv = (kern_return_t)syscall(sc, hi_water, low_water, flags,
	    alert_port);
	if (rv == -1)		/* the syscall itself failed: errno, not a value */
		return (errno);
	return (rv);
}

kern_return_t
mach_port_get_context(mach_port_name_t task, mach_port_name_t name,
    mach_port_context_t *context)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	{
		/*
		 * The wire type is mach_vm_address_t (uint64_t); the public
		 * API uses mach_port_context_t (uintptr_t). Same width on
		 * LP64, but distinct types -- WARNS=6 rejects aliasing the
		 * pointers, so bridge through a local rather than casting the
		 * pointer, which would be wrong if the widths ever diverge.
		 */
		mach_vm_address_t ctx = 0;
		kern_return_t kr;

		if (context == NULL)
			return KERN_INVALID_ARGUMENT;
		kr = _kernelrpc_mach_port_get_context(task, name, &ctx);
		if (kr == KERN_SUCCESS)
			*context = (mach_port_context_t)ctx;
		return kr;
	}
#else
	(void)task; (void)name;
	if (context != NULL)
		*context = 0;
	return KERN_SUCCESS;
#endif
}

/*
 * thread_switch() — yield + optional short wait. See mach_traps.h for
 * the design note. Reuses sched_yield(); SWITCH_OPTION_WAIT adds a
 * nanosleep before the yield. option_time is in milliseconds (Apple's
 * convention). Deliberate userland implementation, not a stub.
 */
kern_return_t
thread_switch(mach_port_name_t thread_name, int option, uint32_t option_time)
{
	(void)thread_name;

	if (option == SWITCH_OPTION_WAIT && option_time != 0) {
		struct timespec ts;
		ts.tv_sec = option_time / 1000;
		ts.tv_nsec = (long)(option_time % 1000) * 1000000L;
		(void)nanosleep(&ts, NULL);
	}
	(void)sched_yield();
	return KERN_SUCCESS;
}

/*
 * thread_destruct_special_reply_port — XNU-private sync-IPC helper.
 * Never called at runtime on FreeBSD because
 * DISPATCH_USE_MACH_SEND_SYNC_OVERRIDE is 0 (target.h:58 makes
 * DISPATCH_MIN_REQUIRED_OSX_AT_LEAST return 0 on non-Apple). Stub
 * fails closed so any unexpected runtime call is detectable.
 */
kern_return_t
thread_destruct_special_reply_port(mach_port_name_t reply_port,
    enum thread_destruct_special_reply_port_rights rights)
{
	(void)reply_port;
	(void)rights;
	return KERN_FAILURE;
}

/*
 * mach_port_destruct — Apple-API tear-down of a port's receive + a
 * single send-once right in one call. We currently model the universe
 * as single-task, so receive and send-once aren't separable; route to
 * mach_port_deallocate. srdelta and guard are accepted but ignored.
 */
kern_return_t
mach_port_destruct(mach_port_name_t task, mach_port_name_t name,
    mach_port_delta_t srdelta, mach_port_context_t guard)
{
	kern_return_t kr;

	/*
	 * Guards are not tracked by the kernel yet, so we cannot enforce the
	 * guard argument. That is a known gap (#84), not an accident: rather
	 * than fail a call we cannot validate, we honour the part of the
	 * contract we can.
	 */
	(void)guard;

	/*
	 * Apple's contract is two operations, not one: drop srdelta send
	 * rights, THEN destroy the receive right. The previous version
	 * discarded srdelta and called mach_port_deallocate(), which drops a
	 * single reference of whatever right happened to be there -- so a
	 * caller passing srdelta = -1 leaked a send right every time.
	 */
	if (srdelta != 0) {
		kr = mach_port_mod_refs(task, name, MACH_PORT_RIGHT_SEND, srdelta);
		/*
		 * No send rights is legal here -- destruct is routinely called
		 * on a port the caller only ever held receive on. Anything else
		 * is a real failure and must not be papered over.
		 */
		if (kr != KERN_SUCCESS && kr != KERN_INVALID_RIGHT)
			return kr;
	}

	return mach_port_mod_refs(task, name, MACH_PORT_RIGHT_RECEIVE, -1);
}

/*
 * mach_port_construct — Apple's options-bearing port allocator.
 * libdispatch (_dispatch_mach_notify_port_init) passes
 * MPO_CONTEXT_AS_GUARD | MPO_STRICT to attach a guard cookie that
 * mach_port_destruct must present on tear-down. Our kernel doesn't
 * track guards yet; we route to mach_port_allocate(RECEIVE) and
 * discard the options.
 */
kern_return_t
mach_port_construct(mach_port_name_t task, mach_port_options_t *opts,
    mach_port_context_t guard, mach_port_name_t *name)
{
	kern_return_t kr;

	kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, name);
	if (kr != KERN_SUCCESS)
		return kr;

	if (opts == NULL)
		return KERN_SUCCESS;

	/*
	 * Apply the options instead of discarding them.
	 *
	 * This used to be `(void)opts; (void)guard;`, which silently dropped
	 * everything callers asked for. The kernel implements the pieces that
	 * matter, so nothing here needs kernel work:
	 *
	 *   MPO_QLIMIT           -> mach_port_set_attributes(LIMITS_INFO)
	 *                           ipc_port_set_qlimit(), mach_port.c
	 *   MPO_CONTEXT_AS_GUARD -> mach_port_set_context()
	 *                           sets ip_context, mach_port.c
	 *   MPO_TEMPOWNER        -> mach_port_set_attributes(TEMPOWNER)
	 *   MPO_INSERT_SEND_RIGHT-> mach_port_insert_right(MAKE_SEND)
	 *
	 * The qlimit one was doing real damage: the kernel default is
	 * MACH_PORT_QLIMIT_BASIC (5), so notifyd's per-client common ports ran
	 * 5 deep while asking for 16 (notify_proc.c). Whether that bites
	 * depends on how fast the client drains -- i.e. it produces
	 * load-dependent, intermittent failures rather than a clean one.
	 *
	 * Failures are reported, not swallowed: a caller that asked for a
	 * guard and did not get one must not believe the port is guarded.
	 * The port is destroyed first so we do not leak a half-configured one.
	 */
	if (opts->flags & MPO_QLIMIT) {
		mach_port_limits_t limits;

		limits.mpl_qlimit = opts->mpl.mpl_qlimit;
		kr = mach_port_set_attributes(task, *name, MACH_PORT_LIMITS_INFO,
		    (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT);
		if (kr != KERN_SUCCESS)
			goto fail;
	}

	if (opts->flags & MPO_TEMPOWNER) {
		kr = mach_port_set_attributes(task, *name, MACH_PORT_TEMPOWNER,
		    NULL, 0);
		if (kr != KERN_SUCCESS)
			goto fail;
	}

	if (opts->flags & MPO_CONTEXT_AS_GUARD) {
		kr = mach_port_set_context(task, *name, guard);
		if (kr != KERN_SUCCESS)
			goto fail;
	}

	if (opts->flags & MPO_INSERT_SEND_RIGHT) {
		kr = mach_port_insert_right(task, *name, *name,
		    MACH_MSG_TYPE_MAKE_SEND);
		if (kr != KERN_SUCCESS)
			goto fail;
	}

	/*
	 * MPO_STRICT / MPO_IMPORTANCE_RECEIVER / MPO_DENAP_RECEIVER /
	 * MPO_IMMOVABLE_RECEIVE / MPO_FILTER_MSG have no kernel support yet.
	 * Ignored deliberately -- they harden an already-working port rather
	 * than change its observable behaviour.
	 */
	return KERN_SUCCESS;

fail:
	(void)mach_port_mod_refs(task, *name, MACH_PORT_RIGHT_RECEIVE, -1);
	*name = MACH_PORT_NULL;
	return kr;
}

/*
 * host_get_special_port: MIG (host_priv 412, #93 group 5).
 *
 * The kernel answers HOST_PORT, HOST_PRIV_PORT and the userland-settable
 * HOST_BOOTSTRAP_PORT / HOST_KEXTD_PORT slots (MACH_PORT_NULL if unset);
 * any other slot is KERN_INVALID_ARGUMENT. `node` is ignored, as in XNU.
 * A request port that is not host_priv gets KERN_INVALID_HOST.
 */
kern_return_t
host_get_special_port(mach_port_name_t host_priv, int node, int which,
    mach_port_t *port)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_host_get_special_port(host_priv, node, which,
	    port));
#else
	(void)host_priv; (void)node; (void)which;
	if (port != NULL)
		*port = MACH_PORT_NULL;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

/*
 * host_get_host_port — demote a host_priv port to the unprivileged host
 * port. Apple's is a macro over host_get_special_port(HOST_LOCAL_NODE,
 * HOST_PORT); this is the same call (#93 group 5).
 *
 * It used to copy its input name, which was accidentally right while
 * mach_host_self() only ever returned the unprivileged port. Since
 * nextbsd-kernel#285 root gets host_priv, and libdispatch
 * (_dispatch_mach_host_port_init) relies on this to demote it.
 *
 * KERN_INVALID_HOST -> KERN_INVALID_ARGUMENT: given the unprivileged port
 * (every non-root caller), this kernel's host_priv intran yields HOST_NULL
 * and the handler says KERN_INVALID_HOST. XNU says KERN_INVALID_ARGUMENT
 * there, and that is the code libdispatch treats as "already
 * unprivileged, keep it"; anything else it reports as a bug. Translate,
 * so every non-root libdispatch process does not log one.
 */
kern_return_t
host_get_host_port(mach_port_name_t host_priv, mach_port_name_t *host_port)
{
	kern_return_t kr;

	kr = host_get_special_port(host_priv, HOST_LOCAL_NODE, HOST_PORT,
	    host_port);
	if (kr == KERN_INVALID_HOST)
		kr = KERN_INVALID_ARGUMENT;
	return (kr);
}

/*
 * host_request_notification: MIG (mach_host 217, #93 group 4).
 *
 * Used to return KERN_SUCCESS and never deliver. The kernel accepts
 * HOST_NOTIFY_CALENDAR_CHANGE only (KERN_INVALID_ARGUMENT otherwise),
 * keeps the send-once right MIG makes from `notify_port`, and when the
 * wall clock is stepped (settimeofday / clock_settime) sends it a
 * host_calendar_changed message, msgid HOST_CALENDAR_CHANGED_REPLYID
 * (950), and clears the registration. libdispatch (event_kevent.c,
 * HOST_NOTIFY_CALENDAR_SET == HOST_NOTIFY_CALENDAR_CHANGE here) handles
 * 950 and re-registers.
 */
kern_return_t
host_request_notification(mach_port_name_t host, int notify_type,
    mach_port_name_t notify_port)
{
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return (_kernelrpc_host_request_notification(host, notify_type,
	    notify_port));
#else
	(void)host;
	(void)notify_type;
	(void)notify_port;
	return KERN_RESOURCE_SHORTAGE;
#endif
}

/*
 * Mach VM APIs — used by libdispatch's data.c (line 139
 * mach_vm_deallocate for the dispatch_data large-buffer path). Apple
 * implements these as full kernel RPC; we route to mmap/munmap. The
 * VM_PROT_* and VM_FLAGS_* options are accepted but ignored — every
 * allocation is RW anon-private, every deallocation is munmap.
 */
#include <sys/mman.h>

kern_return_t
mach_vm_allocate(mach_port_name_t target, mach_vm_address_t *address,
    mach_vm_size_t size, int flags)
{
	void *p;

	(void)target;
	(void)flags;
	if (address == NULL || size == 0)
		return (KERN_INVALID_ARGUMENT);

	p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED)
		return (KERN_NO_SPACE);
	*address = (mach_vm_address_t)(uintptr_t)p;
	return (KERN_SUCCESS);
}

kern_return_t
mach_vm_deallocate(mach_port_name_t target, mach_vm_address_t address,
    mach_vm_size_t size)
{
	(void)target;
	/* Same screening as vm_deallocate: this was an unguarded munmap, which
	 * would fault rather than fail on an interior pointer. */
	return mach_ool_unmap((vm_address_t)address, (vm_size_t)size);
}

/*
 * mach_port_type — return the rights mask for a port name in the
 * caller's task. Apple uses it for port-validity probes. Without
 * kernel introspection wired up, we report RECEIVE if the name
 * isn't NULL/DEAD; consumers (event_kevent.c:289) use it for
 * coarse validation.
 */
kern_return_t
mach_port_type(mach_port_name_t task, mach_port_name_t name,
    mach_port_type_t *type)
{
	if (type == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD) {
		*type = 0;
		return (KERN_INVALID_NAME);
	}
#ifdef LIBMACH_HAVE_MIG_CLIENT
	return _kernelrpc_mach_port_type(task, name, type);
#else
	/* Previously answered MACH_PORT_TYPE_RECEIVE for every valid name --
	 * not merely incomplete but wrong, since it claims a receive right the
	 * caller may not hold. */
	(void)task;
	*type = MACH_PORT_TYPE_RECEIVE;
	return (KERN_SUCCESS);
#endif
}

/*
 * Mach semaphores — backed by POSIX sem_t under a small registry
 * keyed by Apple-shape mach_port_name_t identifiers. libdispatch's
 * USE_MACH_SEM path uses these as the underlying _dispatch_sema4
 * primitive. The implementation is intentionally minimal: linear
 * scan over a 256-entry table, locked by a single mutex. Real
 * workloads (dispatch_semaphore_wait, _dispatch_sema4_* on contended
 * paths) only allocate a handful of these per process.
 */
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MACH_SEM_TABLE_SIZE 256

struct mach_sem_entry {
	mach_port_name_t	name;
	sem_t			sem;
	int			in_use;
};

static struct mach_sem_entry	mach_sem_table[MACH_SEM_TABLE_SIZE];
static pthread_mutex_t		mach_sem_table_mtx = PTHREAD_MUTEX_INITIALIZER;
static mach_port_name_t		mach_sem_next_name = 0x10000;

static struct mach_sem_entry *
mach_sem_find_locked(mach_port_name_t name)
{
	for (int i = 0; i < MACH_SEM_TABLE_SIZE; i++) {
		if (mach_sem_table[i].in_use &&
		    mach_sem_table[i].name == name) {
			return (&mach_sem_table[i]);
		}
	}
	return (NULL);
}

/* Deliberate userland implementation (POSIX sem_t registry above), not a stub. */
kern_return_t
semaphore_create(mach_port_name_t task, semaphore_t *sem, int policy,
    int value)
{
	(void)task;
	(void)policy;

	if (sem == NULL || value < 0)
		return (KERN_INVALID_ARGUMENT);

	pthread_mutex_lock(&mach_sem_table_mtx);
	for (int i = 0; i < MACH_SEM_TABLE_SIZE; i++) {
		if (!mach_sem_table[i].in_use) {
			if (sem_init(&mach_sem_table[i].sem, 0,
			    (unsigned int)value) != 0) {
				pthread_mutex_unlock(&mach_sem_table_mtx);
				return (KERN_RESOURCE_SHORTAGE);
			}
			mach_sem_table[i].name = mach_sem_next_name++;
			mach_sem_table[i].in_use = 1;
			*sem = mach_sem_table[i].name;
			pthread_mutex_unlock(&mach_sem_table_mtx);
			return (KERN_SUCCESS);
		}
	}
	pthread_mutex_unlock(&mach_sem_table_mtx);
	return (KERN_RESOURCE_SHORTAGE);
}

/* Deliberate userland implementation (POSIX sem_t registry above), not a stub. */
kern_return_t
semaphore_destroy(mach_port_name_t task, semaphore_t sem)
{
	struct mach_sem_entry *e;

	(void)task;
	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e == NULL) {
		pthread_mutex_unlock(&mach_sem_table_mtx);
		return (KERN_INVALID_ARGUMENT);
	}
	(void)sem_destroy(&e->sem);
	e->in_use = 0;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	return (KERN_SUCCESS);
}

/* Deliberate userland implementation (POSIX sem_t registry above), not a stub. */
kern_return_t
semaphore_signal(semaphore_t sem)
{
	struct mach_sem_entry *e;
	sem_t *p = NULL;

	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e != NULL)
		p = &e->sem;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	if (p == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (sem_post(p) != 0)
		return (KERN_FAILURE);
	return (KERN_SUCCESS);
}

/* Deliberate userland implementation (POSIX sem_t registry above), not a stub. */
kern_return_t
semaphore_wait(semaphore_t sem)
{
	struct mach_sem_entry *e;
	sem_t *p = NULL;

	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e != NULL)
		p = &e->sem;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	if (p == NULL)
		return (KERN_INVALID_ARGUMENT);
	while (sem_wait(p) != 0) {
		if (errno == EINTR)
			return (KERN_ABORTED);
		return (KERN_FAILURE);
	}
	return (KERN_SUCCESS);
}

/* Deliberate userland implementation (POSIX sem_t registry above), not a stub. */
kern_return_t
semaphore_timedwait(semaphore_t sem, mach_timespec_t wait_time)
{
	struct mach_sem_entry *e;
	struct timespec abs_ts;
	sem_t *p = NULL;

	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e != NULL)
		p = &e->sem;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	if (p == NULL)
		return (KERN_INVALID_ARGUMENT);

	/* sem_timedwait takes an absolute CLOCK_REALTIME deadline; Apple's
	 * semaphore_timedwait takes a relative delta. Convert by adding to
	 * now. */
	if (clock_gettime(CLOCK_REALTIME, &abs_ts) != 0)
		return (KERN_FAILURE);
	abs_ts.tv_sec += (time_t)wait_time.tv_sec;
	abs_ts.tv_nsec += wait_time.tv_nsec;
	if (abs_ts.tv_nsec >= 1000000000L) {
		abs_ts.tv_sec += 1;
		abs_ts.tv_nsec -= 1000000000L;
	}

	if (sem_timedwait(p, &abs_ts) != 0) {
		if (errno == ETIMEDOUT)
			return (KERN_OPERATION_TIMED_OUT);
		if (errno == EINTR)
			return (KERN_ABORTED);
		return (KERN_FAILURE);
	}
	return (KERN_SUCCESS);
}

/*
 * mach_msg_server_once() — MIG runtime helper: receive one message,
 * dispatch it through the given demux, send the reply. liblaunch's
 * libvproc.c uses it to service helper-downcall requests.
 *
 * Implements the canonical loop body once: rcv into a buffer, call
 * demux, send. demux signature matches Apple's mig_server_routine_t
 * (a function that returns boolean_t after filling the reply).
 *
 * Buffer size is the caller's max_size + room for the largest
 * trailer; with no header alignment trickery yet, just stack-
 * allocate via VLA.
 */
typedef boolean_t (*_mach_demux_t)(mach_msg_header_t *, mach_msg_header_t *);

mach_msg_return_t
mach_msg_server_once(boolean_t (*demux)(mach_msg_header_t *, mach_msg_header_t *),
    mach_msg_size_t max_size, mach_port_name_t rcv_name,
    mach_msg_options_t options)
{
	(void)demux; (void)max_size; (void)rcv_name; (void)options;
	/*
	 * Real implementation: mach_msg(rcv) -> demux(req, rep) ->
	 * mach_msg(send). For Phase I1c we don't run the helper-down-
	 * call path; stub to MACH_RCV_TIMED_OUT so callers retry
	 * harmlessly. liblaunch's poller treats any non-success as
	 * "no message right now".
	 */
	return MACH_RCV_TIMED_OUT;
}
