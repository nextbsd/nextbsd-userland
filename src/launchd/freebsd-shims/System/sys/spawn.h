/*
 * System/sys/spawn.h — FreeBSD shim for Apple's posix_spawn
 * extensions. launchd-842's core.c drives job execution through
 * posix_spawn() with a pile of Apple-only attributes: the SETEXEC /
 * START_SUSPENDED spawn flags, process-type bands, and the
 * posix_spawnattr_*_np() setters.
 *
 * FreeBSD's base <spawn.h> has standard POSIX_SPAWN_SET* flags but
 * none of the Apple extensions. This header (the single source of
 * truth — <System/sys/spawn_internal.h> and <spawn_private.h> just
 * re-include it) supplies the missing constants and stubs most _np
 * setters to success: NextBSD has no kernel knob behind them, so those
 * attributes are dropped and posix_spawn() runs with standard POSIX
 * semantics.
 *
 * The process type and the importance watch ports are real (nextbsd#539
 * part E): their setters record them in a side table keyed by the
 * posix_spawnattr_t, and launchd's NextBSD SETEXEC emulation in
 * job_start_child() hands them to the kernel right before execve(2)
 * through _posix_spawnattr_apply_importance_np().
 */
#ifndef _FREEBSD_SHIM_SYSTEM_SYS_SPAWN_H_
#define _FREEBSD_SHIM_SYSTEM_SYS_SPAWN_H_

#include <sys/types.h>
#include <stdint.h>
#include <spawn.h>		/* posix_spawnattr_t (FreeBSD base) */
#include <stdlib.h>		/* malloc, free */
#include <errno.h>
#include <mach/machine.h>	/* cpu_type_t */
#include <mach/mach_traps.h>	/* task_set_importance_watchports_np */

/*
 * Apple posix_spawn flags. Values are placed above FreeBSD's base
 * POSIX_SPAWN_SET* bits (0x01..0x20) to avoid collision when OR'd
 * into the same short flag word.
 */
#ifndef POSIX_SPAWN_SETEXEC
#define POSIX_SPAWN_SETEXEC		0x0040
#endif
#ifndef POSIX_SPAWN_START_SUSPENDED
#define POSIX_SPAWN_START_SUSPENDED	0x0080
#endif
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR	0x0100
#endif
#ifndef POSIX_SPAWN_IOS_INTERACTIVE
#define POSIX_SPAWN_IOS_INTERACTIVE	0x0200
#endif
#ifndef POSIX_SPAWN_JETSAM_HIWATER_BACKGROUND
#define POSIX_SPAWN_JETSAM_HIWATER_BACKGROUND		0x0400
#endif
#ifndef POSIX_SPAWN_JETSAM_USE_EFFECTIVE_PRIORITY
#define POSIX_SPAWN_JETSAM_USE_EFFECTIVE_PRIORITY	0x0800
#endif

/*
 * Apple QoS tiers (latency + throughput). On macOS these live in
 * <sys/qos.h>; launchd-842 references the "launch default" tiers
 * from core.c for the job-spawn QoS attributes. FreeBSD has no QoS
 * tier mechanism — values are placeholder, swallowed by the no-op
 * posix_spawnattr_*_np() setters below.
 */
#ifndef LATENCY_QOS_TIER_UNSPECIFIED
#define LATENCY_QOS_TIER_UNSPECIFIED		0x0
#define LATENCY_QOS_TIER_0			((0xFF<<16) | 1)
#define LATENCY_QOS_TIER_1			((0xFF<<16) | 2)
#define LATENCY_QOS_TIER_2			((0xFF<<16) | 3)
#define LATENCY_QOS_TIER_3			((0xFF<<16) | 4)
#define LATENCY_QOS_TIER_4			((0xFF<<16) | 5)
#define LATENCY_QOS_TIER_5			((0xFF<<16) | 6)
#define LATENCY_QOS_LAUNCH_DEFAULT_TIER		LATENCY_QOS_TIER_3

#define THROUGHPUT_QOS_TIER_UNSPECIFIED		0x0
#define THROUGHPUT_QOS_TIER_0			((0xFE<<16) | 1)
#define THROUGHPUT_QOS_TIER_1			((0xFE<<16) | 2)
#define THROUGHPUT_QOS_TIER_2			((0xFE<<16) | 3)
#define THROUGHPUT_QOS_TIER_3			((0xFE<<16) | 4)
#define THROUGHPUT_QOS_TIER_4			((0xFE<<16) | 5)
#define THROUGHPUT_QOS_TIER_5			((0xFE<<16) | 6)
#define THROUGHPUT_QOS_LAUNCH_DEFAULT_TIER	THROUGHPUT_QOS_TIER_3
#endif

/*
 * Apple process-type bands passed to posix_spawnattr_setprocesstype_np(),
 * at their xnu-2422 (OS X 10.9) values from bsd/sys/spawn_internal.h:
 * the NextBSD kernel's task_set_importance_watchports_np trap takes them
 * as xnu-2422's posix_spawn does.
 */
#define POSIX_SPAWN_PROC_TYPE_MASK			0x00000F00
#define POSIX_SPAWN_PROC_TYPE_APP_DEFAULT		0x00000100
#define POSIX_SPAWN_PROC_TYPE_APP_TAL			0x00000200
#define POSIX_SPAWN_PROC_TYPE_DAEMON_STANDARD		0x00000300
#define POSIX_SPAWN_PROC_TYPE_DAEMON_INTERACTIVE	0x00000400
#define POSIX_SPAWN_PROC_TYPE_DAEMON_BACKGROUND		0x00000500
#define POSIX_SPAWN_PROC_TYPE_DAEMON_ADAPTIVE		0x00000600

/*
 * Side table for the attributes NextBSD acts on (nextbsd#539 part E).
 *
 * posix_spawnattr_t, inherited from FreeBSD, is an opaque pointer to a
 * struct libc owns, so the Apple-only fields cannot live in it. They are
 * kept here instead, keyed by that pointer, from the _np setter until
 * posix_spawnattr_destroy() (wrapped below) or the exec. launchd sets up
 * one attr per job, in the forked child, so the table is small; it is
 * per translation unit (only core.c uses it) and not thread-safe, as
 * launchd's child is single-threaded at this point.
 */
#define _NEXTBSD_SPAWNATTR_SLOTS	8

struct _nextbsd_spawnattr_extra {
	posix_spawnattr_t	key;		/* NULL: slot free */
	int			proctype;	/* POSIX_SPAWN_PROC_TYPE_*, 0 unset */
	int			nports;
	mach_port_name_t	*ports;		/* names; the rights stay in the space */
};

static __inline struct _nextbsd_spawnattr_extra *
_nextbsd_spawnattr_extra(const posix_spawnattr_t *attr, int create)
{
	static struct _nextbsd_spawnattr_extra table[_NEXTBSD_SPAWNATTR_SLOTS];
	struct _nextbsd_spawnattr_extra *slot = NULL;
	int i;

	if (attr == NULL || *attr == NULL)
		return NULL;
	for (i = 0; i < _NEXTBSD_SPAWNATTR_SLOTS; i++) {
		if (table[i].key == *attr)
			return &table[i];
		if (slot == NULL && table[i].key == NULL)
			slot = &table[i];
	}
	if (!create || slot == NULL)
		return NULL;
	slot->key = *attr;
	slot->proctype = 0;
	slot->nports = 0;
	slot->ports = NULL;
	return slot;
}

static __inline void
_nextbsd_spawnattr_extra_free(const posix_spawnattr_t *attr)
{
	struct _nextbsd_spawnattr_extra *x = _nextbsd_spawnattr_extra(attr, 0);

	if (x == NULL)
		return;
	free(x->ports);
	x->ports = NULL;
	x->nports = 0;
	x->proctype = 0;
	x->key = NULL;
}

/*
 * posix_spawnattr_destroy() also drops the attr's side-table entry. A
 * function-like macro: it reaches every call, and leaves the function's
 * own declaration in <spawn.h> (already included above) alone.
 */
static __inline int
_nextbsd_posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
	_nextbsd_spawnattr_extra_free(attr);
	return (posix_spawnattr_destroy)(attr);
}
#define posix_spawnattr_destroy(attr)	_nextbsd_posix_spawnattr_destroy(attr)

static __inline int
posix_spawnattr_setprocesstype_np(posix_spawnattr_t *attr, uint64_t type)
{
	struct _nextbsd_spawnattr_extra *x;

	x = _nextbsd_spawnattr_extra(attr, 1);
	if (x == NULL)
		return (attr == NULL || *attr == NULL) ? EINVAL : ENOMEM;
	x->proctype = (int)(type & POSIX_SPAWN_PROC_TYPE_MASK);
	return 0;
}

/*
 * Apple posix_spawnattr_*_np() setters with nothing behind them on
 * NextBSD — stub to success so the (best-effort, job_assumes_zero-wrapped)
 * callers see "attribute accepted" and posix_spawn() proceeds normally.
 */

static __inline int
posix_spawnattr_setbinpref_np(posix_spawnattr_t *attr, size_t count,
    cpu_type_t *pref, size_t *ocount)
{
	(void)attr;
	(void)pref;
	if (ocount != NULL) {
		*ocount = count;
	}
	return 0;
}

static __inline int
posix_spawnattr_setmacpolicyinfo_np(posix_spawnattr_t *attr,
    const char *policyname, void *policyinfo, size_t policyinfosize)
{
	(void)attr;
	(void)policyname;
	(void)policyinfo;
	(void)policyinfosize;
	return 0;
}

/*
 * Record the importance watch ports, as xnu-2422 appends a
 * PSPA_IMP_WATCHPORTS port action for each. Only the names are kept: the
 * caller keeps its send rights until the exec (launchd does), and the
 * kernel copies them in when _posix_spawnattr_apply_importance_np() runs.
 * Adds to any ports set before, as xnu-2422 does.
 */
static __inline int
posix_spawnattr_set_importancewatch_port_np(posix_spawnattr_t *attr,
    int count, mach_port_t portarray[])
{
	struct _nextbsd_spawnattr_extra *x;
	mach_port_name_t *ports;
	int i;

	if (count < 0 || (count > 0 && portarray == NULL))
		return EINVAL;
	if (count == 0)
		return 0;
	x = _nextbsd_spawnattr_extra(attr, 1);
	if (x == NULL)
		return (attr == NULL || *attr == NULL) ? EINVAL : ENOMEM;
	ports = realloc(x->ports, (size_t)(x->nports + count) * sizeof(*ports));
	if (ports == NULL)
		return ENOMEM;
	for (i = 0; i < count; i++)
		ports[x->nports + i] = (mach_port_name_t)portarray[i];
	x->ports = ports;
	x->nports += count;
	return 0;
}

/*
 * NextBSD: hand the attr's process type and importance watch ports to
 * the kernel for the coming execve(2), the part of xnu-2422's
 * posix_spawn(POSIX_SPAWN_SETEXEC) that launchd's SETEXEC emulation
 * otherwise loses. Call it last, right before execve(2): the kernel
 * applies both once the new image is loaded. Fails open: if the kernel
 * has no such trap, or refuses the call, the exec simply goes ahead
 * without them. Returns the kern_return_t, for logging only.
 */
static __inline kern_return_t
_posix_spawnattr_apply_importance_np(const posix_spawnattr_t *attr)
{
	struct _nextbsd_spawnattr_extra *x = _nextbsd_spawnattr_extra(attr, 0);

	if (x == NULL || (x->proctype == 0 && x->nports == 0))
		return KERN_SUCCESS;
	return task_set_importance_watchports_np(x->ports, x->nports,
	    x->proctype);
}

static __inline int
posix_spawnattr_setcpumonitor_default(posix_spawnattr_t *attr)
{
	(void)attr;
	return 0;
}

/*
 * NB on POSIX_SPAWN_SETEXEC: launchd's job_start_child sets this
 * flag and calls posix_spawn() from the runtime_fork child. Apple
 * semantics are "do not fork; replace the calling process." FreeBSD
 * libc has no equivalent and its POSIX_SPAWN_VALID_FLAGS=0x3F masks
 * our SETEXEC bit out as invalid. Earlier shim attempts here defined
 * a static-inline wrapper + macro to intercept the call, but the
 * macro substitution did not actually reach core.c's call site
 * (suspected interaction with core.c's later "#include <spawn.h>"
 * redeclaring the symbol). The SETEXEC emulation is therefore done
 * directly at the call site in core.c (#ifdef __FreeBSD__ block in
 * job_start_child, iter 15). Do not re-add wrappers here without
 * verifying the macro reaches the call site.
 */

#endif /* _FREEBSD_SHIM_SYSTEM_SYS_SPAWN_H_ */
