/*
 * sys/mount.h — FreeBSD shim layered over the base header.
 *
 * launchd-842's core.c learns that /var became writable from the Darwin
 * VQ_UPDATE EVFILT_FS event (jobmgr_callback(), flag2check). The VQ_* set
 * inherited from FreeBSD has no VQ_UPDATE; the NextBSD kernel adds it with
 * Darwin's value, 0x0100 (the unused VQ_FLAG0100 slot), and posts it when
 * an MNT_UPDATE mount succeeds (nextbsd/nextbsd-kernel
 * patches/0064-NextBSD-post-VQ_UPDATE-to-EVFILT_FS-on-mount-update.patch).
 *
 * userland builds against the toolchain sysroot, whose sys/mount.h does not
 * carry that patch, so define the value here. It must match the kernel:
 * filt_fsevent() delivers only hint bits present in the knote's fflags.
 */
#ifndef _FREEBSD_SHIM_SYS_MOUNT_H_
#define _FREEBSD_SHIM_SYS_MOUNT_H_

#include_next <sys/mount.h>

#ifndef VQ_UPDATE
#define VQ_UPDATE	0x0100	/* filesystem information has changed (Darwin) */
#endif

#endif /* _FREEBSD_SHIM_SYS_MOUNT_H_ */
