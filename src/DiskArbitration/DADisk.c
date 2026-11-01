/*
 * DADisk.c — diskarbitrationd's disk object (E15 C2, nextbsd-userland#192).
 *
 * The C counterpart of Apple's struct __DADisk (diskarbitrationd/DADisk.c):
 * an ID (the /dev path), a CF description dictionary and the bookkeeping the
 * later stages hang off it. DAGeom.c fills the dictionary from libgeom; this
 * file owns allocation, the global disk list and the DA-DISK log line.
 */
#include "DADisk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct da_disk_list gDADiskList = TAILQ_HEAD_INITIALIZER(gDADiskList);

void
DALog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "diskarbitrationd[geom] %s ", tbuf);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

struct da_disk *
DADiskCreate(const char *id, const char *geom_class)
{
	struct da_disk *disk;

	disk = calloc(1, sizeof(*disk));
	if (disk == NULL)
		return (NULL);
	disk->id = strdup(id);
	disk->geom_class = strdup(geom_class);
	disk->description = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (disk->id == NULL || disk->geom_class == NULL ||
	    disk->description == NULL) {
		DADiskRelease(disk);
		return (NULL);
	}
	return (disk);
}

void
DADiskRelease(struct da_disk *disk)
{
	int i;

	if (disk == NULL)
		return;
	if (disk->description != NULL)
		CFRelease(disk->description);
	for (i = 0; i < DA_DISK_LINK_MAX; i++)
		free(disk->device_link[i]);
	free(disk->id);
	free(disk->geom_class);
	free(disk->ioreg_path);
	free(disk->bus_driver);
	free(disk->signature);
	free(disk);
}

const char *
DADiskGetID(const struct da_disk *disk)
{
	return (disk->id);
}

struct da_disk *
DADiskListFind(const char *id)
{
	struct da_disk *disk;

	TAILQ_FOREACH(disk, &gDADiskList, link)
		if (strcmp(disk->id, id) == 0)
			return (disk);
	return (NULL);
}

CFTypeRef
DADiskGetDescription(const struct da_disk *disk, CFStringRef key)
{
	return (CFDictionaryGetValue(disk->description, key));
}

void
DADiskSetDescription(struct da_disk *disk, CFStringRef key, CFTypeRef value)
{
	if (value != NULL)
		CFDictionarySetValue(disk->description, key, value);
	else
		CFDictionaryRemoveValue(disk->description, key);
}

void
DADiskSetDescriptionString(struct da_disk *disk, CFStringRef key,
    const char *value)
{
	CFStringRef s;

	if (value == NULL) {
		DADiskSetDescription(disk, key, NULL);
		return;
	}
	s = CFStringCreateWithCString(kCFAllocatorDefault, value,
	    kCFStringEncodingUTF8);
	/* A label that is not valid UTF-8: keep the bytes rather than drop it. */
	if (s == NULL)
		s = CFStringCreateWithCString(kCFAllocatorDefault, value,
		    kCFStringEncodingISOLatin1);
	DADiskSetDescription(disk, key, s);
	if (s != NULL)
		CFRelease(s);
}

void
DADiskSetDescriptionBool(struct da_disk *disk, CFStringRef key, int value)
{
	DADiskSetDescription(disk, key, value ? kCFBooleanTrue : kCFBooleanFalse);
}

void
DADiskSetDescriptionInt32(struct da_disk *disk, CFStringRef key, int32_t value)
{
	CFNumberRef n;

	n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
	DADiskSetDescription(disk, key, n);
	if (n != NULL)
		CFRelease(n);
}

void
DADiskSetDescriptionInt64(struct da_disk *disk, CFStringRef key, int64_t value)
{
	CFNumberRef n;

	n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
	DADiskSetDescription(disk, key, n);
	if (n != NULL)
		CFRelease(n);
}

void
DADiskSetDescriptionDouble(struct da_disk *disk, CFStringRef key, double value)
{
	CFNumberRef n;

	n = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
	DADiskSetDescription(disk, key, n);
	if (n != NULL)
		CFRelease(n);
}

int
DADiskSetDeviceLinks(struct da_disk *disk, char *const *links, int count)
{
	int changed = 0;
	int i;

	if (count > DA_DISK_LINK_MAX)
		count = DA_DISK_LINK_MAX;
	for (i = 0; i < DA_DISK_LINK_MAX; i++) {
		const char *want = i < count ? links[i] : NULL;
		const char *have = disk->device_link[i];

		if (want == NULL && have == NULL)
			continue;
		if (want != NULL && have != NULL && strcmp(want, have) == 0)
			continue;
		free(disk->device_link[i]);
		disk->device_link[i] = want != NULL ? strdup(want) : NULL;
		changed = 1;
	}
	return (changed);
}

/* --- description readers / DA-DISK log line ------------------------------ */

/* String (or CFUUID, unparsed) value of `key`; "" and -1 if absent. */
int
DADiskGetDescriptionCString(const struct da_disk *disk, CFStringRef key,
    char *buf, size_t len)
{
	CFTypeRef v = DADiskGetDescription(disk, key);

	buf[0] = '\0';
	if (v == NULL)
		return (-1);
	if (CFGetTypeID(v) == CFStringGetTypeID()) {
		if (CFStringGetCString((CFStringRef)v, buf, (CFIndex)len,
		    kCFStringEncodingUTF8))
			return (0);
	} else if (CFGetTypeID(v) == CFUUIDGetTypeID()) {
		CFStringRef s = CFUUIDCreateString(kCFAllocatorDefault,
		    (CFUUIDRef)v);
		Boolean ok = false;

		if (s != NULL) {
			ok = CFStringGetCString(s, buf, (CFIndex)len,
			    kCFStringEncodingUTF8);
			CFRelease(s);
		}
		if (ok)
			return (0);
	}
	buf[0] = '\0';
	return (-1);
}

/* "1" / "0", or "?" when the key is absent (source unknown). */
static const char *
desc_bool(const struct da_disk *disk, CFStringRef key)
{
	CFTypeRef v = DADiskGetDescription(disk, key);

	if (v == NULL || CFGetTypeID(v) != CFBooleanGetTypeID())
		return ("?");
	return (CFBooleanGetValue((CFBooleanRef)v) ? "1" : "0");
}

static long long
desc_int64(const struct da_disk *disk, CFStringRef key)
{
	CFTypeRef v = DADiskGetDescription(disk, key);
	int64_t n = 0;

	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID())
		(void)CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &n);
	return ((long long)n);
}

void
DADiskLog(const struct da_disk *disk)
{
	char content[128], name[256], kind[32], uuid[64], links[512];
	int i;

	(void)DADiskGetDescriptionCString(disk,
	    kDADiskDescriptionMediaContentKey, content, sizeof(content));
	(void)DADiskGetDescriptionCString(disk,
	    kDADiskDescriptionMediaNameKey, name, sizeof(name));
	(void)DADiskGetDescriptionCString(disk,
	    kDADiskDescriptionMediaKindKey, kind, sizeof(kind));
	(void)DADiskGetDescriptionCString(disk,
	    kDADiskDescriptionMediaUUIDKey, uuid, sizeof(uuid));
	links[0] = '\0';
	for (i = 0; i < DA_DISK_LINK_MAX && disk->device_link[i] != NULL; i++) {
		if (i > 0)
			(void)strlcat(links, ",", sizeof(links));
		(void)strlcat(links, disk->device_link[i], sizeof(links));
	}

	DALog("DA-DISK: %s whole=%s leaf=%s content=%s size=%lld "
	    "removable=%s internal=%s bus=%s name=\"%s\" kind=%s writable=%s "
	    "uuid=%s links=%s",
	    disk->id,
	    desc_bool(disk, kDADiskDescriptionMediaWholeKey),
	    desc_bool(disk, kDADiskDescriptionMediaLeafKey),
	    content[0] != '\0' ? content : "\"\"",
	    desc_int64(disk, kDADiskDescriptionMediaSizeKey),
	    desc_bool(disk, kDADiskDescriptionMediaRemovableKey),
	    desc_bool(disk, kDADiskDescriptionDeviceInternalKey),
	    disk->bus_driver != NULL ? disk->bus_driver : "?",
	    name,
	    kind,
	    desc_bool(disk, kDADiskDescriptionMediaWritableKey),
	    uuid[0] != '\0' ? uuid : "-",
	    links[0] != '\0' ? links : "-");
}
