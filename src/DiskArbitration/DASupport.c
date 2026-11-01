/*
 * DASupport.c — daemon preferences and support routines (E15 C5,
 * nextbsd-userland#195).
 *
 * Port of DAPreferenceListRefresh (Apple DASupport.m). Apple reads two
 * files through SCPreferences: autodiskmount.plist, for the one
 * AutomountDisksWithoutUserLogin switch, and the daemon's own plist for the
 * DA* keys. NextBSD reads both sets from autodiskmount.plist, so there is
 * one file to edit and no com.apple-named preference file; the key names
 * and their meaning are Apple's. Plain CFPropertyList reads, not
 * SCPreferences.
 *
 * The console user (#199) is Apple's DAMain.c session on configd and the
 * DABase.c ___SCDynamicStoreCopyConsole* wrappers: State:/Users/ConsoleUser,
 * which a login program publishes (/usr/libexec/consoleuser), watched on the
 * daemon queue. The login and logout handling is _DAConfigurationCallback
 * in DAServer.c, as on Apple.
 *
 * The /etc/fstab mount map (#196) is Apple's __DAMountMapCreate1 and
 * DAMountMapListRefresh1 (DASupport.m), read through the vendored fstab.c.
 * Apple's second map, the vsdb(8) "ignore ownership" database
 * (/var/db/volinfo.database, DAMountMapListRefresh2), is not ported:
 * noowners has no NextBSD equivalent (#195), so it would carry nothing.
 */
#include "DASupport.h"
#include "DADisk.h"
#include "DAServer.h"

#include <SystemConfiguration/SCDynamicStoreCopySpecific.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecificPrivate.h>

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

/* --- the console user (#199) --------------------------------------------- */

CFStringRef		gDAConsoleUser;
uid_t			gDAConsoleUserUID;
gid_t			gDAConsoleUserGID;
CFArrayRef		gDAConsoleUserList;

static SCDynamicStoreRef	g_console_store;
static dispatch_queue_t		g_console_queue;
static int			g_console_retry_logged;

/* Apple's ___UID_ROOT / ___GID_WHEEL. */
#define DA_UID_ROOT	((uid_t)0)
#define DA_GID_WHEEL	((gid_t)0)

CFStringRef
___SCDynamicStoreCopyConsoleUser(SCDynamicStoreRef store, uid_t *uid,
    gid_t *gid)
{
	CFStringRef user;

	if (gid != NULL)
		*gid = DA_GID_WHEEL;
	if (uid != NULL)
		*uid = DA_UID_ROOT;
	user = SCDynamicStoreCopyConsoleUser(store, uid, gid);
	if (user != NULL && CFEqual(user, CFSTR("loginwindow"))) {
		/* Apple: the login window itself is nobody. */
		if (gid != NULL)
			*gid = DA_GID_WHEEL;
		if (uid != NULL)
			*uid = DA_UID_ROOT;
		CFRelease(user);
		user = NULL;
	}
	return (user);
}

CFArrayRef
___SCDynamicStoreCopyConsoleInformation(SCDynamicStoreRef store)
{
	CFMutableArrayRef list = NULL;
	CFMutableDictionaryRef dictionary;
	CFArrayRef sessions;
	CFDictionaryRef session;
	CFStringRef user;
	CFIndex index;

	sessions = SCDynamicStoreCopyConsoleInformation(store);
	if (sessions != NULL) {
		list = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0,
		    sessions);
		CFRelease(sessions);
		if (list == NULL)
			return (NULL);
		/* Sessions still logging in are not logged in yet. */
		for (index = CFArrayGetCount(list) - 1; index > -1; index--) {
			session = CFArrayGetValueAtIndex(list, index);
			if (CFGetTypeID(session) != CFDictionaryGetTypeID() ||
			    CFDictionaryGetValue(session,
			    kSCConsoleSessionLoginDone) == kCFBooleanFalse)
				CFArrayRemoveValueAtIndex(list, index);
		}
		if (CFArrayGetCount(list) == 0) {
			CFRelease(list);
			list = NULL;
		}
		return (list);
	}

	/* No session list: one session for the user, if there is one. */
	user = ___SCDynamicStoreCopyConsoleUser(store, NULL, NULL);
	if (user == NULL)
		return (NULL);
	dictionary = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (dictionary != NULL) {
		CFDictionarySetValue(dictionary, kSCConsoleSessionUserName,
		    user);
		list = CFArrayCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeArrayCallBacks);
		if (list != NULL)
			CFArrayAppendValue(list, dictionary);
		CFRelease(dictionary);
	}
	CFRelease(user);
	return (list);
}

int
DAConsoleUserGet(uid_t *uid, gid_t *gid)
{
	if (gDAConsoleUserList == NULL) {
		*uid = DA_UID_ROOT;
		*gid = DA_GID_WHEEL;
		return (0);
	}
	*uid = gDAConsoleUserUID;
	*gid = gDAConsoleUserGID;
	return (1);
}

/* The whole disk a volume sits on. */
static const struct da_disk *
console_unit(const struct da_disk *disk)
{
	return (disk->whole != NULL ? disk->whole : disk);
}

int
DAConsoleUserMayOwn(const struct da_disk *disk)
{
	const struct da_disk *unit, *sub;

	if (disk->volume_only)
		return (0);
	unit = console_unit(disk);
	if (DADiskGetDescription(unit, kDADiskDescriptionMediaRemovableKey) !=
	    kCFBooleanTrue && DADiskGetDescription(unit,
	    kDADiskDescriptionDeviceInternalKey) == kCFBooleanTrue)
		return (0);
	TAILQ_FOREACH(sub, &gDADiskList, link) {
		if (!sub->volume_only && console_unit(sub) == unit &&
		    (sub->state & kDADiskStateMountSystem) != 0)
			return (0);
	}
	return (1);
}

void
DAConsoleUserClaim(struct da_disk *disk)
{
	uid_t uid;
	gid_t gid;
	int console;

	if (disk->volume_only)
		return;
	if (!DAConsoleUserMayOwn(disk)) {
		/*
		 * A whole disk only ever gets a user here; one that has
		 * since become a system disk (an /etc/fstab volume mounted
		 * on it) goes back to root.
		 */
		if (disk->whole == NULL && disk->user_uid != DA_UID_ROOT &&
		    DADiskGetDescription(disk,
		    kDADiskDescriptionMediaWholeKey) == kCFBooleanTrue) {
			disk->user_uid = DA_UID_ROOT;
			disk->user_gid = DA_GID_WHEEL;
			DALog("DA-CONSOLE: %s carries a system volume; now "
			    "owned by root", disk->id);
		}
		return;
	}
	console = DAConsoleUserGet(&uid, &gid);
	if (disk->whole == NULL && DADiskGetDescription(disk,
	    kDADiskDescriptionMediaWholeKey) == kCFBooleanTrue) {
		if (disk->user_uid == uid && disk->user_gid == gid)
			return;
		disk->user_uid = uid;
		disk->user_gid = gid;
		DALog("DA-CONSOLE: %s now owned by %s (uid %u)", disk->id,
		    console ? "the console user" : "root", (unsigned)uid);
		return;
	}
	if (console && disk->user_uid == DA_UID_ROOT &&
	    DADiskGetDescription(disk, kDADiskDescriptionVolumePathKey) !=
	    NULL) {
		disk->user_uid = uid;
		disk->user_gid = gid;
		DALog("DA-CONSOLE: %s (mounted while nobody was logged in) now "
		    "owned by the console user (uid %u)", disk->id,
		    (unsigned)uid);
	}
}

static void
console_log(const char *why)
{
	char name[256];

	if (gDAConsoleUser == NULL || !CFStringGetCString(gDAConsoleUser,
	    name, sizeof(name), kCFStringEncodingUTF8))
		(void)strlcpy(name, "none", sizeof(name));
	DALog("DA-CONSOLE: %s: console user = %s [%u], %ld session%s%s", why,
	    name, (unsigned)gDAConsoleUserUID,
	    gDAConsoleUserList != NULL ?
	    (long)CFArrayGetCount(gDAConsoleUserList) : 0L,
	    gDAConsoleUserList != NULL &&
	    CFArrayGetCount(gDAConsoleUserList) == 1 ? "" : "s",
	    gDAConsoleUserList != NULL ? "" :
	    DAPreferenceGetBool(kDAPreferenceMountDeferRemovableKey, 1) ||
	    DAPreferenceGetBool(kDAPreferenceMountDeferExternalKey, 1) ?
	    "; removable and external media wait for a login" :
	    "; removable and external media mount owned by root "
	    "(AutomountDisksWithoutUserLogin)");
}

static void	console_connect(void *context);

static void
console_retry(void)
{
	if (!g_console_retry_logged) {
		g_console_retry_logged = 1;
		DALog("DA-CONSOLE: configd is not reachable (%s); nobody is "
		    "logged in until it is, retrying", SCErrorString(SCError()));
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
	    5 * (int64_t)NSEC_PER_SEC), g_console_queue, (void *)1,
	    console_connect);
}

/*
 * DAMain.c: SCDynamicStoreCreate(_kDADaemonName, _DAConfigurationCallback),
 * the console-user key as the notification key, the session on the daemon
 * queue. At startup Apple only reads the globals (there are no disks
 * yet). `context` is non-NULL on a retry, where a console user already
 * logged in runs the callback as a login.
 */
static void
console_connect(void *context)
{
	SCDynamicStoreRef store;
	CFStringRef key;
	CFArrayRef keys;

	store = SCDynamicStoreCreate(kCFAllocatorDefault,
	    CFSTR("com.apple.DiskArbitration.diskarbitrationd"),
	    _DAConfigurationCallback, NULL);
	if (store == NULL) {
		console_retry();
		return;
	}
	key = SCDynamicStoreKeyCreateConsoleUser(kCFAllocatorDefault);
	keys = key != NULL ? CFArrayCreate(kCFAllocatorDefault,
	    (const void **)&key, 1, &kCFTypeArrayCallBacks) : NULL;
	if (key != NULL)
		CFRelease(key);
	if (keys == NULL || !SCDynamicStoreSetNotificationKeys(store, keys,
	    NULL) || !SCDynamicStoreSetDispatchQueue(store, g_console_queue)) {
		if (keys != NULL)
			CFRelease(keys);
		CFRelease(store);
		console_retry();
		return;
	}
	CFRelease(keys);
	g_console_store = store;

	if (context != NULL) {
		CFArrayRef list;

		/*
		 * configd answered late. Somebody already logged in is a
		 * login (the callback mounts what was deferred); nobody is
		 * no change from what the daemon assumed, so nothing is
		 * unmounted.
		 */
		list = ___SCDynamicStoreCopyConsoleInformation(store);
		if (list != NULL) {
			CFRelease(list);
			DALog("DA-CONSOLE: configd reachable again");
			_DAConfigurationCallback(store, NULL, NULL);
			return;
		}
	}
	gDAConsoleUser = ___SCDynamicStoreCopyConsoleUser(store,
	    &gDAConsoleUserUID, &gDAConsoleUserGID);
	gDAConsoleUserList = ___SCDynamicStoreCopyConsoleInformation(store);
	console_log("watching State:/Users/ConsoleUser");
}

void
DAConsoleUserInit(dispatch_queue_t queue)
{
	g_console_queue = queue;
	gDAConsoleUserUID = DA_UID_ROOT;
	gDAConsoleUserGID = DA_GID_WHEEL;
	console_connect(NULL);
}

void
DAConsoleUserLog(const char *why)
{
	console_log(why);
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
