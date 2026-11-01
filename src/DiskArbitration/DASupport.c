/*
 * DASupport.c — daemon preferences and support routines (E15 C5,
 * nextbsd-userland#195).
 *
 * Port of DAPreferenceListRefresh (Apple DASupport.m). Apple reads two
 * files through SCPreferences: autodiskmount.plist, for the one
 * AutomountDisksWithoutUserLogin switch, and the daemon's own plist for the
 * DA* keys. NextBSD reads both sets from autodiskmount.plist, so there is
 * one file to edit and no com.apple-named preference file; the key names
 * and their meaning are Apple's. Plain CFPropertyList reads, so the daemon
 * does not link SystemConfiguration for a file read.
 *
 * The /etc/fstab mount map (#196) is Apple's __DAMountMapCreate1 and
 * DAMountMapListRefresh1 (DASupport.m), read through the vendored fstab.c.
 * Apple's second map, the vsdb(8) "ignore ownership" database
 * (/var/db/volinfo.database, DAMountMapListRefresh2), is not ported:
 * noowners has no NextBSD equivalent (#195), so it would carry nothing.
 */
#include "DASupport.h"
#include "DADisk.h"

#include <sys/stat.h>

#include <ctype.h>
#include <fcntl.h>
#include <fstab.h>
#include <paths.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static CFMutableDictionaryRef	g_prefs;
static struct timespec		g_prefs_mtime;
static int			g_prefs_loaded;

CFDictionaryRef
DAPropertyListCreateFromFile(const char *path)
{
	CFPropertyListRef plist;
	CFDataRef data;
	struct stat st;
	UInt8 *buf;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (NULL);
	if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1 << 20) {
		(void)close(fd);
		return (NULL);
	}
	buf = malloc((size_t)st.st_size);
	if (buf == NULL) {
		(void)close(fd);
		return (NULL);
	}
	n = read(fd, buf, (size_t)st.st_size);
	(void)close(fd);
	if (n != st.st_size) {
		free(buf);
		return (NULL);
	}
	data = CFDataCreate(kCFAllocatorDefault, buf, (CFIndex)n);
	free(buf);
	if (data == NULL)
		return (NULL);
	plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
	    kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist != NULL && CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		plist = NULL;
	}
	return ((CFDictionaryRef)plist);
}

/* Copy one boolean key from the file into the preference list. */
static void
copy_bool(CFDictionaryRef file, CFStringRef key)
{
	CFTypeRef v = CFDictionaryGetValue(file, key);

	if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID())
		CFDictionarySetValue(g_prefs, key, v);
}

void
DAPreferenceListRefresh(void)
{
	CFDictionaryRef file;
	CFTypeRef v;
	struct stat st;
	struct timespec mt = { 0, 0 };

	if (g_prefs == NULL) {
		g_prefs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (g_prefs == NULL)
			return;
	}
	if (stat(DA_PREFERENCE_FILE, &st) == 0)
		mt = st.st_mtim;
	if (g_prefs_loaded && mt.tv_sec == g_prefs_mtime.tv_sec &&
	    mt.tv_nsec == g_prefs_mtime.tv_nsec)
		return;
	g_prefs_loaded = 1;
	g_prefs_mtime = mt;
	CFDictionaryRemoveAllValues(g_prefs);

	file = DAPropertyListCreateFromFile(DA_PREFERENCE_FILE);
	if (file == NULL) {
		if (mt.tv_sec != 0)
			DALog("DA-PREFS: %s unreadable, using defaults",
			    DA_PREFERENCE_FILE);
		return;
	}

	/* Apple's autodiskmount.plist switch (DASupport.m). */
	v = CFDictionaryGetValue(file, kDAPreferenceAutomountWithoutLoginKey);
	if (v == kCFBooleanTrue) {
		CFDictionarySetValue(g_prefs,
		    kDAPreferenceMountDeferExternalKey, kCFBooleanFalse);
		CFDictionarySetValue(g_prefs,
		    kDAPreferenceMountDeferRemovableKey, kCFBooleanFalse);
		CFDictionarySetValue(g_prefs,
		    kDAPreferenceMountTrustExternalKey, kCFBooleanTrue);
	} else if (v == kCFBooleanFalse) {
		CFDictionarySetValue(g_prefs,
		    kDAPreferenceMountDeferExternalKey, kCFBooleanFalse);
		CFDictionarySetValue(g_prefs,
		    kDAPreferenceMountDeferRemovableKey, kCFBooleanTrue);
		CFDictionarySetValue(g_prefs,
		    kDAPreferenceMountTrustExternalKey, kCFBooleanTrue);
	}

	/* The daemon keys; explicit keys win over the switch, as on Apple. */
	copy_bool(file, kDAPreferenceMountDeferExternalKey);
	copy_bool(file, kDAPreferenceMountDeferInternalKey);
	copy_bool(file, kDAPreferenceMountDeferRemovableKey);
	copy_bool(file, kDAPreferenceMountTrustExternalKey);
	copy_bool(file, kDAPreferenceMountTrustInternalKey);
	copy_bool(file, kDAPreferenceMountTrustRemovableKey);
	copy_bool(file, kDAPreferenceAutoMountDisableKey);
	copy_bool(file, kDAPreferenceMountNoAccessTimeUntrustedKey);
	copy_bool(file, kDAPreferenceMountNoExecuteUntrustedKey);
	CFRelease(file);
	DALog("DA-PREFS: read %s (%ld keys)", DA_PREFERENCE_FILE,
	    (long)CFDictionaryGetCount(g_prefs));
}

int
DAPreferenceGetBool(CFStringRef key, int fallback)
{
	CFTypeRef v;

	if (g_prefs == NULL)
		return (fallback);
	v = CFDictionaryGetValue(g_prefs, key);
	if (v == NULL || CFGetTypeID(v) != CFBooleanGetTypeID())
		return (fallback);
	return (CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0);
}

int
DAConsoleUserGet(uid_t *uid, gid_t *gid)
{
	/* #199: SCDynamicStoreCopyConsoleUser(State:/Users/ConsoleUser). */
	*uid = 0;
	*gid = 0;
	return (1);
}

/* --- the /etc/fstab mount map (#196) -------------------------------------- */

/* The vendored fstab.c (Apple's getfsent copy, renamed). */
int		 DAsetfsent(void);
struct fstab	*DAgetfsent(void);
void		 DAendfsent(void);

CFMutableArrayRef		gDAMountMapList1;
static struct timespec		g_map_mtime;

/* Apple's ___CFUUIDCreateFromString: only a well-formed 8-4-4-4-12 UUID. */
static CFUUIDRef
map_uuid_create(const char *text)
{
	CFStringRef s;
	CFUUIDRef uuid;
	size_t i;

	if (strlen(text) != 36)
		return (NULL);
	for (i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (text[i] != '-')
				return (NULL);
		} else if (!isxdigit((unsigned char)text[i]))
			return (NULL);
	}
	s = CFStringCreateWithCString(kCFAllocatorDefault, text,
	    kCFStringEncodingUTF8);
	if (s == NULL)
		return (NULL);
	uuid = CFUUIDCreateFromString(kCFAllocatorDefault, s);
	CFRelease(s);
	return (uuid);
}

/* Apple's ___CFDictionaryCreateFromXMLString. */
static CFDictionaryRef
map_device_create(const char *text)
{
	CFPropertyListRef plist;
	CFDataRef data;

	data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)text,
	    (CFIndex)strlen(text));
	if (data == NULL)
		return (NULL);
	plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
	    kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist != NULL && CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		plist = NULL;
	}
	return ((CFDictionaryRef)plist);
}

static void
map_set_string(CFMutableDictionaryRef map, CFStringRef key, const char *value)
{
	CFStringRef s;

	s = CFStringCreateWithCString(kCFAllocatorDefault, value,
	    kCFStringEncodingUTF8);
	if (s != NULL) {
		CFDictionarySetValue(map, key, s);
		CFRelease(s);
	}
}

/*
 * __DAMountMapCreate1: one map entry for a UUID=, LABEL= or DEVICE= line,
 * NULL for anything else (a /dev path, tmpfs, host:/export, swap).
 */
static CFDictionaryRef
map_create(struct fstab *fs)
{
	CFMutableDictionaryRef map;
	CFTypeRef id = NULL;
	char spec[1024], options[1024], *value, *arguments, *argument;
	CFBooleanRef automatic = NULL;

	if (strcmp(fs->fs_type, FSTAB_SW) == 0)
		return (NULL);
	(void)strlcpy(spec, fs->fs_spec, sizeof(spec));
	value = fs->fs_spec;
	(void)strsep(&value, "=");
	if (value == NULL)
		return (NULL);
	if (strcmp(fs->fs_spec, "UUID") == 0)
		id = map_uuid_create(value);
	else if (strcmp(fs->fs_spec, "LABEL") == 0)
		id = CFStringCreateWithCString(kCFAllocatorDefault, value,
		    kCFStringEncodingUTF8);
	else if (strcmp(fs->fs_spec, "DEVICE") == 0)
		id = map_device_create(value);
	if (id == NULL) {
		if (strcmp(fs->fs_spec, "UUID") == 0 ||
		    strcmp(fs->fs_spec, "LABEL") == 0 ||
		    strcmp(fs->fs_spec, "DEVICE") == 0)
			DALog("DA-FSTAB: %s: cannot parse \"%s\"; line ignored",
			    _PATH_FSTAB, spec);
		return (NULL);
	}

	map = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (map == NULL) {
		CFRelease(id);
		return (NULL);
	}
	CFDictionarySetValue(map, kDAMountMapProbeIDKey, id);
	CFRelease(id);
	map_set_string(map, kDAMountMapSpecKey, spec);

	/* auto / noauto; every other token goes to mount(8) verbatim. */
	options[0] = '\0';
	arguments = fs->fs_mntops;
	while ((argument = strsep(&arguments, ",")) != NULL) {
		if (strcmp(argument, "auto") == 0)
			automatic = kCFBooleanTrue;
		else if (strcmp(argument, "noauto") == 0)
			automatic = kCFBooleanFalse;
		else if (argument[0] != '\0') {
			if (options[0] != '\0')
				(void)strlcat(options, ",", sizeof(options));
			(void)strlcat(options, argument, sizeof(options));
		}
	}
	if (automatic != NULL)
		CFDictionarySetValue(map, kDAMountMapMountAutomaticKey,
		    automatic);
	if (options[0] != '\0')
		map_set_string(map, kDAMountMapMountOptionsKey, options);

	/* "none": the default /Volumes/<name>. */
	if (strcmp(fs->fs_file, "none") != 0)
		map_set_string(map, kDAMountMapMountPathKey, fs->fs_file);
	/* "auto": no filesystem filter. */
	if (strcmp(fs->fs_vfstype, "auto") != 0)
		map_set_string(map, kDAMountMapProbeKindKey, fs->fs_vfstype);

	DALog("DA-FSTAB: %s entry %s on %s kind=%s automatic=%s options=%s",
	    _PATH_FSTAB, spec, strcmp(fs->fs_file, "none") != 0 ?
	    fs->fs_file : "(default)", strcmp(fs->fs_vfstype, "auto") != 0 ?
	    fs->fs_vfstype : "(any)", automatic == kCFBooleanTrue ? "auto" :
	    automatic == kCFBooleanFalse ? "noauto" : "(default)",
	    options[0] != '\0' ? options : "-");
	return (map);
}

void
DAMountMapListRefresh1(void)
{
	struct fstab *item;
	struct stat st;
	struct timespec mt = { 0, 0 };
	CFDictionaryRef map;
	CFIndex had;

	if (gDAMountMapList1 == NULL) {
		gDAMountMapList1 = CFArrayCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeArrayCallBacks);
		if (gDAMountMapList1 == NULL)
			return;
	}
	if (stat(_PATH_FSTAB, &st) == 0)
		mt = st.st_mtim;
	if (mt.tv_sec == g_map_mtime.tv_sec &&
	    mt.tv_nsec == g_map_mtime.tv_nsec)
		return;
	g_map_mtime = mt;
	had = CFArrayGetCount(gDAMountMapList1);
	CFArrayRemoveAllValues(gDAMountMapList1);

	if (mt.tv_sec == 0 && mt.tv_nsec == 0) {
		/* No fstab: an empty map, quietly (the NextBSD default). */
		if (had != 0)
			DALog("DA-FSTAB: %s is gone; the mount map is empty",
			    _PATH_FSTAB);
		return;
	}
	if (DAsetfsent()) {
		while ((item = DAgetfsent()) != NULL) {
			map = map_create(item);
			if (map != NULL) {
				CFArrayAppendValue(gDAMountMapList1, map);
				CFRelease(map);
			}
		}
		DAendfsent();
	}
	DALog("DA-FSTAB: read %s, %ld mount map entries", _PATH_FSTAB,
	    (long)CFArrayGetCount(gDAMountMapList1));
}
