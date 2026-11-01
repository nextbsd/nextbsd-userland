/*
 * DAMountHelper.c — the per-filesystem mount option table (E15 C5,
 * nextbsd-userland#195).
 *
 * Apple's DAMountWithArguments knows one filesystem by name: for hfs it
 * adds -u=<uid>,-g=<gid>,-m=0755 so the volume shows its owner's files
 * (DAMount.c, the hfs block). NextBSD's mount helpers differ per
 * filesystem, so the same idea is a table keyed by the bundle's
 * CFBundleName (the vfs type). mount(8) turns "-x=y" items into "-x y"
 * arguments for the helper and plain items into "-o item" (mount.c
 * mangle()).
 *
 * Kind      Helper                              Options added here
 * msdosfs   /sbin/mount_msdosfs (base)          longnames,-u,-g,-m=755,-M=755
 * exfat     mount.exfat-fuse (FUSE, a port)     uid,gid,umask=022
 * ntfs      ntfs-3g (FUSE, a port)              uid,gid
 * ufs       mount -t ufs (base)                 -
 * cd9660    /sbin/mount_cd9660 (base)           -e; always read-only
 * udf       /sbin/mount_udf (base)              always read-only
 *
 * Owner options are always added for the filesystems that store no
 * ownership (Apple does the same for hfs regardless of trust): without
 * them mount_msdosfs takes owner and mode from the mount point, which
 * DAMount.c creates root-owned and mode 0111. Not passed, on purpose:
 * mount_msdosfs -L/-D (they load msdosfs_iconv, which a kernel without
 * modules cannot; nextbsd-kernel#228), -C for cd9660 (needs CD9660_ICONV,
 * same ticket), and ntfs-3g "recover"/"remove_hiberfile" (a dirty NTFS
 * mounts read-only instead; DAMount.c). ext2fs, hfs+ and xfs have no
 * bundle yet.
 */
#include "DAMount.h"

#include <stdio.h>
#include <string.h>

static const struct da_mount_helper {
	const char	*kind;
	const char	*owner;		/* printf(uid, gid), or NULL */
	const char	*extra;		/* fixed options, or NULL */
	int		 noatime;	/* noatime applies */
	int		 readonly;	/* read-only media format */
} helpers[] = {
	{ "msdosfs",	"-u=%u,-g=%u,-m=755,-M=755",	"longnames",	1, 0 },
	{ "exfat",	"uid=%u,gid=%u,umask=022",	NULL,		1, 0 },
	{ "ntfs",	"uid=%u,gid=%u",		NULL,		1, 0 },
	{ "ufs",	NULL,				NULL,		1, 0 },
	{ "cd9660",	NULL,				"-e",		0, 1 },
	{ "udf",	NULL,				NULL,		0, 1 },
};

int
DAMountOptionContains(const char *list, const char *option)
{
	size_t n = strlen(option);
	const char *p = list;

	while (*p != '\0') {
		const char *end = strchr(p, ',');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);

		if (len == n && strncmp(p, option, n) == 0)
			return (1);
		if (end == NULL)
			break;
		p = end + 1;
	}
	return (0);
}

int
DAMountOptionAppend(char *list, size_t len, const char *option)
{
	size_t have = strlen(list);

	if (option == NULL || option[0] == '\0')
		return (0);
	if (have + (have != 0) + strlen(option) + 1 > len)
		return (-1);
	if (have != 0)
		list[have++] = ',';
	(void)strlcpy(list + have, option, len - have);
	return (0);
}

int
DAMountOptionPrepend(char *list, size_t len, const char *option)
{
	size_t have = strlen(list), n;

	if (option == NULL || option[0] == '\0')
		return (0);
	n = strlen(option);
	if (n + (have != 0) + have + 1 > len)
		return (-1);
	if (have != 0) {
		memmove(list + n + 1, list, have + 1);
		list[n] = ',';
	} else
		list[n] = '\0';
	memcpy(list, option, n);
	return (0);
}

void
DAMountHelperOptions(const char *kind, uid_t uid, gid_t gid,
    struct da_mount_args *args)
{
	char owner[128];
	size_t i;

	args->readonly = 0;
	args->noatime = 0;
	for (i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++) {
		const struct da_mount_helper *h = &helpers[i];

		if (strcmp(h->kind, kind) != 0)
			continue;
		args->readonly = h->readonly;
		args->noatime = h->noatime;
		(void)DAMountOptionAppend(args->options,
		    sizeof(args->options), h->extra);
		if (h->owner != NULL) {
			(void)snprintf(owner, sizeof(owner), h->owner,
			    (unsigned)uid, (unsigned)gid);
			(void)DAMountOptionAppend(args->options,
			    sizeof(args->options), owner);
		}
		return;
	}
	/*
	 * A bundle the table does not know (dropped into
	 * /Library/Filesystems): Apple's generic options only.
	 */
	args->noatime = 1;
}
