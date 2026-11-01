/*
 * DAMount.h — the mount decision, mount points and /Volumes (E15 C5,
 * nextbsd-userland#195).
 *
 * Port of Apple's DAMount.c (DAMountWithArguments, the mount-point
 * routines, DAMountGetPreference) and __DAMainCreateMountPointFolder
 * (DAMain.c). The per-filesystem option table is DAMountHelper.c.
 */
#ifndef DISKARBITRATION_DAMOUNT_H
#define DISKARBITRATION_DAMOUNT_H

#include <dispatch/dispatch.h>

#include "DADisk.h"

/* Apple's mount folder (DAMain.c kDAMainMountPointFolder). */
#define DA_MOUNT_POINT_FOLDER	"/Volumes"

/* ___EDIRTY: a dirty volume asked to mount read-write without repair. */
#define DA_EDIRTY		0x550

/* Set the daemon queue mount completions run on. Call once, first. */
void	DAMountInit(dispatch_queue_t queue);

typedef enum {
	kDAMountPreferenceDefer,
	kDAMountPreferenceTrust,
	kDAMountPreferenceDisableAutoMount,
} DAMountPreference;

/* Apple's DAMountGetPreference (DAMount.c), over DASupport.c's list. */
int	DAMountGetPreference(const struct da_disk *disk,
	    DAMountPreference preference);

/*
 * Status 0 and the mount point on success; an errno-style status (or
 * DA_EDIRTY) and NULL otherwise. Runs on the daemon queue.
 */
typedef void (*DAMountCallback)(int status, const char *mountpoint,
	    void *context);

/*
 * Mount `disk` (on the daemon queue). `mountpoint` NULL means a new
 * /Volumes/<name> directory. `automatic` is Apple's "automatic" argument:
 * the stage machine's mount, which the autodiskmount, defer and repair
 * rules apply to; a client request (#197) passes 0. `options` (may be
 * NULL) are extra comma-separated mount options; "force" is understood as
 * Apple's kDAFileSystemMountArgumentForce. `callback` may be NULL.
 */
void	DAMountWithArguments(struct da_disk *disk, const char *mountpoint,
	    int automatic, const char *options, DAMountCallback callback,
	    void *context);

/*
 * Make /Volumes 0755 root:wheel and sweep it (Apple's
 * __DAMainCreateMountPointFolder): empty directories and symlinks left
 * from an earlier boot are removed; anything mounted on or populated is
 * left alone, because rmdir(2) refuses it. Call once at startup, before
 * the first mount.
 */
void	DAMountPointFolderCreate(void);

/*
 * Drop every mount point the daemon made that is no longer mounted on
 * (DAMountRemoveMountPoint for the unmount path). `mounted` holds the
 * f_mntonname of every current mount. Called by DAVolume.c's refresh.
 */
void	DAMountPointListRefresh(const char *const *mounted, int count);

/*
 * Apple's DAMountRemoveMountPoint after a client's unmount (#198): rmdir
 * `path` if this daemon made it under /Volumes. A mount point from the
 * /etc/fstab map, or any directory the daemon did not create, is left.
 */
void	DAMountRemoveMountPoint(const char *path);

/* --- per-filesystem helper table (DAMountHelper.c) ------------------------ */

struct da_mount_args {
	char	options[1024];	/* comma-separated, for mount -o */
	int	readonly;	/* the filesystem only mounts read-only */
	int	noatime;	/* noatime means something for it */
};

/*
 * Append the filesystem-specific options for `kind` (owner and mode
 * options for filesystems without ownership, extras such as longnames or
 * -e) to `args`, for a volume owned by uid/gid.
 */
void	DAMountHelperOptions(const char *kind, uid_t uid, gid_t gid,
	    struct da_mount_args *args);

/* Append one option to a comma-separated list; -1 if it does not fit. */
int	DAMountOptionAppend(char *list, size_t len, const char *option);
/* Insert one option at the front; -1 if it does not fit. */
int	DAMountOptionPrepend(char *list, size_t len, const char *option);
/* Whether the list holds `option` exactly. */
int	DAMountOptionContains(const char *list, const char *option);

#endif /* DISKARBITRATION_DAMOUNT_H */
