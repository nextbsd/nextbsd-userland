/*
 * libproc_internal.h — FreeBSD shim for Apple's <libproc_internal.h>,
 * the private companion to <libproc.h>.
 *
 * The proc_info(2) wrappers + struct vocabulary live in the
 * <libproc.h> shim and <sys/proc_info.h> shim; this header adds the
 * private entry point launchd-842's core.c uses on top of them,
 * proc_setpcontrol(), and the importance assertion calls Apple declares
 * here (nextbsd#539 part G), which libsystem_kernel provides.
 */
#ifndef _FREEBSD_SHIM_LIBPROC_INTERNAL_H_
#define _FREEBSD_SHIM_LIBPROC_INTERNAL_H_

#include <libproc.h>		/* proc_* wrappers + dirty-tracking flags */
#include <sys/proc_info.h>	/* PROC_SETPC_* controls */
#include <stdint.h>
#include <mach/message.h>	/* mach_msg_header_t, mach_msg_trailer_t */

/*
 * proc_setpcontrol() — sets how the kernel treats the calling
 * process under memory pressure (throttle / suspend / terminate).
 * FreeBSD has no per-process memory-pressure control knob, so this
 * is a no-op success: launchd's request is silently dropped.
 */
static __inline int
proc_setpcontrol(int control)
{
	(void)control;
	return 0;
}

/*
 * Importance assertions (xnu-2422 libproc). Take over, or add to, the
 * importance boost a MACH_MSGH_BITS_RAISEIMP message brought (EIO if it
 * brought none), and drop it again. Implemented in libsystem_kernel
 * (src/libmach); they succeed and do nothing when the kernel has no
 * task_importance_assertion_np trap.
 */
int proc_importance_assertion_begin_with_msg(mach_msg_header_t *msg,
    mach_msg_trailer_t *trailer, uint64_t *assertion_token);
int proc_importance_assertion_complete(uint64_t assertion_token);

#endif /* _FREEBSD_SHIM_LIBPROC_INTERNAL_H_ */
