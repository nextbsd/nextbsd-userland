/*
 * fsutil — the "<name>.util" helper of every NextBSD filesystem bundle
 * (E15 C4, nextbsd-userland#194).
 *
 * Darwin ships one helper per bundle (msdos.util, hfs.util, ...) that
 * implements the <sys/loadable_fs.h> contract diskarbitrationd drives:
 *
 *   -p <dev> removable readonly   print the volume label; exit FSUR_RECOGNIZED
 *                                 or FSUR_UNRECOGNIZED
 *   -k <dev>                      print the volume UUID; exit FSUR_IO_SUCCESS
 *   -q <devpath>                  exit 0 if the volume is clean, 1 if dirty
 *   -y <devpath>                  repair it (the real fsck, never automatic)
 *
 * On NextBSD one binary serves all six bundles. It is installed once and
 * symlinked as each bundle's Contents/Resources/<name>.util; argv[0] picks
 * the filesystem. Recognition and labels come from fstyp(8) (UDF, which
 * fstyp does not know, is read here). The clean checks read on-disk state
 * only and never write to the device.
 *
 * Not done yet: the -k UUID is the GPT partition UUID when the daemon
 * passes one (DA_PARTITION_UUID), else a name-based UUID (v5) over the disk
 * ident, partition index and label. Real volume serials (FAT BPB volume ID,
 * UFS fs_id, NTFS/exFAT serial) are a follow-up.
 */
#include <sys/param.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libufs.h>
#include <paths.h>
#include <sha.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../loadable_fs.h"

extern char **environ;

/* Bundle name (the helper's basename without .util) -> NextBSD vfs type. */
static const struct fstype {
	const char	*bundle;	/* msdos */
	const char	*vfs;		/* msdosfs: CFBundleName, fstyp's name */
	const char	*fsck;		/* repair tool for -y, NULL = none */
} fstypes[] = {
	{ "msdos",	"msdosfs",	"/sbin/fsck_msdosfs" },
	{ "exfat",	"exfat",	"/usr/local/sbin/fsck.exfat" },
	{ "ntfs",	"ntfs",		"/usr/local/bin/ntfsfix" },
	{ "ufs",	"ufs",		"/sbin/fsck_ufs" },
	{ "cd9660",	"cd9660",	NULL },
	{ "udf",	"udf",		NULL },
};

/* Namespace for the name-based -k UUIDs (fixed, NextBSD-local). */
static const uint8_t uuid_ns[16] = {
	0x06, 0xd1, 0xbb, 0x37, 0x5f, 0x64, 0x4a, 0x30,
	0x85, 0x2d, 0x37, 0x63, 0xf6, 0xd1, 0xb5, 0xcb,
};

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s -p device removable|fixed readonly|writable\n"
	    "       %s -k device\n"
	    "       %s -q|-y device\n", prog, prog, prog);
	exit(FSUR_INVAL);
}

/* "da0p1" or "/dev/da0p1" -> "/dev/da0p1". */
static void
dev_path(const char *dev, char *buf, size_t len)
{
	if (dev[0] == '/')
		(void)strlcpy(buf, dev, len);
	else
		(void)snprintf(buf, len, "%s%s", _PATH_DEV, dev);
}

/* --- device reads --------------------------------------------------------- */

static unsigned
sector_size(int fd)
{
	u_int ss = 0;

	if (ioctl(fd, DIOCGSECTORSIZE, &ss) != 0 || ss == 0)
		ss = 512;
	return (ss);
}

/*
 * Read `len` bytes at `off` from a disk device. Devices only accept
 * sector-aligned, sector-sized transfers, so read the covering sectors.
 */
static int
read_at(int fd, off_t off, void *buf, size_t len)
{
	unsigned ss = sector_size(fd);
	off_t start = off - (off % ss);
	size_t span = (size_t)(off - start) + len;
	size_t total = roundup2(span, ss);
	char *tmp;
	ssize_t n;

	tmp = malloc(total);
	if (tmp == NULL)
		return (-1);
	n = pread(fd, tmp, total, start);
	if (n < 0 || (size_t)n < span) {
		free(tmp);
		return (-1);
	}
	memcpy(buf, tmp + (off - start), len);
	free(tmp);
	return (0);
}

static uint16_t
le16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | (p[1] << 8)));
}

static uint32_t
le32(const uint8_t *p)
{
	return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* --- -p: recognition ------------------------------------------------------ */

/*
 * Run fstyp -l on the device. Returns 0 and fills `type` and `label` when
 * fstyp recognised something, -1 otherwise.
 */
static int
run_fstyp(const char *path, char *type, size_t tlen, char *label, size_t llen)
{
	char *argv[] = { "fstyp", "-l", (char *)(uintptr_t)path, NULL };
	posix_spawn_file_actions_t fa;
	char out[1024], *sp, *nl;
	size_t got = 0;
	ssize_t n;
	pid_t pid;
	int pfd[2], status;

	type[0] = label[0] = '\0';
	if (pipe(pfd) != 0)
		return (-1);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fa, pfd[0]);
	posix_spawn_file_actions_addclose(&fa, pfd[1]);
	if (posix_spawn(&pid, "/usr/sbin/fstyp", &fa, NULL, argv,
	    environ) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		(void)close(pfd[0]);
		(void)close(pfd[1]);
		return (-1);
	}
	posix_spawn_file_actions_destroy(&fa);
	(void)close(pfd[1]);
	while (got < sizeof(out) - 1 &&
	    (n = read(pfd[0], out + got, sizeof(out) - 1 - got)) > 0)
		got += (size_t)n;
	out[got] = '\0';
	(void)close(pfd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return (-1);

	if ((nl = strchr(out, '\n')) != NULL)
		*nl = '\0';
	sp = strchr(out, ' ');
	if (sp != NULL)
		*sp++ = '\0';
	(void)strlcpy(type, out, tlen);
	if (sp != NULL) {
		while (*sp == ' ')
			sp++;
		(void)strlcpy(label, sp, llen);
	}
	return (type[0] != '\0' ? 0 : -1);
}

/*
 * UDF (ECMA-167): a Volume Recognition Sequence of 2048-byte descriptors
 * from byte 32768, with an "NSR02"/"NSR03" descriptor. fstyp has no UDF
 * probe, so read it here. The label (the logical volume identifier) needs
 * the full descriptor walk; it is left empty, and the daemon names the
 * volume "Untitled" (Apple DAMount.c).
 */
static int
probe_udf(const char *path)
{
	uint8_t d[8];
	int fd, i, found = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	for (i = 0; i < 16; i++) {
		if (read_at(fd, 32768 + (off_t)i * 2048, d, sizeof(d)) != 0)
			break;
		if (memcmp(d + 1, "NSR02", 5) == 0 ||
		    memcmp(d + 1, "NSR03", 5) == 0) {
			found = 1;
			break;
		}
		if (d[1] == '\0')	/* end of the sequence */
			break;
	}
	(void)close(fd);
	return (found ? 0 : -1);
}

static int
cmd_probe(const struct fstype *ft, const char *dev)
{
	char path[MAXPATHLEN], type[64], label[512];

	dev_path(dev, path, sizeof(path));
	if (strcmp(ft->vfs, "udf") == 0)
		return (probe_udf(path) == 0 ? FSUR_RECOGNIZED :
		    FSUR_UNRECOGNIZED);
	if (run_fstyp(path, type, sizeof(type), label, sizeof(label)) != 0 ||
	    strcmp(type, ft->vfs) != 0)
		return (FSUR_UNRECOGNIZED);
	if (label[0] != '\0')
		printf("%s\n", label);
	return (FSUR_RECOGNIZED);
}

/* --- -k: UUID ------------------------------------------------------------- */

/* Trailing partition index of a provider name: da0p3 -> 3, da0s1a -> 1. */
static int
partition_index(const char *dev)
{
	const char *p = dev + strlen(dev);

	while (p > dev && isdigit((unsigned char)p[-1]))
		p--;
	if (p == dev || *p == '\0')
		return (0);
	if (p[-1] != 'p' && p[-1] != 's')
		return (0);
	return (atoi(p));
}

static int
cmd_uuid(const struct fstype *ft, const char *dev)
{
	char path[MAXPATHLEN], ident[DISK_IDENT_SIZE], name[1024];
	char type[64], label[512];
	const char *base, *env;
	uint8_t digest[20];
	SHA1_CTX ctx;
	int fd;

	env = getenv("DA_PARTITION_UUID");
	if (env != NULL && strlen(env) == 36) {
		printf("%s\n", env);
		return (FSUR_IO_SUCCESS);
	}

	dev_path(dev, path, sizeof(path));
	ident[0] = '\0';
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (FSUR_IO_FAIL);
	if (ioctl(fd, DIOCGIDENT, ident) != 0)
		ident[0] = '\0';
	ident[sizeof(ident) - 1] = '\0';
	(void)close(fd);

	label[0] = '\0';
	if (strcmp(ft->vfs, "udf") != 0)
		(void)run_fstyp(path, type, sizeof(type), label, sizeof(label));
	base = strrchr(path, '/') + 1;
	(void)snprintf(name, sizeof(name), "%s:%d:%s",
	    ident[0] != '\0' ? ident : base, partition_index(base), label);

	/* RFC 4122 version 5: SHA-1 over namespace + name. */
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, uuid_ns, sizeof(uuid_ns));
	SHA1_Update(&ctx, name, strlen(name));
	SHA1_Final(digest, &ctx);
	digest[6] = (digest[6] & 0x0f) | 0x50;
	digest[8] = (digest[8] & 0x3f) | 0x80;
	printf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
	    "%02X%02X%02X%02X%02X%02X\n",
	    digest[0], digest[1], digest[2], digest[3], digest[4], digest[5],
	    digest[6], digest[7], digest[8], digest[9], digest[10], digest[11],
	    digest[12], digest[13], digest[14], digest[15]);
	return (FSUR_IO_SUCCESS);
}

/* --- -q: is the volume clean? --------------------------------------------- */

/* UFS: the superblock's fs_clean, read through libufs (read-only). */
static int
clean_ufs(const char *path)
{
	struct uufsd disk;
	int clean;

	if (ufs_disk_fillout(&disk, path) == -1)
		return (-1);
	clean = disk.d_fs.fs_clean != 0;
	(void)ufs_disk_close(&disk);
	return (clean);
}

/*
 * FAT: the clean-shutdown bit in FAT[1] (FAT16 bit 15, FAT32 bit 27; set
 * means clean). FAT12 has none.
 */
static int
clean_msdos(int fd)
{
	uint8_t bpb[512], ent[4];
	uint32_t bps, spc, rsvd, nfats, rootents, tot, fatsz, rootsecs;
	uint32_t datasecs, clusters;

	if (read_at(fd, 0, bpb, sizeof(bpb)) != 0)
		return (-1);
	bps = le16(bpb + 11);
	spc = bpb[13];
	rsvd = le16(bpb + 14);
	nfats = bpb[16];
	rootents = le16(bpb + 17);
	tot = le16(bpb + 19) != 0 ? le16(bpb + 19) : le32(bpb + 32);
	fatsz = le16(bpb + 22) != 0 ? le16(bpb + 22) : le32(bpb + 36);
	if (bps == 0 || spc == 0 || nfats == 0)
		return (-1);
	rootsecs = (rootents * 32 + bps - 1) / bps;
	if (tot < rsvd + nfats * fatsz + rootsecs)
		return (-1);
	datasecs = tot - (rsvd + nfats * fatsz + rootsecs);
	clusters = datasecs / spc;

	if (clusters < 4085)			/* FAT12 */
		return (1);
	if (read_at(fd, (off_t)rsvd * bps, ent, sizeof(ent)) != 0)
		return (-1);
	if (clusters < 65525)			/* FAT16: FAT[1] at byte 2 */
		return ((le16(ent + 2) & 0x8000) != 0);
	return ((le32(ent + 4) & 0x08000000) != 0);	/* FAT32 */
}

/* exFAT: VolumeFlags (boot sector offset 106), bit 1 = VolumeDirty. */
static int
clean_exfat(int fd)
{
	uint8_t bs[512];

	if (read_at(fd, 0, bs, sizeof(bs)) != 0)
		return (-1);
	if (memcmp(bs + 3, "EXFAT   ", 8) != 0)
		return (-1);
	return ((le16(bs + 106) & 0x0002) == 0);
}

static int
cmd_verify(const struct fstype *ft, const char *dev)
{
	char path[MAXPATHLEN];
	int fd, clean;

	dev_path(dev, path, sizeof(path));
	if (strcmp(ft->vfs, "ufs") == 0) {
		clean = clean_ufs(path);
	} else if (strcmp(ft->vfs, "msdosfs") == 0 ||
	    strcmp(ft->vfs, "exfat") == 0) {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return (FSUR_IO_FAIL);
		clean = strcmp(ft->vfs, "exfat") == 0 ? clean_exfat(fd) :
		    clean_msdos(fd);
		(void)close(fd);
	} else if (strcmp(ft->vfs, "ntfs") == 0) {
		/*
		 * The $Volume dirty flag needs an MFT walk. Report dirty and
		 * leave the decision to ntfs-3g, which refuses a dirty volume
		 * read-write on its own.
		 */
		clean = 0;
	} else {
		clean = 1;			/* cd9660, udf: read-only media */
	}
	if (clean < 0)
		return (FSUR_IO_FAIL);
	return (clean ? 0 : 1);
}

/* --- -y: repair ----------------------------------------------------------- */

static int
cmd_repair(const struct fstype *ft, const char *dev)
{
	char path[MAXPATHLEN];

	if (ft->fsck == NULL)
		return (FSUR_INVAL);
	dev_path(dev, path, sizeof(path));
	execl(ft->fsck, ft->fsck, "-y", path, (char *)NULL);
	fprintf(stderr, "%s: %s: %s\n", ft->bundle, ft->fsck, strerror(errno));
	return (FSUR_IO_FAIL);
}

int
main(int argc, char **argv)
{
	const struct fstype *ft = NULL;
	char prog[MAXPATHLEN], *p;
	size_t i;

	(void)strlcpy(prog, argv[0], sizeof(prog));
	if ((p = strrchr(prog, '/')) != NULL)
		memmove(prog, p + 1, strlen(p + 1) + 1);
	if ((p = strstr(prog, FS_UTIL_SUFFIX)) != NULL)
		*p = '\0';
	for (i = 0; i < nitems(fstypes); i++)
		if (strcmp(prog, fstypes[i].bundle) == 0)
			ft = &fstypes[i];
	if (ft == NULL) {
		fprintf(stderr, "fsutil: run as <name>.util "
		    "(msdos, exfat, ntfs, ufs, cd9660, udf)\n");
		return (FSUR_INVAL);
	}
	if (argc < 3 || argv[1][0] != '-' || argv[1][1] == '\0' ||
	    argv[1][2] != '\0')
		usage(argv[0]);

	switch (argv[1][1]) {
	case FSUC_PROBE:
		return (cmd_probe(ft, argv[2]));
	case FSUC_GETUUID:
		return (cmd_uuid(ft, argv[2]));
	case FSUC_QUICKVERIFY:
		return (cmd_verify(ft, argv[2]));
	case FSUC_REPAIR:
		return (cmd_repair(ft, argv[2]));
	default:
		usage(argv[0]);
	}
	return (FSUR_INVAL);
}
