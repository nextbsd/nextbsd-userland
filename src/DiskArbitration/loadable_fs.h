/*
 * loadable_fs.h — the filesystem-bundle helper contract (E15 C4,
 * nextbsd-userland#194).
 *
 * Darwin's <sys/loadable_fs.h> names the bundle directory, the per-bundle
 * "<name>.util" helper, its single-letter commands (FSUC_*) and its exit
 * codes (FSUR_*). diskarbitrationd runs the helpers and fsutil(8)
 * implements them, so both include this header. The values are Darwin's:
 * a helper exits with a small negative number, which the parent reads back
 * as (char)WEXITSTATUS (Apple DACommand.c).
 */
#ifndef DISKARBITRATION_LOADABLE_FS_H
#define DISKARBITRATION_LOADABLE_FS_H

#define FS_DIR_LOCATION		"/System/Library/Filesystems"
#define FS_DIR_LOCATION_LOCAL	"/Library/Filesystems"	/* Apple DABase.h */
#define FS_DIR_SUFFIX		".fs"
#define FS_UTIL_SUFFIX		".util"

/* Commands: argv[1] is "-<letter>". */
#define FSUC_PROBE		'p'	/* -p <dev> removable|fixed readonly|writable */
#define FSUC_GETUUID		'k'	/* -k <dev>: print the volume UUID */
#define FSUC_QUICKVERIFY	'q'	/* -q <devpath>: exit 0 if clean (repair executable) */
#define FSUC_REPAIR		'y'	/* -y <devpath>: repair (repair executable) */

/* Exit codes. */
#define FSUR_RECOGNIZED		(-1)	/* -p: the filesystem is this bundle's */
#define FSUR_UNRECOGNIZED	(-2)	/* -p: it is not */
#define FSUR_IO_SUCCESS		(-3)	/* -k: success */
#define FSUR_IO_FAIL		(-4)	/* I/O error */
#define FSUR_IO_UNCLEAN		(-5)	/* the volume needs repair */
#define FSUR_INVAL		(-6)	/* invalid command or argument */

#endif /* DISKARBITRATION_LOADABLE_FS_H */
