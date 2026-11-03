/*
 * libproc.h — pmconfigd's <libproc.h>: launchd's shim plus proc_name().
 *
 * pmconfigd tags every request with the caller's process name, read from
 * the audit token's PID through libproc's proc_name() (AutoWakeScheduler.c,
 * PMAssertions.c, PMConnection.m, HIDEventWatcher.c). launchd's
 * freebsd-shims/libproc.h, next on pmconfigd's include path, covers the rest
 * of libproc but not proc_name(), so this header adds it on top.
 *
 * proc_name() is sysctl(KERN_PROC_PID)'s ki_comm, the same field launchd's
 * proc_pidinfo(PROC_PIDT_SHORTBSDINFO) copies into pbsi_comm. Like Apple's,
 * it returns the length of the name copied, or 0 if the process is gone.
 */
#ifndef _PM_SHIM_LIBPROC_H_
#define _PM_SHIM_LIBPROC_H_

#include_next <libproc.h>

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <stdint.h>
#include <string.h>

static __inline int
proc_name(int pid, void *buffer, uint32_t buffersize)
{
	struct kinfo_proc kp;
	size_t kplen = sizeof(kp);
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };

	if (buffer == NULL || buffersize == 0 || pid <= 0)
		return 0;
	if (sysctl(mib, 4, &kp, &kplen, NULL, 0) != 0 || kplen == 0)
		return 0;
	(void)strlcpy(buffer, kp.ki_comm, buffersize);
	return (int)strlen(buffer);
}

#endif /* !_PM_SHIM_LIBPROC_H_ */
