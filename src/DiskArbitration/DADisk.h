/*
 * DADisk.h — diskarbitrationd's disk object (E15 C2, nextbsd-userland#192).
 *
 * One struct da_disk per media object the daemon knows about: a whole disk
 * (GEOM DISK-class provider) or a partition (GEOM PART-class provider). Each
 * carries Apple's kDADiskDescription* dictionary, the same key set Apple's
 * DADiskCreateFromIOMedia (diskarbitrationd/DADisk.c) fills from an IOMedia
 * object; on NextBSD the sources are libgeom and CAM (see DAGeom.c).
 *
 * The dictionary is the contract every later stage keys on: the fs-bundle
 * probe (#194) matches on Content/Leaf/Whole, the mount policy (#195) on
 * Removable/Internal/Writable, and the client framework (#197) serialises it
 * unchanged. All access happens on the daemon's serial work queue.
 */
#ifndef DISKARBITRATION_DADISK_H
#define DISKARBITRATION_DADISK_H

#include <CoreFoundation/CoreFoundation.h>

#include <sys/types.h>
#include <sys/queue.h>
#include <stdint.h>

/*
 * Apple's description keys (DiskArbitration/DADisk.h). The framework (#197)
 * exports them as `extern const CFStringRef`; inside the daemon they are
 * macros so no file-scope CFSTR() initialiser is needed.
 */
#define kDADiskDescriptionVolumeKindKey		CFSTR("DAVolumeKind")
#define kDADiskDescriptionVolumeMountableKey	CFSTR("DAVolumeMountable")
#define kDADiskDescriptionVolumeNameKey		CFSTR("DAVolumeName")
#define kDADiskDescriptionVolumeNetworkKey	CFSTR("DAVolumeNetwork")
#define kDADiskDescriptionVolumePathKey		CFSTR("DAVolumePath")
#define kDADiskDescriptionVolumeTypeKey		CFSTR("DAVolumeType")
#define kDADiskDescriptionVolumeUUIDKey		CFSTR("DAVolumeUUID")

#define kDADiskDescriptionMediaBlockSizeKey	CFSTR("DAMediaBlockSize")
#define kDADiskDescriptionMediaBSDMajorKey	CFSTR("DAMediaBSDMajor")
#define kDADiskDescriptionMediaBSDMinorKey	CFSTR("DAMediaBSDMinor")
#define kDADiskDescriptionMediaBSDNameKey	CFSTR("DAMediaBSDName")
#define kDADiskDescriptionMediaBSDUnitKey	CFSTR("DAMediaBSDUnit")
#define kDADiskDescriptionMediaContentKey	CFSTR("DAMediaContent")
#define kDADiskDescriptionMediaEjectableKey	CFSTR("DAMediaEjectable")
#define kDADiskDescriptionMediaIconKey		CFSTR("DAMediaIcon")
#define kDADiskDescriptionMediaKindKey		CFSTR("DAMediaKind")
#define kDADiskDescriptionMediaLeafKey		CFSTR("DAMediaLeaf")
#define kDADiskDescriptionMediaNameKey		CFSTR("DAMediaName")
#define kDADiskDescriptionMediaPathKey		CFSTR("DAMediaPath")
#define kDADiskDescriptionMediaRemovableKey	CFSTR("DAMediaRemovable")
#define kDADiskDescriptionMediaSizeKey		CFSTR("DAMediaSize")
#define kDADiskDescriptionMediaTypeKey		CFSTR("DAMediaType")
#define kDADiskDescriptionMediaUUIDKey		CFSTR("DAMediaUUID")
#define kDADiskDescriptionMediaWholeKey		CFSTR("DAMediaWhole")
#define kDADiskDescriptionMediaWritableKey	CFSTR("DAMediaWritable")

#define kDADiskDescriptionDeviceGUIDKey		CFSTR("DADeviceGUID")
#define kDADiskDescriptionDeviceInternalKey	CFSTR("DADeviceInternal")
#define kDADiskDescriptionDeviceModelKey	CFSTR("DADeviceModel")
#define kDADiskDescriptionDevicePathKey		CFSTR("DADevicePath")
#define kDADiskDescriptionDeviceProtocolKey	CFSTR("DADeviceProtocol")
#define kDADiskDescriptionDeviceRevisionKey	CFSTR("DADeviceRevision")
#define kDADiskDescriptionDeviceUnitKey		CFSTR("DADeviceUnit")
#define kDADiskDescriptionDeviceVendorKey	CFSTR("DADeviceVendor")

#define kDADiskDescriptionBusNameKey		CFSTR("DABusName")
#define kDADiskDescriptionBusPathKey		CFSTR("DABusPath")

#define kDADiskDescriptionAppearanceTimeKey	CFSTR("DAAppearanceTime")

/*
 * Disk state bits, named after Apple's diskarbitrationd/DADisk.h. The values
 * are daemon-internal (never on the wire). Nothing sets them yet (C2 is
 * log-only); the stage machine (#195) drives them.
 */
enum {
	kDADiskStateCommandActive	= 0x00000001,
	kDADiskStateRequireRepair	= 0x00000002,
	kDADiskStateRequireRepairQuotas	= 0x00000004,
	kDADiskStateRequireReprobe	= 0x00000008,
	kDADiskStateStagedProbe		= 0x00010000,
	kDADiskStateStagedPeek		= 0x00020000,
	kDADiskStateStagedApprove	= 0x00080000,
	kDADiskStateStagedAuthorize	= 0x00100000,
	kDADiskStateStagedMount		= 0x00200000,
	kDADiskStateStagedAppear	= 0x00400000,
	kDADiskStateStagedUnrepairable	= 0x00800000,
	kDADiskStateZombie		= 0x10000000
};

/* Aliases a LABEL-class geom publishes for one provider (gpt/, ufs/, ...). */
#define DA_DISK_LINK_MAX	4

struct da_disk {
	TAILQ_ENTRY(da_disk)	link;		/* gDADiskList */
	char			*id;		/* "/dev/da0p1" (DADiskGetID) */
	CFMutableDictionaryRef	description;	/* kDADiskDescription* keys */
	uint32_t		state;		/* kDADiskState* */
	char			*device_link[DA_DISK_LINK_MAX]; /* /dev/gpt/EFI ... */
	struct da_disk		*whole;		/* the DISK-class disk; NULL if whole */
	char			*geom_class;	/* "DISK" | "PART" */
	char			*ioreg_path;	/* registry node the media hangs off */
	char			*bus_driver;	/* storage driver (vtblk, umass, ...) */
	uid_t			user_uid;	/* mount owner (#195); 0 for now */
	gid_t			user_gid;

	/* GEOM bookkeeping for the walk/diff (DAGeom.c). */
	char			*signature;	/* size|content|uuid at creation */
	uint32_t		walk_gen;	/* last walk that saw this provider */
};

TAILQ_HEAD(da_disk_list, da_disk);
extern struct da_disk_list gDADiskList;

struct da_disk	*DADiskCreate(const char *id, const char *geom_class);
void		 DADiskRelease(struct da_disk *disk);
const char	*DADiskGetID(const struct da_disk *disk);
struct da_disk	*DADiskListFind(const char *id);

CFTypeRef	 DADiskGetDescription(const struct da_disk *disk, CFStringRef key);
/* String or CFUUID value as UTF-8 into buf; returns -1 (buf "") if absent. */
int		 DADiskGetDescriptionCString(const struct da_disk *disk,
		    CFStringRef key, char *buf, size_t len);
/* Setters: a NULL / empty string value removes the key. */
void		 DADiskSetDescription(struct da_disk *disk, CFStringRef key,
		    CFTypeRef value);
void		 DADiskSetDescriptionString(struct da_disk *disk, CFStringRef key,
		    const char *value);
void		 DADiskSetDescriptionBool(struct da_disk *disk, CFStringRef key,
		    int value);
void		 DADiskSetDescriptionInt32(struct da_disk *disk, CFStringRef key,
		    int32_t value);
void		 DADiskSetDescriptionInt64(struct da_disk *disk, CFStringRef key,
		    int64_t value);
void		 DADiskSetDescriptionDouble(struct da_disk *disk, CFStringRef key,
		    double value);

/* Replace the _deviceLink aliases; returns 1 if they changed. */
int		 DADiskSetDeviceLinks(struct da_disk *disk, char *const *links,
		    int count);

/* One DA-DISK line, read back from the description dictionary. */
void		 DADiskLog(const struct da_disk *disk);

/* Daemon log line (stderr, launchd routes it to diskarbitrationd.stderr). */
void		 DALog(const char *fmt, ...) __printflike(1, 2);

#endif /* DISKARBITRATION_DADISK_H */
