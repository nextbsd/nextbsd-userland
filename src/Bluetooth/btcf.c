/*
 * btcf.c — CoreFoundation helpers for bluetoothd's CF modules. See btcf.h.
 */
#include "btcf.h"

#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

CFStringRef
btcf_str(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

void
btcf_set_str(CFMutableDictionaryRef d, CFStringRef key, const char *v)
{
	CFStringRef s;

	if (v == NULL || v[0] == '\0')
		return;
	s = btcf_str(v);
	if (s == NULL)
		return;
	CFDictionarySetValue(d, key, s);
	CFRelease(s);
}

void
btcf_set_int(CFMutableDictionaryRef d, CFStringRef key, long long v)
{
	CFNumberRef n;

	n = CFNumberCreate(NULL, kCFNumberLongLongType, &v);
	if (n == NULL)
		return;
	CFDictionarySetValue(d, key, n);
	CFRelease(n);
}

void
btcf_set_bool(CFMutableDictionaryRef d, CFStringRef key, bool v)
{
	CFDictionarySetValue(d, key, v ? kCFBooleanTrue : kCFBooleanFalse);
}

void
btcf_set_data(CFMutableDictionaryRef d, CFStringRef key, const uint8_t *p,
    size_t len)
{
	CFDataRef data;

	if (p == NULL || len == 0)
		return;
	data = CFDataCreate(NULL, p, (CFIndex)len);
	if (data == NULL)
		return;
	CFDictionarySetValue(d, key, data);
	CFRelease(data);
}

bool
btcf_get_str(CFDictionaryRef d, CFStringRef key, char *out, size_t len)
{
	CFTypeRef v;

	if (d == NULL)
		return (false);
	v = CFDictionaryGetValue(d, key);
	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return (false);
	return (CFStringGetCString(v, out, (CFIndex)len,
	    kCFStringEncodingUTF8));
}

bool
btcf_get_int(CFDictionaryRef d, CFStringRef key, long long *out)
{
	CFTypeRef v;

	if (d == NULL)
		return (false);
	v = CFDictionaryGetValue(d, key);
	if (v == NULL)
		return (false);
	if (CFGetTypeID(v) == CFNumberGetTypeID())
		return (CFNumberGetValue(v, kCFNumberLongLongType, out));
	if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
		*out = CFBooleanGetValue(v) ? 1 : 0;
		return (true);
	}
	return (false);
}

bool
btcf_get_bool(CFDictionaryRef d, CFStringRef key, bool *out)
{
	CFTypeRef v;
	long long n;

	if (d == NULL)
		return (false);
	v = CFDictionaryGetValue(d, key);
	if (v == NULL)
		return (false);
	if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
		*out = CFBooleanGetValue(v);
		return (true);
	}
	/* `defaults write ... -int 0` style values read as booleans too. */
	if (CFGetTypeID(v) == CFNumberGetTypeID() &&
	    CFNumberGetValue(v, kCFNumberLongLongType, &n)) {
		*out = (n != 0);
		return (true);
	}
	return (false);
}

bool
btcf_get_data(CFDictionaryRef d, CFStringRef key, uint8_t *out, size_t len,
    size_t *got)
{
	CFTypeRef v;
	CFIndex n;

	if (d == NULL)
		return (false);
	v = CFDictionaryGetValue(d, key);
	if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID())
		return (false);
	n = CFDataGetLength(v);
	if (got != NULL)
		*got = (size_t)n;
	if ((size_t)n > len)
		n = (CFIndex)len;
	CFDataGetBytes(v, CFRangeMake(0, n), out);
	return (true);
}

CFDictionaryRef
btcf_get_dict(CFDictionaryRef d, CFStringRef key)
{
	CFTypeRef v;

	if (d == NULL)
		return (NULL);
	v = CFDictionaryGetValue(d, key);
	if (v == NULL || CFGetTypeID(v) != CFDictionaryGetTypeID())
		return (NULL);
	return (v);
}

SCPreferencesRef
btcf_prefs_open(const char *path)
{
	SCPreferencesRef prefs;
	CFStringRef app, id;

	/*
	 * The name is the caller, as wland passes its app ID; the prefsID picks
	 * the file. wland keeps KnownNetworks in the shared preferences.plist
	 * (prefsID NULL); bluetoothd needs two files with different modes, so
	 * it names them, by full path.
	 */
	app = btcf_str("org.nextbsd.bluetoothd");
	id = btcf_str(path);
	prefs = (app != NULL && id != NULL) ?
	    SCPreferencesCreate(NULL, app, id) : NULL;
	if (app != NULL)
		CFRelease(app);
	if (id != NULL)
		CFRelease(id);
	return (prefs);
}

CFMutableDictionaryRef
btcf_prefs_copy_dict(SCPreferencesRef prefs, CFStringRef key)
{
	CFPropertyListRef v;

	v = SCPreferencesGetValue(prefs, key);
	if (v != NULL && CFGetTypeID(v) == CFDictionaryGetTypeID())
		return (CFDictionaryCreateMutableCopy(NULL, 0, v));
	return (CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

int
btcf_prefs_commit(SCPreferencesRef prefs, const char *path, mode_t mode)
{
	if (!SCPreferencesCommitChanges(prefs))
		return (-1);
	if (chmod(path, mode) != 0 && errno != ENOENT) {
		(void)fprintf(stderr, "bluetoothd[store] chmod %s: %s\n",
		    path, strerror(errno));
		return (-1);
	}
	return (0);
}
