/*
 * DAGeom.c — build the DADisk model from GEOM (E15 C2, nextbsd-userland#192).
 *
 * Apple's DADiskCreateFromIOMedia (diskarbitrationd/DADisk.c) reads one
 * IOMedia object, its IOBlockStorageDevice parent and its device-tree bus
 * node. NextBSD has no IOMedia: the media objects are GEOM providers, so this
 * file is that function with libgeom and CAM as the sources:
 *
 *   IOMedia (whole)          DISK-class provider        da0, vtbd0, cd0
 *   IOMedia (partition)      PART-class provider        da0p1, da0s1a
 *   IOPartitionScheme        PART geom on a provider    <scheme> GPT/MBR/BSD
 *   IOMedia "Content Hint"   PART provider <rawtype>    upper-cased GPT GUID,
 *                                                       Apple FDisk names
 *   IOBlockStorageDevice     GEOM::attachment + descr   vtblk0, ahcich0
 *   device-tree bus node     registry parent of that    /dev/ioregistry
 *   dev-name link            LABEL-class providers      /dev/gpt/EFI
 *
 * Disks are created, described, logged and removed here; each walk then
 * signals the stage machine (DAStage.c), which probes (#194) and mounts
 * (#195) the new ones.
 */
#include "DAGeom.h"
#include "DADisk.h"
#include "DAStage.h"
#include "DAVolume.h"
#include "da_iokit_subscribe.h"

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgeom.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static dispatch_queue_t	g_da_queue;
static atomic_uint	g_event_gen;
static uint32_t		g_walk_gen;
static int		g_geom_ok_logged;
static int		g_volumes_seeded;

/*
 * Walk delays after the last storage event. The registry event is the
 * newbus attach (umass0, ahcich0); CAM then probes the LUN and GEOM tastes
 * the new disk, which can take seconds on USB. Each walk only logs what
 * changed, so walking again is cheap and idempotent.
 */
static const int64_t	settle_ms[] = { 250, 1000, 3000, 8000 };

/* --- small helpers -------------------------------------------------------- */

static const char *
gconf_get(struct gconf *conf, const char *name)
{
	struct gconfig *c;

	LIST_FOREACH(c, conf, lg_config)
		if (c->lg_name != NULL && strcmp(c->lg_name, name) == 0)
			return (c->lg_val != NULL ? c->lg_val : "");
	return (NULL);
}

/* Leading letters of a provider or newbus name: "da0p1" -> "da". */
static void
name_prefix(const char *name, char *buf, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len && name[i] != '\0' &&
	    !isdigit((unsigned char)name[i]); i++)
		buf[i] = name[i];
	buf[i] = '\0';
}

/* First run of digits in a name: "da12p3" -> 12, "mmcsd0boot0" -> 0. */
static int
name_unit(const char *name)
{
	while (*name != '\0' && !isdigit((unsigned char)*name))
		name++;
	if (*name == '\0')
		return (-1);
	return ((int)strtol(name, NULL, 10));
}

static void
strip_unit(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && isdigit((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

static void
trim(char *s)
{
	size_t n;
	char *p = s;

	while (*p == ' ' || *p == '\t')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

/* --- content hints -------------------------------------------------------- */

/*
 * Apple's IOFDiskPartitionScheme "Content Table" (IOStorageFamily
 * Info.plist), shipped verbatim; anything else is "0x%02X" as in
 * IOFDiskPartitionScheme.cpp.
 */
static const struct {
	unsigned	type;
	const char	*hint;
} fdisk_content[] = {
	{   1, "DOS_FAT_12" },
	{   4, "DOS_FAT_16_S" },
	{   6, "DOS_FAT_16" },
	{   7, "Windows_NTFS" },
	{  11, "DOS_FAT_32" },
	{  12, "Windows_FAT_32" },
	{  14, "Windows_FAT_16" },
	{  20, "DOS_FAT_16_S_Hidden" },
	{  22, "DOS_FAT_16_Hidden" },
	{  66, "Windows_LDM" },
	{ 130, "Linux_Swap" },
	{ 131, "Linux" },
	{ 142, "Linux_LVM" },
	{ 165, "FreeBSD" },
	{ 166, "OpenBSD" },
	{ 167, "Apple_Rhapsody_UFS" },
	{ 168, "Apple_UFS" },
	{ 169, "NetBSD" },
	{ 171, "Apple_Boot" },
	{ 174, "Apple_Encrypted" },
	{ 175, "Apple_HFS" },
	{ 253, "Linux_RAID" },
};

static int
looks_like_uuid(const char *s)
{
	size_t i;

	if (strlen(s) != 36)
		return (0);
	for (i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (s[i] != '-')
				return (0);
		} else if (!isxdigit((unsigned char)s[i]))
			return (0);
	}
	return (1);
}

/*
 * DAMediaContent of a partition, from its table's scheme and the entry's
 * <rawtype>. GPT GUIDs are upper-cased: gpart prints them lower case
 * (kern_uuid.c), Apple's IOGUIDPartitionScheme uses uuid_unparse (upper
 * case on Darwin), and every fs-bundle match key is upper case.
 */
static void
part_content(const char *scheme, const char *rawtype, const char *type,
    char *buf, size_t len)
{
	size_t i;

	if (rawtype == NULL || rawtype[0] == '\0') {
		(void)strlcpy(buf, type != NULL ? type : "", len);
		return;
	}
	if (looks_like_uuid(rawtype)) {			/* GPT, BSD64 */
		for (i = 0; i + 1 < len && rawtype[i] != '\0'; i++)
			buf[i] = (char)toupper((unsigned char)rawtype[i]);
		buf[i] = '\0';
		return;
	}
	if (scheme != NULL &&
	    (strcmp(scheme, "MBR") == 0 || strcmp(scheme, "EBR") == 0)) {
		unsigned t = (unsigned)strtoul(rawtype, NULL, 10);

		for (i = 0; i < nitems(fdisk_content); i++) {
			if (fdisk_content[i].type == t) {
				(void)strlcpy(buf, fdisk_content[i].hint, len);
				return;
			}
		}
		(void)snprintf(buf, len, "0x%02X", t);
		return;
	}
	if (scheme != NULL && strcmp(scheme, "BSD") == 0) {
		/* No Apple equivalent: NextBSD-local names for disklabel fstypes. */
		unsigned t = (unsigned)strtoul(rawtype, NULL, 10);

		if (t == 7)
			(void)strlcpy(buf, "BSD_FFS", len);
		else if (t == 1)
			(void)strlcpy(buf, "BSD_Swap", len);
		else
			(void)snprintf(buf, len, "BSD_0x%02X", t);
		return;
	}
	(void)strlcpy(buf, rawtype, len);		/* APM: "Apple_HFS" */
}

/* DAMediaContent of media carrying a partition table: Apple's scheme names. */
static void
scheme_content(const char *scheme, char *buf, size_t len)
{
	if (scheme == NULL)
		scheme = "";
	if (strcmp(scheme, "GPT") == 0)
		(void)strlcpy(buf, "GUID_partition_scheme", len);
	else if (strcmp(scheme, "MBR") == 0 || strcmp(scheme, "EBR") == 0)
		(void)strlcpy(buf, "FDisk_partition_scheme", len);
	else if (strcmp(scheme, "APM") == 0)
		(void)strlcpy(buf, "Apple_partition_scheme", len);
	else if (strcmp(scheme, "BSD") == 0 || strcmp(scheme, "BSD64") == 0)
		(void)strlcpy(buf, "BSD_partition_scheme", len);
	else
		(void)snprintf(buf, len, "%s_partition_scheme", scheme);
}

/* --- device / bus (the IOBlockStorageDevice side) ------------------------- */

/*
 * Apple's kIOPropertyPhysicalInterconnectType / Location per storage driver.
 * `driver` is the newbus driver the disk attaches through (GEOM::attachment
 * with the unit and CAM's "-sim" suffix removed).
 */
static const struct {
	const char	*driver;
	const char	*protocol;
	int		internal;
} bus_table[] = {
	{ "umass",	"USB",			0 },
	{ "ahcich",	"SATA",			1 },
	{ "ata",	"ATA",			1 },
	{ "nvme",	"PCI-Express",		1 },
	{ "mmc",	"Secure Digital",	1 },
	{ "mmcsd",	"Secure Digital",	1 },
	{ "vtblk",	"Virtual Interface",	1 },
	{ "vtscsi",	"Virtual Interface",	1 },
};

struct da_bus_info {
	char	nameunit[64];		/* newbus node: vtblk0, umass0, mmcsd0 */
	char	driver[64];		/* vtblk, umass, mmcsd */
	int	unit;			/* DADeviceUnit */
	char	physpath[MAXPATHLEN];	/* DIOCGPHYSPATH, "" if unset */
	char	regpath[512];		/* registry path of nameunit */
	char	busname[64];		/* registry parent's driver name */
	char	buspath[512];		/* registry parent's path */
};

/*
 * Read GEOM::attachment and the physical path from the whole disk and find
 * its newbus node in the kernel registry.
 */
static void
disk_bus_info(const char *name, struct da_bus_info *bi)
{
	struct diocgattr_arg attr;
	char dev[MAXPATHLEN];
	char *p;
	int fd;

	memset(bi, 0, sizeof(*bi));
	bi->unit = -1;

	(void)snprintf(dev, sizeof(dev), "/dev/%s", name);
	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		memset(&attr, 0, sizeof(attr));
		(void)strlcpy(attr.name, "GEOM::attachment", sizeof(attr.name));
		attr.len = sizeof(attr.value.str);
		if (ioctl(fd, DIOCGATTR, &attr) == 0) {
			attr.value.str[sizeof(attr.value.str) - 1] = '\0';
			(void)strlcpy(bi->nameunit, attr.value.str,
			    sizeof(bi->nameunit));
		}
		if (ioctl(fd, DIOCGPHYSPATH, bi->physpath) != 0)
			bi->physpath[0] = '\0';
		bi->physpath[sizeof(bi->physpath) - 1] = '\0';
		(void)close(fd);
	}

	/* CAM SIMs attach as "<driver>-sim<unit>" (umass-sim0 -> umass0). */
	if ((p = strstr(bi->nameunit, "-sim")) != NULL &&
	    isdigit((unsigned char)p[4]))
		memmove(p, p + 4, strlen(p + 4) + 1);

	/* mmcsd sets no attachment; the disk is its own newbus node. */
	if (bi->nameunit[0] == '\0' && strncmp(name, "mmcsd", 5) == 0 &&
	    name_unit(name) >= 0)
		(void)snprintf(bi->nameunit, sizeof(bi->nameunit), "mmcsd%d",
		    name_unit(name));

	if (bi->nameunit[0] == '\0')
		return;
	(void)strlcpy(bi->driver, bi->nameunit, sizeof(bi->driver));
	strip_unit(bi->driver);
	bi->unit = name_unit(bi->nameunit + strlen(bi->driver));

	if (da_iokit_copy_node_path(bi->nameunit, bi->regpath,
	    sizeof(bi->regpath)) == 0 && (p = strrchr(bi->regpath, '/')) != NULL &&
	    p != strchr(bi->regpath, '/')) {
		size_t plen = (size_t)(p - bi->regpath);
		const char *parent;

		if (plen < sizeof(bi->buspath)) {
			memcpy(bi->buspath, bi->regpath, plen);
			bi->buspath[plen] = '\0';
			parent = strrchr(bi->buspath, '/');
			(void)strlcpy(bi->busname, parent + 1,
			    sizeof(bi->busname));
			strip_unit(bi->busname);
		}
	}
}

/* CAM's removable-media bit for da<unit> (scsi_da.c DA_FLAG_PACK_REMOVABLE). */
static int
da_pack_removable(int unit)
{
	char oid[64], flags[512];
	size_t len = sizeof(flags) - 1;

	(void)snprintf(oid, sizeof(oid), "kern.cam.da.%d.flags", unit);
	if (sysctlbyname(oid, flags, &len, NULL, 0) != 0)
		return (0);
	flags[len] = '\0';
	return (strstr(flags, "PACK_REMOVABLE") != NULL);
}

/*
 * DAMediaWritable for a whole disk. An open writer (the provider's access
 * mode) settles it; otherwise try O_RDWR: EROFS is a write-protected disk
 * (geom_disk's DISKFLAG_WRITE_PROTECT). Other failures (an exclusive
 * holder) say nothing about the media, so they count as writable.
 */
static int
disk_writable(const char *name, const char *prefix, const char *mode)
{
	char dev[MAXPATHLEN];
	int r = 0, w = 0, e = 0, fd;

	if (strcmp(prefix, "cd") == 0)
		return (0);
	if (mode != NULL && sscanf(mode, "r%dw%de%d", &r, &w, &e) == 3 && w > 0)
		return (1);
	(void)snprintf(dev, sizeof(dev), "/dev/%s", name);
	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		(void)close(fd);
		return (1);
	}
	return (errno != EROFS && errno != EACCES);
}

/* --- building one disk ---------------------------------------------------- */

static void
set_bsd_numbers(struct da_disk *disk, const char *name)
{
	char dev[MAXPATHLEN];
	struct stat st;

	(void)snprintf(dev, sizeof(dev), "/dev/%s", name);
	if (stat(dev, &st) == 0) {
		DADiskSetDescriptionInt32(disk,
		    kDADiskDescriptionMediaBSDMajorKey, (int32_t)major(st.st_rdev));
		DADiskSetDescriptionInt32(disk,
		    kDADiskDescriptionMediaBSDMinorKey, (int32_t)minor(st.st_rdev));
	}
}

static void
set_common(struct da_disk *disk, struct gprovider *pp, int whole, int leaf,
    const char *content)
{
	DADiskSetDescriptionString(disk, kDADiskDescriptionMediaBSDNameKey,
	    pp->lg_name);
	set_bsd_numbers(disk, pp->lg_name);
	DADiskSetDescriptionInt64(disk, kDADiskDescriptionMediaSizeKey,
	    (int64_t)pp->lg_mediasize);
	DADiskSetDescriptionInt32(disk, kDADiskDescriptionMediaBlockSizeKey,
	    (int32_t)pp->lg_sectorsize);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionMediaWholeKey, whole);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionMediaLeafKey, leaf);
	DADiskSetDescriptionString(disk, kDADiskDescriptionMediaContentKey,
	    content);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionVolumeNetworkKey, 0);
	DADiskSetDescriptionDouble(disk, kDADiskDescriptionAppearanceTimeKey,
	    CFAbsoluteTimeGetCurrent());
}

static void
describe_whole(struct da_disk *disk, struct gprovider *pp, int leaf,
    const char *content)
{
	struct da_bus_info bi;
	char prefix[16], descr[256], media_path[1024];
	const char *s;
	int i, unit, removable;

	name_prefix(pp->lg_name, prefix, sizeof(prefix));
	unit = name_unit(pp->lg_name);

	set_common(disk, pp, 1, leaf, content);
	if (unit >= 0)
		DADiskSetDescriptionInt32(disk,
		    kDADiskDescriptionMediaBSDUnitKey, unit);
	DADiskSetDescriptionString(disk, kDADiskDescriptionMediaKindKey,
	    strcmp(prefix, "cd") == 0 ? "IOCDMedia" : "IOMedia");

	if (strcmp(prefix, "cd") == 0 || strcmp(prefix, "mmcsd") == 0)
		removable = 1;
	else if (strcmp(prefix, "da") == 0)
		removable = da_pack_removable(unit);
	else
		removable = 0;
	DADiskSetDescriptionBool(disk, kDADiskDescriptionMediaRemovableKey,
	    removable);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionMediaEjectableKey,
	    removable);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionMediaWritableKey,
	    disk_writable(pp->lg_name, prefix, pp->lg_mode));

	/* GEOM::descr: CAM builds it as "<vendor> <product>" for da/cd. */
	descr[0] = '\0';
	if ((s = gconf_get(&pp->lg_config, "descr")) != NULL)
		(void)strlcpy(descr, s, sizeof(descr));
	trim(descr);
	if (descr[0] != '\0') {
		char *sp = strchr(descr, ' ');

		DADiskSetDescriptionString(disk, kDADiskDescriptionMediaNameKey,
		    descr);
		if ((strcmp(prefix, "da") == 0 || strcmp(prefix, "cd") == 0) &&
		    sp != NULL) {
			*sp++ = '\0';
			trim(sp);
			DADiskSetDescriptionString(disk,
			    kDADiskDescriptionDeviceVendorKey, descr);
			DADiskSetDescriptionString(disk,
			    kDADiskDescriptionDeviceModelKey, sp);
		} else {
			DADiskSetDescriptionString(disk,
			    kDADiskDescriptionDeviceModelKey, descr);
		}
	}

	disk_bus_info(pp->lg_name, &bi);
	if (bi.driver[0] != '\0') {
		disk->bus_driver = strdup(bi.driver);
		for (i = 0; i < (int)nitems(bus_table); i++) {
			if (strcmp(bus_table[i].driver, bi.driver) != 0)
				continue;
			DADiskSetDescriptionBool(disk,
			    kDADiskDescriptionDeviceInternalKey,
			    bus_table[i].internal);
			DADiskSetDescriptionString(disk,
			    kDADiskDescriptionDeviceProtocolKey,
			    bus_table[i].protocol);
			break;
		}
	}
	if (bi.unit >= 0)
		DADiskSetDescriptionInt32(disk, kDADiskDescriptionDeviceUnitKey,
		    bi.unit);
	if (bi.regpath[0] != '\0') {
		disk->ioreg_path = strdup(bi.regpath);
		DADiskSetDescriptionString(disk,
		    kDADiskDescriptionDevicePathKey, bi.regpath);
	}
	if (bi.busname[0] != '\0') {
		DADiskSetDescriptionString(disk, kDADiskDescriptionBusNameKey,
		    bi.busname);
		DADiskSetDescriptionString(disk, kDADiskDescriptionBusPathKey,
		    bi.buspath);
	}

	/* DAMediaPath: the physical path when the driver sets one, else the
	 * IOMedia-style path under the registry node. */
	media_path[0] = '\0';
	if (bi.physpath[0] != '\0')
		(void)strlcpy(media_path, bi.physpath, sizeof(media_path));
	else if (bi.regpath[0] != '\0')
		(void)snprintf(media_path, sizeof(media_path), "%s/%s",
		    bi.regpath, pp->lg_name);
	if (media_path[0] != '\0')
		DADiskSetDescriptionString(disk, kDADiskDescriptionMediaPathKey,
		    media_path);
}

/* Keys a partition takes from the device it sits on. */
static void
inherit_from_whole(struct da_disk *disk, const struct da_disk *whole)
{
	CFStringRef keys[] = {
		kDADiskDescriptionMediaBSDUnitKey,
		kDADiskDescriptionMediaRemovableKey,
		kDADiskDescriptionMediaEjectableKey,
		kDADiskDescriptionMediaWritableKey,
		kDADiskDescriptionDeviceVendorKey,
		kDADiskDescriptionDeviceModelKey,
		kDADiskDescriptionDeviceRevisionKey,
		kDADiskDescriptionDeviceInternalKey,
		kDADiskDescriptionDeviceProtocolKey,
		kDADiskDescriptionDevicePathKey,
		kDADiskDescriptionDeviceUnitKey,
		kDADiskDescriptionBusNameKey,
		kDADiskDescriptionBusPathKey,
	};
	size_t i;

	for (i = 0; i < nitems(keys); i++)
		DADiskSetDescription(disk, keys[i],
		    DADiskGetDescription(whole, keys[i]));
	if (whole->bus_driver != NULL)
		disk->bus_driver = strdup(whole->bus_driver);
	if (whole->ioreg_path != NULL)
		disk->ioreg_path = strdup(whole->ioreg_path);
}

static void
describe_part(struct da_disk *disk, struct gprovider *pp, int leaf,
    const char *content, const struct da_disk *parent)
{
	char parent_path[1024];
	const char *label, *type, *rawuuid;
	CFStringRef s;
	CFUUIDRef uuid;

	set_common(disk, pp, 0, leaf, content);
	DADiskSetDescriptionString(disk, kDADiskDescriptionMediaKindKey,
	    "IOMedia");
	inherit_from_whole(disk, disk->whole);

	rawuuid = gconf_get(&pp->lg_config, "rawuuid");
	if (rawuuid != NULL && looks_like_uuid(rawuuid)) {
		s = CFStringCreateWithCString(kCFAllocatorDefault, rawuuid,
		    kCFStringEncodingUTF8);
		if (s != NULL) {
			uuid = CFUUIDCreateFromString(kCFAllocatorDefault, s);
			if (uuid != NULL) {
				DADiskSetDescription(disk,
				    kDADiskDescriptionMediaUUIDKey, uuid);
				CFRelease(uuid);
			}
			CFRelease(s);
		}
	}

	/* DAMediaName: the GPT label, else gpart's type alias. */
	label = gconf_get(&pp->lg_config, "label");
	type = gconf_get(&pp->lg_config, "type");
	if (label != NULL && label[0] != '\0')
		DADiskSetDescriptionString(disk, kDADiskDescriptionMediaNameKey,
		    label);
	else if (type != NULL && type[0] != '\0')
		DADiskSetDescriptionString(disk, kDADiskDescriptionMediaNameKey,
		    type);

	/* DAMediaPath nests like IOMedia: <parent media path>/<name>. */
	if (DADiskGetDescriptionCString(parent, kDADiskDescriptionMediaPathKey,
	    parent_path, sizeof(parent_path)) == 0) {
		char path[1280];

		(void)snprintf(path, sizeof(path), "%s/%s", parent_path,
		    pp->lg_name);
		DADiskSetDescriptionString(disk, kDADiskDescriptionMediaPathKey,
		    path);
	}
}

/* --- the walk ------------------------------------------------------------- */

struct da_walk {
	struct gclass	*part_class;
	struct gclass	*label_class;
	int		created;
	int		removed;
};

static void
remove_disk(struct da_disk *disk)
{
	struct da_disk *d;

	/* A whole disk takes its partitions with it. */
	if (disk->whole == NULL) {
restart:
		TAILQ_FOREACH(d, &gDADiskList, link) {
			if (d->whole == disk) {
				DALog("DA-DISK-GONE: %s", d->id);
				DADiskListRemove(d);
				goto restart;
			}
		}
	}
	DALog("DA-DISK-GONE: %s", disk->id);
	DADiskListRemove(disk);
}

/* The PART geom consuming `pp`, if `pp` carries a partition table. */
static struct ggeom *
part_geom_on(struct da_walk *w, struct gprovider *pp)
{
	struct gconsumer *cp;

	LIST_FOREACH(cp, &pp->lg_consumers, lg_consumers)
		if (cp->lg_geom != NULL && cp->lg_geom->lg_class == w->part_class)
			return (cp->lg_geom);
	return (NULL);
}

/* /dev/<alias> for every LABEL-class provider stacked on `pp`. */
static int
label_links(struct da_walk *w, struct gprovider *pp, char **links, int max)
{
	struct gconsumer *cp;
	struct gprovider *lp;
	int n = 0;

	LIST_FOREACH(cp, &pp->lg_consumers, lg_consumers) {
		if (cp->lg_geom == NULL || cp->lg_geom->lg_class != w->label_class)
			continue;
		LIST_FOREACH(lp, &cp->lg_geom->lg_provider, lg_provider) {
			if (n >= max)
				return (n);
			if (asprintf(&links[n], "/dev/%s", lp->lg_name) > 0)
				n++;
		}
	}
	return (n);
}

static int
provider_cmp(const void *a, const void *b)
{
	const struct gprovider *pa = *(struct gprovider *const *)a;
	const struct gprovider *pb = *(struct gprovider *const *)b;

	return (strcmp(pa->lg_name, pb->lg_name));
}

/* The providers of one list as an array sorted by name; caller frees. */
static int
sorted_providers(void *head, struct gprovider ***out, size_t *count)
{
	LIST_HEAD(, gprovider) *list = head;
	struct gprovider *pp, **v;
	size_t n = 0;

	LIST_FOREACH(pp, list, lg_provider)
		n++;
	v = calloc(n + 1, sizeof(*v));
	if (v == NULL)
		return (-1);
	n = 0;
	LIST_FOREACH(pp, list, lg_provider)
		v[n++] = pp;
	qsort(v, n, sizeof(*v), provider_cmp);
	*out = v;
	*count = n;
	return (0);
}

static void
visit_provider(struct da_walk *w, struct gprovider *pp,
    struct da_disk *whole, struct da_disk *parent)
{
	struct ggeom *table = part_geom_on(w, pp);
	struct da_disk *disk;
	char id[MAXPATHLEN], content[128], sig[256];
	char *links[DA_DISK_LINK_MAX];
	const char *rawuuid = NULL;
	int nlinks, i;

	(void)snprintf(id, sizeof(id), "/dev/%s", pp->lg_name);

	if (whole == NULL) {
		if (table != NULL)
			scheme_content(gconf_get(&table->lg_config, "scheme"),
			    content, sizeof(content));
		else
			content[0] = '\0';
	} else {
		part_content(gconf_get(&pp->lg_geom->lg_config, "scheme"),
		    gconf_get(&pp->lg_config, "rawtype"),
		    gconf_get(&pp->lg_config, "type"), content,
		    sizeof(content));
		rawuuid = gconf_get(&pp->lg_config, "rawuuid");
	}
	(void)snprintf(sig, sizeof(sig), "%jd|%u|%s|%s|%d",
	    (intmax_t)pp->lg_mediasize, pp->lg_sectorsize, content,
	    rawuuid != NULL ? rawuuid : "", table == NULL);

	disk = DADiskListFind(id);
	if (disk != NULL && disk->volume_only) {
		/*
		 * A mount of this provider was adopted before the walk saw it
		 * (#193). Drop the stand-in; the mount refresh after this walk
		 * moves DAVolumePath onto the media disk.
		 */
		DALog("DA-VOLUME-GONE: %s (now media)", disk->id);
		DADiskListRemove(disk);
		disk = NULL;
	}
	if (disk != NULL && (strcmp(disk->signature, sig) != 0 ||
	    disk->whole != whole)) {
		/* Same name, different media (new table, resized, swapped). */
		remove_disk(disk);
		w->removed++;
		disk = NULL;
	}

	nlinks = label_links(w, pp, links, DA_DISK_LINK_MAX);

	if (disk == NULL) {
		disk = DADiskCreate(id, whole == NULL ? "DISK" : "PART");
		if (disk == NULL) {
			DALog("DA-GEOM: out of memory creating %s", id);
			goto out;
		}
		disk->signature = strdup(sig);
		disk->whole = whole;
		if (whole == NULL)
			describe_whole(disk, pp, table == NULL, content);
		else
			describe_part(disk, pp, table == NULL, content, parent);
		(void)DADiskSetDeviceLinks(disk, links, nlinks);
		disk->walk_gen = g_walk_gen;
		TAILQ_INSERT_TAIL(&gDADiskList, disk, link);
		w->created++;
		DADiskLog(disk);
	} else {
		disk->walk_gen = g_walk_gen;
		/* LABEL tastes after PART and withers aliases on open. */
		if (DADiskSetDeviceLinks(disk, links, nlinks)) {
			char buf[512];

			buf[0] = '\0';
			for (i = 0; i < DA_DISK_LINK_MAX &&
			    disk->device_link[i] != NULL; i++) {
				if (i > 0)
					(void)strlcat(buf, ",", sizeof(buf));
				(void)strlcat(buf, disk->device_link[i],
				    sizeof(buf));
			}
			DALog("DA-DISK-LINKS: %s links=%s", disk->id,
			    buf[0] != '\0' ? buf : "-");
		}
	}

	if (table != NULL) {
		struct gprovider **kids;
		size_t nkids;

		/* libgeom lists providers newest-first; walk them by name. */
		if (sorted_providers(&table->lg_provider, &kids, &nkids) == 0) {
			for (i = 0; i < (int)nkids; i++)
				visit_provider(w, kids[i],
				    whole != NULL ? whole : disk, disk);
			free(kids);
		}
	}
out:
	for (i = 0; i < nlinks; i++)
		free(links[i]);
}

static void
da_geom_walk(void)
{
	struct gmesh mesh;
	struct gclass *cls, *disk_class = NULL;
	struct ggeom *gp;
	struct gprovider *pp;
	struct da_disk *disk;
	struct da_walk w;
	int error, total, wholes;

	error = geom_gettree(&mesh);
	if (error != 0) {
		DALog("DA-GEOM-FAIL: geom_gettree: %s", strerror(error));
		if (!g_volumes_seeded) {
			g_volumes_seeded = 1;
			DAVolumeRefresh();
		}
		DAStageSignal();
		return;
	}

	memset(&w, 0, sizeof(w));
	LIST_FOREACH(cls, &mesh.lg_class, lg_class) {
		if (strcmp(cls->lg_name, "DISK") == 0)
			disk_class = cls;
		else if (strcmp(cls->lg_name, "PART") == 0)
			w.part_class = cls;
		else if (strcmp(cls->lg_name, "LABEL") == 0)
			w.label_class = cls;
	}

	g_walk_gen++;
	if (disk_class != NULL) {
		struct gprovider **disks;
		size_t ndisks = 0, i;

		/* One provider per DISK geom; gather them all, then sort. */
		LIST_FOREACH(gp, &disk_class->lg_geom, lg_geom)
			LIST_FOREACH(pp, &gp->lg_provider, lg_provider)
				ndisks++;
		disks = calloc(ndisks + 1, sizeof(*disks));
		if (disks != NULL) {
			ndisks = 0;
			LIST_FOREACH(gp, &disk_class->lg_geom, lg_geom)
				LIST_FOREACH(pp, &gp->lg_provider, lg_provider)
					disks[ndisks++] = pp;
			qsort(disks, ndisks, sizeof(*disks), provider_cmp);
			for (i = 0; i < ndisks; i++) {
				/* No media (empty cd0, card reader): no IOMedia. */
				if (disks[i]->lg_mediasize <= 0)
					continue;
				visit_provider(&w, disks[i], NULL, NULL);
			}
			free(disks);
		} else {
			/* Out of memory: keep the model as it is. */
			geom_deletetree(&mesh);
			return;
		}
	}

	/* Departures: partitions first, then whole disks. */
restart_parts:
	TAILQ_FOREACH(disk, &gDADiskList, link) {
		if (!disk->volume_only && disk->walk_gen != g_walk_gen &&
		    disk->whole != NULL) {
			remove_disk(disk);
			w.removed++;
			goto restart_parts;
		}
	}
restart_wholes:
	TAILQ_FOREACH(disk, &gDADiskList, link) {
		if (!disk->volume_only && disk->walk_gen != g_walk_gen) {
			remove_disk(disk);
			w.removed++;
			goto restart_wholes;
		}
	}

	geom_deletetree(&mesh);

	total = wholes = 0;
	TAILQ_FOREACH(disk, &gDADiskList, link) {
		if (disk->volume_only)
			continue;
		total++;
		if (disk->whole == NULL)
			wholes++;
	}
	if (w.created != 0 || w.removed != 0)
		DALog("DA-GEOM: walk %u: %d disks (%d whole), +%d -%d",
		    g_walk_gen, total, wholes, w.created, w.removed);
	if (!g_geom_ok_logged && wholes > 0) {
		g_geom_ok_logged = 1;
		DALog("DA-GEOM-OK: %d disks from libgeom", total);
	}

	/*
	 * Mounts (#193): the first walk seeds them, so mounts of GEOM media
	 * attach to their disk; later walks re-check them when disks changed.
	 */
	if (!g_volumes_seeded || w.created != 0 || w.removed != 0) {
		g_volumes_seeded = 1;
		DAVolumeRefresh();
	}

	/*
	 * Stage the new media (#195): probe (#194), then mount. After the
	 * mount refresh, so a disk that is already mounted (the root) is
	 * known as mounted, skips the clean check and is not mounted again.
	 */
	DAStageSignal();
}

/* --- scheduling ----------------------------------------------------------- */

static void
walk_cb(void *ctx)
{
	/* Only the newest event's settle schedule walks. */
	if ((unsigned)(uintptr_t)ctx != atomic_load(&g_event_gen))
		return;
	da_geom_walk();
}

void
DAGeomInit(dispatch_queue_t queue)
{
	g_da_queue = queue;
}

void
DAGeomStorageChanged(void)
{
	unsigned gen;
	size_t i;

	if (g_da_queue == NULL)
		return;
	gen = atomic_fetch_add(&g_event_gen, 1) + 1;
	for (i = 0; i < nitems(settle_ms); i++)
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
		    settle_ms[i] * (int64_t)NSEC_PER_MSEC), g_da_queue,
		    (void *)(uintptr_t)gen, walk_cb);
}
