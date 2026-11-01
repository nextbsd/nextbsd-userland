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
 */
#include "DASupport.h"
#include "DADisk.h"

#include <sys/stat.h>

#include <fcntl.h>
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
