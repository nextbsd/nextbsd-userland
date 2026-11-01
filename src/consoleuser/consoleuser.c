/*
 * consoleuser — publish who is logged in at the console
 * (nextbsd-userland#199).
 *
 *   consoleuser set <user>   State:/Users/ConsoleUser = { Name, UID, GID,
 *                            SessionInfo = [ one session ] }
 *   consoleuser clear        remove State:/Users/ConsoleUser
 *   consoleuser show         print the console user, or "none"
 *
 * On a Mac, loginwindow calls SCDynamicStoreSetConsoleInformation after a
 * login and again (with nothing) after the logout. NextBSD's login
 * programs are not Apple's, so this tool is the publisher they call:
 * Gershwin's LoginWindow after pam_open_session and again when the
 * session ends, or an admin by hand. The session dictionary uses Apple's
 * kSCConsoleSession* keys with LoginDone and OnConsole true, which is what
 * diskarbitrationd's console-user list keeps. set and clear need root;
 * configd refuses a non-root write to State:/Users/ as well.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecific.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecificPrivate.h>
#include <SystemConfiguration/SCDynamicStoreSetSpecificPrivate.h>

#include <err.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: consoleuser set user\n"
	    "       consoleuser clear\n"
	    "       consoleuser show\n");
	exit(EX_USAGE);
}

static SCDynamicStoreRef
store_open(void)
{
	SCDynamicStoreRef store;

	store = SCDynamicStoreCreate(NULL, CFSTR("consoleuser"), NULL, NULL);
	if (store == NULL)
		errx(EX_UNAVAILABLE, "cannot reach configd: %s",
		    SCErrorString(SCError()));
	return (store);
}

static void
dict_set_int(CFMutableDictionaryRef dict, CFStringRef key, SInt32 value)
{
	CFNumberRef num;

	num = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
	if (num != NULL) {
		CFDictionarySetValue(dict, key, num);
		CFRelease(num);
	}
}

static int
do_set(const char *name)
{
	CFMutableDictionaryRef session;
	CFMutableArrayRef sessions;
	SCDynamicStoreRef store;
	CFStringRef user;
	struct passwd *pw;
	Boolean ok;

	pw = getpwnam(name);
	if (pw == NULL)
		errx(EX_NOUSER, "%s: no such user", name);
	user = CFStringCreateWithCString(NULL, pw->pw_name,
	    kCFStringEncodingUTF8);
	session = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	sessions = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (user == NULL || session == NULL || sessions == NULL)
		errx(EX_OSERR, "out of memory");
	CFDictionarySetValue(session, kSCConsoleSessionUserName, user);
	dict_set_int(session, kSCConsoleSessionUID, (SInt32)pw->pw_uid);
	dict_set_int(session, kSCConsoleSessionID, 1);
	dict_set_int(session, kSCConsoleSessionConsoleSet, 0);
	CFDictionarySetValue(session, kSCConsoleSessionOnConsole,
	    kCFBooleanTrue);
	CFDictionarySetValue(session, kSCConsoleSessionLoginDone,
	    kCFBooleanTrue);
	CFArrayAppendValue(sessions, session);
	CFRelease(session);
	CFRelease(user);

	store = store_open();
	ok = SCDynamicStoreSetConsoleInformation(store, pw->pw_name,
	    pw->pw_uid, pw->pw_gid, sessions);
	CFRelease(sessions);
	CFRelease(store);
	if (!ok)
		errx(EX_NOPERM, "cannot set the console user: %s",
		    SCErrorString(SCError()));
	return (0);
}

static int
do_clear(void)
{
	SCDynamicStoreRef store;
	Boolean ok;

	store = store_open();
	ok = SCDynamicStoreSetConsoleInformation(store, NULL, 0, 0, NULL);
	CFRelease(store);
	/* Nobody was logged in: nothing to clear. */
	if (!ok && SCError() != kSCStatusNoKey)
		errx(EX_NOPERM, "cannot clear the console user: %s",
		    SCErrorString(SCError()));
	return (0);
}

static int
do_show(void)
{
	SCDynamicStoreRef store;
	CFStringRef user;
	CFArrayRef sessions;
	char name[256];
	uid_t uid = 0;
	gid_t gid = 0;

	store = store_open();
	user = SCDynamicStoreCopyConsoleUser(store, &uid, &gid);
	sessions = SCDynamicStoreCopyConsoleInformation(store);
	CFRelease(store);
	if (user == NULL) {
		(void)printf("none\n");
	} else {
		if (!CFStringGetCString(user, name, sizeof(name),
		    kCFStringEncodingUTF8))
			(void)strlcpy(name, "?", sizeof(name));
		(void)printf("%s uid=%u gid=%u sessions=%ld\n", name,
		    (unsigned)uid, (unsigned)gid, sessions != NULL ?
		    (long)CFArrayGetCount(sessions) : 0L);
		CFRelease(user);
	}
	if (sessions != NULL)
		CFRelease(sessions);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "show") == 0 && argc == 2)
		return (do_show());
	if (strcmp(argv[1], "set") == 0 && argc == 3) {
		if (geteuid() != 0)
			errx(EX_NOPERM, "set: must be root");
		return (do_set(argv[2]));
	}
	if (strcmp(argv[1], "clear") == 0 && argc == 2) {
		if (geteuid() != 0)
			errx(EX_NOPERM, "clear: must be root");
		return (do_clear());
	}
	usage();
	return (EX_USAGE);
}
