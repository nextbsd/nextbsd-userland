/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_login.c — SetAutoLogin and SetLoginWindowOptions: Users & Login.app's
 * login settings (nextbsd/nextbsd#639).
 *
 * Both are keys of the login window's system preferences,
 * /Local/Library/Preferences/org.nextbsd.loginwindow.plist (the system
 * default domain, as CFPreferences' kCFPreferencesAnyUser), with
 * Darwin's names from com.apple.loginwindow:
 *
 *   autoLoginUser      the account to log in at startup. Absent: the
 *                      built-in rule of autologin-user (the only account,
 *                      "admin", without a password), so a machine nobody
 *                      has changed keeps logging admin in. An empty
 *                      string: automatic login off.
 *   SHOWFULLNAME       true: the login window asks for a name and a
 *                      password; false: it lists the users. Absent: true,
 *                      what NextBSD's login window has always done (on
 *                      macOS the list is the default).
 *   RestartDisabled,   true: no Restart, or no Shut Down, button on the
 *   ShutDownDisabled   login window. Absent: false.
 *
 * autologin-user and loginwindow read them, and they apply at the next
 * login window. The file is replaced through a temporary and a rename,
 * and keys this helper doesn't set are kept.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

/* Overridable only so a test can point it elsewhere. */
#ifndef LOGINWINDOW_PREFS
#define	LOGINWINDOW_PREFS \
	"/Local/Library/Preferences/org.nextbsd.loginwindow.plist"
#endif

#define	KEY_AUTO_LOGIN		CFSTR("autoLoginUser")
#define	KEY_SHOW_FULL_NAME	CFSTR("SHOWFULLNAME")
#define	KEY_RESTART_DISABLED	CFSTR("RestartDisabled")
#define	KEY_SHUTDOWN_DISABLED	CFSTR("ShutDownDisabled")

/* The preferences as they are, to change; empty if there are none. */
static CFMutableDictionaryRef
load_prefs(void)
{
	CFURLRef url;
	CFReadStreamRef stream;
	CFPropertyListRef plist = NULL;
	CFMutableDictionaryRef prefs = NULL;

	url = CFURLCreateFromFileSystemRepresentation(NULL,
	    (const UInt8 *)LOGINWINDOW_PREFS, strlen(LOGINWINDOW_PREFS), false);
	if (url != NULL) {
		stream = CFReadStreamCreateWithFile(NULL, url);
		CFRelease(url);
		if (stream != NULL) {
			if (CFReadStreamOpen(stream))
				plist = CFPropertyListCreateWithStream(NULL,
				    stream, 0, kCFPropertyListImmutable, NULL,
				    NULL);
			CFReadStreamClose(stream);
			CFRelease(stream);
		}
	}
	if (plist != NULL && CFGetTypeID(plist) == CFDictionaryGetTypeID())
		prefs = CFDictionaryCreateMutableCopy(NULL, 0, plist);
	else
		prefs = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
	if (plist != NULL)
		CFRelease(plist);
	return (prefs);
}

/* Write the preferences as an XML plist, mode 0644, through a temporary. */
static int
save_prefs(CFDictionaryRef prefs)
{
	char tmp[PATH_MAX];
	CFDataRef data;
	const UInt8 *bytes;
	size_t len, done = 0;
	ssize_t n;
	int fd, rc = SH_ERR_FAILED;

	data = CFPropertyListCreateData(NULL, prefs,
	    kCFPropertyListXMLFormat_v1_0, 0, NULL);
	if (data == NULL)
		return (SH_ERR_FAILED);
	bytes = CFDataGetBytePtr(data);
	len = (size_t)CFDataGetLength(data);
	(void)snprintf(tmp, sizeof(tmp), "%s.SettingsHelper", LOGINWINDOW_PREFS);
	(void)unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd == -1) {
		sh_log("%s: %s", tmp, strerror(errno));
		CFRelease(data);
		return (SH_ERR_FAILED);
	}
	while (done < len) {
		n = write(fd, bytes + done, len - done);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += (size_t)n;
	}
	if (done == len && fchmod(fd, 0644) == 0 && fsync(fd) == 0)
		rc = SH_OK;
	else
		sh_log("%s: %s", tmp, strerror(errno));
	(void)close(fd);
	if (rc == SH_OK && rename(tmp, LOGINWINDOW_PREFS) == -1) {
		sh_log("%s: %s", LOGINWINDOW_PREFS, strerror(errno));
		rc = SH_ERR_FAILED;
	}
	if (rc != SH_OK)
		(void)unlink(tmp);
	CFRelease(data);
	return (rc);
}

static int
set_auto_login(CFMutableDictionaryRef prefs, const char *user)
{
	CFStringRef name;

	name = CFStringCreateWithCString(NULL, user, kCFStringEncodingUTF8);
	if (name == NULL)
		return (SH_ERR_INVALID);
	CFDictionarySetValue(prefs, KEY_AUTO_LOGIN, name);
	CFRelease(name);
	return (save_prefs(prefs));
}

int
sh_login_set_auto_login(const char *user)
{
	CFMutableDictionaryRef prefs;
	int rc;

	if (user[0] != '\0' && !sh_user_is_passwordless(user))
		return (SH_ERR_INVALID);
	if ((prefs = load_prefs()) == NULL)
		return (SH_ERR_FAILED);
	rc = set_auto_login(prefs, user);
	CFRelease(prefs);
	return (rc);
}

/*
 * The account `user` is gone: if it was the one to log in automatically,
 * turn automatic login off rather than leave a name another account could
 * later be created under.
 */
int
sh_login_forget_user(const char *user)
{
	CFMutableDictionaryRef prefs;
	CFTypeRef v;
	char name[SH_USER_MAX + 1];
	int rc = SH_OK;

	if ((prefs = load_prefs()) == NULL)
		return (SH_ERR_FAILED);
	v = CFDictionaryGetValue(prefs, KEY_AUTO_LOGIN);
	if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID() &&
	    CFStringGetCString(v, name, sizeof(name), kCFStringEncodingUTF8) &&
	    strcmp(name, user) == 0)
		rc = set_auto_login(prefs, "");
	CFRelease(prefs);
	return (rc);
}

int
sh_login_set_options(bool show_name_fields, bool show_power_buttons)
{
	CFMutableDictionaryRef prefs;
	int rc;

	if ((prefs = load_prefs()) == NULL)
		return (SH_ERR_FAILED);
	CFDictionarySetValue(prefs, KEY_SHOW_FULL_NAME,
	    show_name_fields ? kCFBooleanTrue : kCFBooleanFalse);
	CFDictionarySetValue(prefs, KEY_RESTART_DISABLED,
	    show_power_buttons ? kCFBooleanFalse : kCFBooleanTrue);
	CFDictionarySetValue(prefs, KEY_SHUTDOWN_DISABLED,
	    show_power_buttons ? kCFBooleanFalse : kCFBooleanTrue);
	rc = save_prefs(prefs);
	CFRelease(prefs);
	return (rc);
}
