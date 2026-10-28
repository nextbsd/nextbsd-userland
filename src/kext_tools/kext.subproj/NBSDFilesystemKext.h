/*
 * NBSDFilesystemKext.h — load the kext a filesystem type needs before
 * mounting it (nextbsd-userland#436).
 *
 * The NextBSD kernel never loads modules by name (E21 D9), so `mount -t X`
 * no longer pulls in X's module. On Darwin, whoever mounts loads the
 * filesystem's kext first: the mount helpers call
 * KextManagerLoadKextWithIdentifier(), automount loads autofs.kext. This is
 * that step, shared, so each caller doesn't carry its own copy.
 */
#ifndef __NBSDFILESYSTEMKEXT_H__
#define __NBSDFILESYSTEMKEXT_H__

#include <sys/cdefs.h>

__BEGIN_DECLS

/*
 * Make filesystem type `fstype` (as mount(2) and getvfsbyname(3) name it,
 * e.g. "autofs") available, loading its kext through kextd if the kernel
 * doesn't have it yet.
 *
 * The kext is found from, in order:
 *   1. the filesystem bundle, /System/Library/Filesystems/<fstype>.fs: its
 *      Info.plist key NBSDKextIdentifier names the kext's bundle ID;
 *   2. the kexts in /System/Library/Extensions: the one whose NBSDModules
 *      (the module names it provides) lists `fstype`.
 *
 * Returns 0 once getvfsbyname(fstype) succeeds (at once if the kernel
 * already has it), else an errno: ENOENT when no kext provides the type,
 * EPERM when kextd refused the caller, EIO when the load failed, ENODEV when
 * the kext loaded but registered no such filesystem.
 */
int	NBSDFilesystemLoadKext(const char *fstype);

__END_DECLS

#endif /* __NBSDFILESYSTEMKEXT_H__ */
