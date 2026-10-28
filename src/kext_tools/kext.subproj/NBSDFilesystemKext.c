/*
 * NBSDFilesystemKext.c — load the kext a filesystem type needs before
 * mounting it (nextbsd-userland#436). See NBSDFilesystemKext.h.
 *
 * Like KextManager.c, this creates no OSKext objects in the caller: it reads
 * Info.plists directly and leaves the load to kextd.
 */
#include <sys/param.h>
#include <sys/mount.h>		/* getvfsbyname, struct xvfsconf */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <AvailabilityMacros.h>	/* KextManager.h's AVAILABLE_MAC_OS_X_* */
#include <CoreFoundation/CoreFoundation.h>

#include "OSKext.h"		/* kOSKextReturnNotPrivileged */
#include "KextManager.h"
#include "NBSDFilesystemKext.h"

#define	FILESYSTEMS_DIR	"/System/Library/Filesystems"
#define	EXTENSIONS_DIR	"/System/Library/Extensions"

static int
have_vfs(const char *fstype)
{
	struct xvfsconf vfc;

	return (getvfsbyname(fstype, &vfc) == 0);
}

static CFDictionaryRef
read_plist(const char *path)
{
	CFURLRef url;
	CFReadStreamRef stream;
	CFPropertyListRef plist = NULL;

	url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)path, (CFIndex)strlen(path), false);
	if (url == NULL)
		return (NULL);
	stream = CFReadStreamCreateWithFile(kCFAllocatorDefault, url);
	CFRelease(url);
	if (stream == NULL)
		return (NULL);
	if (CFReadStreamOpen(stream)) {
		plist = CFPropertyListCreateWithStream(kCFAllocatorDefault,
		    stream, 0, kCFPropertyListImmutable, NULL, NULL);
		CFReadStreamClose(stream);
	}
	CFRelease(stream);
	if (plist != NULL && CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		plist = NULL;
	}
	return ((CFDictionaryRef)plist);
}

/* A copy of the string value for `key`, or NULL. */
static CFStringRef
copy_string(CFDictionaryRef d, CFStringRef key)
{
	CFStringRef s = CFDictionaryGetValue(d, key);

	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID())
		return (NULL);
	return (CFStringCreateCopy(kCFAllocatorDefault, s));
}

/* 1. The filesystem bundle names its kext. */
static CFStringRef
kext_from_fs_bundle(const char *fstype)
{
	char path[MAXPATHLEN];
	CFDictionaryRef info;
	CFStringRef ident;

	snprintf(path, sizeof(path), "%s/%s.fs/Contents/Info.plist",
	    FILESYSTEMS_DIR, fstype);
	if ((info = read_plist(path)) == NULL)
		return (NULL);
	ident = copy_string(info, CFSTR("NBSDKextIdentifier"));
	CFRelease(info);
	return (ident);
}

/* 2. The kext that lists `fstype` among the module names it provides. */
static CFStringRef
kext_from_module_names(const char *fstype)
{
	char path[MAXPATHLEN];
	CFStringRef want, ident = NULL;
	DIR *dir;
	struct dirent *de;

	want = CFStringCreateWithCString(kCFAllocatorDefault, fstype,
	    kCFStringEncodingUTF8);
	if (want == NULL || (dir = opendir(EXTENSIONS_DIR)) == NULL) {
		if (want != NULL)
			CFRelease(want);
		return (NULL);
	}
	while (ident == NULL && (de = readdir(dir)) != NULL) {
		size_t n = strlen(de->d_name);
		CFDictionaryRef info;
		CFArrayRef mods;

		if (n < 6 || strcmp(de->d_name + n - 5, ".kext") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/Contents/Info.plist",
		    EXTENSIONS_DIR, de->d_name);
		if ((info = read_plist(path)) == NULL)
			continue;
		mods = CFDictionaryGetValue(info, CFSTR("NBSDModules"));
		if (mods != NULL && CFGetTypeID(mods) == CFArrayGetTypeID() &&
		    CFArrayContainsValue(mods,
		    CFRangeMake(0, CFArrayGetCount(mods)), want))
			ident = copy_string(info, kCFBundleIdentifierKey);
		CFRelease(info);
	}
	closedir(dir);
	CFRelease(want);
	return (ident);
}

int
NBSDFilesystemLoadKext(const char *fstype)
{
	CFStringRef ident;
	OSReturn r;

	if (fstype == NULL || *fstype == '\0' || strchr(fstype, '/') != NULL)
		return (EINVAL);
	if (have_vfs(fstype))
		return (0);

	ident = kext_from_fs_bundle(fstype);
	if (ident == NULL)
		ident = kext_from_module_names(fstype);
	if (ident == NULL)
		return (ENOENT);

	r = KextManagerLoadKextWithIdentifier(ident, NULL);
	CFRelease(ident);
	if (r == kOSKextReturnNotPrivileged)
		return (EPERM);
	if (r != kOSReturnSuccess)
		return (EIO);
	return (have_vfs(fstype) ? 0 : ENODEV);
}
