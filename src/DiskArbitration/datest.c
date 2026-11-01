/*
 * datest — a DiskArbitration client (nextbsd-userland#197).
 *
 * A C port of the useful part of Apple's datest/datest.m: it links
 * libDiskArbitration and talks to diskarbitrationd the way any client does.
 *
 *   datest                     boot check: DASessionCreate, wait for the
 *                              disk list, then DADiskCopyDescription of the
 *                              root disk; prints DA-BOOT-OK, or DA-BOOT-FAIL
 *                              and exits 1 (run.sh runs this; the marker
 *                              gates in tests/boot-test.sh)
 *   datest --list              one line per disk: the appeared stream, then
 *                              DiskListComplete
 *   datest --watch [--keepalive]
 *                              print every callback until interrupted:
 *                              appeared, disappeared, description changed,
 *                              list complete, idle; --keepalive survives a
 *                              daemon restart (DASessionKeepAlive)
 *   datest --describe <name>   DADiskCopyDescription of a BSD name (vtbd0p2,
 *                              /dev/da0) or a mount point (/, /Volumes/X)
 *   datest --mount <name> [<path>]
 *                              DADiskMount, then the mount callback
 *   datest --unmount <name> [--force] [--whole]
 *                              DADiskUnmount, then the unmount callback
 *                              (the dissenter's status, process ID and
 *                              string when refused) and, on success, the
 *                              DescriptionChanged that drops DAVolumePath
 *   datest --eject <name>      DADiskEject, then the eject callback
 *   datest --approve | --dissent [<string>] | --stall
 *                              register unmount and eject approval
 *                              callbacks and run until interrupted:
 *                              approve each, dissent with
 *                              kDAReturnNotPermitted and <string>, or
 *                              stall 30 s without answering (the daemon
 *                              gives up after 10 s + 1 s)
 *
 * --unmount and --eject exit 0 on success, 1 when refused.
 *
 * --verbose adds the whole description dictionary to --list / --describe.
 */
#include <DiskArbitration/DiskArbitration.h>
#include <DiskArbitration/DiskArbitrationPrivate.h>

#include <dispatch/dispatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DA_SERVICE	"com.apple.DiskArbitration.diskarbitrationd"

static dispatch_queue_t		g_queue;
static dispatch_semaphore_t	g_list_complete;
static dispatch_semaphore_t	g_idle;
static dispatch_semaphore_t	g_done;
static CFMutableArrayRef	g_disks;	/* appeared before list complete */
static int			g_listing;
static int			g_verbose;
static int			g_watch;
static unsigned long		g_events;
static int			g_result;	/* request mode exit status */
static dispatch_semaphore_t	g_changed;
static const char		*g_dissent;	/* --dissent string */
static int			g_approval;	/* 'a', 'd' or 's' */

static const char *
cstr(CFTypeRef v, char *buf, size_t len)
{
	buf[0] = '\0';
	if (v == NULL)
		return (NULL);
	if (CFGetTypeID(v) == CFStringGetTypeID()) {
		if (!CFStringGetCString(v, buf, (CFIndex)len,
		    kCFStringEncodingUTF8))
			return (NULL);
	} else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
		(void)strlcpy(buf, CFBooleanGetValue(v) ? "1" : "0", len);
	} else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
		long long n = 0;

		(void)CFNumberGetValue(v, kCFNumberLongLongType, &n);
		(void)snprintf(buf, len, "%lld", n);
	} else if (CFGetTypeID(v) == CFURLGetTypeID()) {
		if (!CFURLGetFileSystemRepresentation(v, TRUE, (UInt8 *)buf,
		    (CFIndex)len))
			return (NULL);
	} else if (CFGetTypeID(v) == CFUUIDGetTypeID()) {
		CFStringRef s = CFUUIDCreateString(kCFAllocatorDefault, v);

		if (s == NULL)
			return (NULL);
		(void)CFStringGetCString(s, buf, (CFIndex)len,
		    kCFStringEncodingUTF8);
		CFRelease(s);
	} else
		return (NULL);
	return (buf);
}

/* One line: the keys the ticket's --list acceptance names, and a few more. */
static void
print_description(const char *tag, DADiskRef disk, CFDictionaryRef d)
{
	static const struct {
		const char	*name;
		CFStringRef	*key;
	} keys[] = {
		{ "DAMediaBSDName",	(CFStringRef *)&kDADiskDescriptionMediaBSDNameKey },
		{ "DAMediaWhole",	(CFStringRef *)&kDADiskDescriptionMediaWholeKey },
		{ "DAMediaLeaf",	(CFStringRef *)&kDADiskDescriptionMediaLeafKey },
		{ "DAMediaSize",	(CFStringRef *)&kDADiskDescriptionMediaSizeKey },
		{ "DAMediaContent",	(CFStringRef *)&kDADiskDescriptionMediaContentKey },
		{ "DAVolumeKind",	(CFStringRef *)&kDADiskDescriptionVolumeKindKey },
		{ "DAVolumeName",	(CFStringRef *)&kDADiskDescriptionVolumeNameKey },
		{ "DAVolumePath",	(CFStringRef *)&kDADiskDescriptionVolumePathKey },
	};
	char line[2048], buf[1024];
	const char *bsd;
	size_t i;

	bsd = disk != NULL ? DADiskGetBSDName(disk) : NULL;
	(void)snprintf(line, sizeof(line), "%s: %s", tag,
	    bsd != NULL ? bsd : "(no BSD name)");
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		CFTypeRef v = d != NULL ? CFDictionaryGetValue(d, *keys[i].key) :
		    NULL;

		if (cstr(v, buf, sizeof(buf)) == NULL)
			continue;
		(void)strlcat(line, " ", sizeof(line));
		(void)strlcat(line, keys[i].name, sizeof(line));
		(void)strlcat(line, "=", sizeof(line));
		(void)strlcat(line, buf, sizeof(line));
	}
	(void)printf("%s\n", line);
	if (g_verbose && d != NULL) {
		CFStringRef s = CFCopyDescription(d);

		if (s != NULL) {
			char *big;
			CFIndex len = CFStringGetMaximumSizeForEncoding(
			    CFStringGetLength(s), kCFStringEncodingUTF8) + 1;

			big = malloc((size_t)len);
			if (big != NULL && CFStringGetCString(s, big, len,
			    kCFStringEncodingUTF8))
				(void)printf("%s\n", big);
			free(big);
			CFRelease(s);
		}
	}
	(void)fflush(stdout);
}

/* --- callbacks (on g_queue) ----------------------------------------------- */

static void
appeared(DADiskRef disk, void *context __unused)
{
	CFDictionaryRef d;

	g_events++;
	if (g_listing && g_disks != NULL)
		CFArrayAppendValue(g_disks, disk);
	if (g_watch) {
		d = DADiskCopyDescription(disk);
		print_description("DA-CLIENT appeared", disk, d);
		if (d != NULL)
			CFRelease(d);
	}
}

static void
disappeared(DADiskRef disk, void *context __unused)
{
	g_events++;
	if (g_watch)
		print_description("DA-CLIENT disappeared", disk, NULL);
}

static void
changed(DADiskRef disk, CFArrayRef keys, void *context __unused)
{
	CFDictionaryRef d;
	char names[1024], buf[256];
	CFIndex i;

	g_events++;
	if (!g_watch)
		return;
	names[0] = '\0';
	for (i = 0; i < CFArrayGetCount(keys); i++) {
		if (cstr(CFArrayGetValueAtIndex(keys, i), buf, sizeof(buf)) ==
		    NULL)
			continue;
		if (names[0] != '\0')
			(void)strlcat(names, ",", sizeof(names));
		(void)strlcat(names, buf, sizeof(names));
	}
	(void)printf("DA-CLIENT description changed: %s keys=%s\n",
	    DADiskGetBSDName(disk) != NULL ? DADiskGetBSDName(disk) : "?",
	    names);
	d = DADiskCopyDescription(disk);
	print_description("DA-CLIENT now", disk, d);
	if (d != NULL)
		CFRelease(d);
}

static void
list_complete(void *context __unused)
{
	g_events++;
	if (g_watch)
		(void)printf("DA-CLIENT list complete (%lu callbacks so far)\n",
		    g_events);
	(void)fflush(stdout);
	g_listing = 0;
	dispatch_semaphore_signal(g_list_complete);
}

static void
idle(void *context __unused)
{
	g_events++;
	if (g_watch)
		(void)printf("DA-CLIENT idle (%lu callbacks so far)\n",
		    g_events);
	(void)fflush(stdout);
	dispatch_semaphore_signal(g_idle);
}

static void
mounted(DADiskRef disk, DADissenterRef dissenter, void *context __unused)
{
	CFDictionaryRef d;

	if (dissenter != NULL)
		(void)printf("DA-CLIENT mount %s: refused, status 0x%08x\n",
		    DADiskGetBSDName(disk) != NULL ? DADiskGetBSDName(disk) :
		    "?", (unsigned)DADissenterGetStatus(dissenter));
	else {
		d = DADiskCopyDescription(disk);
		print_description("DA-CLIENT mounted", disk, d);
		if (d != NULL)
			CFRelease(d);
	}
	(void)fflush(stdout);
	dispatch_semaphore_signal(g_done);
}

static const char *
bsd_name(DADiskRef disk)
{
	const char *name = disk != NULL ? DADiskGetBSDName(disk) : NULL;

	return (name != NULL ? name : "?");
}

/* One line for a request's completion; sets the exit status. */
static void
report(const char *what, DADiskRef disk, DADissenterRef dissenter)
{
	CFStringRef string;
	char text[512];

	if (dissenter == NULL) {
		(void)printf("DA-CLIENT %s %s: ok\n", what, bsd_name(disk));
		g_result = 0;
	} else {
		text[0] = '\0';
		string = DADissenterGetStatusString(dissenter);
		if (string != NULL)
			(void)CFStringGetCString(string, text, sizeof(text),
			    kCFStringEncodingUTF8);
		(void)printf("DA-CLIENT %s %s: refused, status 0x%08x pid %d%s%s"
		    "%s\n", what, bsd_name(disk),
		    (unsigned)DADissenterGetStatus(dissenter),
		    (int)DADissenterGetProcessID(dissenter),
		    text[0] != '\0' ? " string \"" : "", text,
		    text[0] != '\0' ? "\"" : "");
		g_result = 1;
	}
	(void)fflush(stdout);
}

static void
unmounted(DADiskRef disk, DADissenterRef dissenter, void *context __unused)
{
	report("unmount", disk, dissenter);
	dispatch_semaphore_signal(g_done);
}

static void
ejected(DADiskRef disk, DADissenterRef dissenter, void *context __unused)
{
	report("eject", disk, dissenter);
	dispatch_semaphore_signal(g_done);
}

static void
path_changed(DADiskRef disk, CFArrayRef keys __unused, void *context __unused)
{
	CFDictionaryRef d;
	char buf[1024];

	d = DADiskCopyDescription(disk);
	(void)printf("DA-CLIENT description changed: %s DAVolumePath=%s\n",
	    bsd_name(disk), d != NULL && cstr(CFDictionaryGetValue(d,
	    kDADiskDescriptionVolumePathKey), buf, sizeof(buf)) != NULL ?
	    buf : "(none)");
	if (d != NULL)
		CFRelease(d);
	(void)fflush(stdout);
	dispatch_semaphore_signal(g_changed);
}

/* --approve / --dissent / --stall. */
static DADissenterRef
approval(const char *what, DADiskRef disk)
{
	CFStringRef string;
	DADissenterRef dissenter;

	switch (g_approval) {
	case 'd':
		string = CFStringCreateWithCString(kCFAllocatorDefault,
		    g_dissent != NULL ? g_dissent : "datest dissents",
		    kCFStringEncodingUTF8);
		dissenter = DADissenterCreate(kCFAllocatorDefault,
		    kDAReturnNotPermitted, string);
		if (string != NULL)
			CFRelease(string);
		(void)printf("DA-CLIENT %s approval %s: dissent\n", what,
		    bsd_name(disk));
		(void)fflush(stdout);
		return (dissenter);
	case 's':
		(void)printf("DA-CLIENT %s approval %s: stalling 30 s\n",
		    what, bsd_name(disk));
		(void)fflush(stdout);
		(void)sleep(30);
		(void)printf("DA-CLIENT %s approval %s: approving late\n",
		    what, bsd_name(disk));
		(void)fflush(stdout);
		return (NULL);
	default:
		(void)printf("DA-CLIENT %s approval %s: approve\n", what,
		    bsd_name(disk));
		(void)fflush(stdout);
		return (NULL);
	}
}

static DADissenterRef
unmount_approval(DADiskRef disk, void *context __unused)
{
	return (approval("unmount", disk));
}

static DADissenterRef
eject_approval(DADiskRef disk, void *context __unused)
{
	return (approval("eject", disk));
}

/* --- modes ---------------------------------------------------------------- */

static DASessionRef
session_open(void)
{
	DASessionRef session;

	session = DASessionCreate(kCFAllocatorDefault);
	if (session == NULL)
		return (NULL);
	DASessionSetDispatchQueue(session, g_queue);
	return (session);
}

static int
wait_for(dispatch_semaphore_t sem, int seconds)
{
	return (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)seconds * NSEC_PER_SEC)) == 0);
}

/* Register for the disk stream and wait for DiskListComplete. */
static int
collect(DASessionRef session, int seconds)
{
	g_disks = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	g_listing = 1;
	/* Apple: register list-complete before the appeared callback. */
	DARegisterDiskListCompleteCallback(session, list_complete, NULL);
	DARegisterIdleCallback(session, idle, NULL);
	DARegisterDiskAppearedCallback(session, NULL, appeared, NULL);
	return (wait_for(g_list_complete, seconds));
}

static DADiskRef
disk_named(DASessionRef session, const char *name)
{
	DADiskRef disk;
	CFURLRef url;

	if (name[0] == '/' && strncmp(name, "/dev/", 5) != 0) {
		url = CFURLCreateFromFileSystemRepresentation(
		    kCFAllocatorDefault, (const UInt8 *)name,
		    (CFIndex)strlen(name), TRUE);
		if (url == NULL)
			return (NULL);
		disk = DADiskCreateFromVolumePath(kCFAllocatorDefault, session,
		    url);
		CFRelease(url);
		return (disk);
	}
	return (DADiskCreateFromBSDName(kCFAllocatorDefault, session, name));
}

/*
 * Before a DA-BOOT-FAIL: the daemon's session lines, so the boot log says
 * why (the harness stops reading at the marker).
 */
static void
dump_daemon_log(void)
{
	char line[1024], *keep[20];
	FILE *f;
	int n = 0, i;

	f = fopen("/var/log/diskarbitrationd.stderr", "r");
	if (f == NULL)
		return;
	memset(keep, 0, sizeof(keep));
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, "DA-SESSION") == NULL &&
		    strstr(line, "DA-SERVICE") == NULL &&
		    strstr(line, "DA-STAGE: idle") == NULL)
			continue;
		free(keep[n % 20]);
		keep[n % 20] = strdup(line);
		n++;
	}
	(void)fclose(f);
	for (i = n > 20 ? n - 20 : 0; i < n; i++)
		if (keep[i % 20] != NULL)
			(void)printf("DA-CLIENT-DIAG: %s", keep[i % 20]);
	for (i = 0; i < 20; i++)
		free(keep[i]);
	(void)fflush(stdout);
}

static int
mode_boot(void)
{
	DASessionRef session;
	DADiskRef disk = NULL;
	CFDictionaryRef d = NULL;
	CFIndex i, n;
	char what[1024];
	int got_idle;

	session = session_open();
	if (session == NULL) {
		(void)printf("DA-BOOT-FAIL: DASessionCreate failed "
		    "(%s not reachable)\n", DA_SERVICE);
		return (1);
	}
	if (!collect(session, 60)) {
		dump_daemon_log();
		(void)printf("DA-BOOT-FAIL: no DiskListComplete from %s within "
		    "60 s\n", DA_SERVICE);
		return (1);
	}
	n = CFArrayGetCount(g_disks);
	for (i = 0; i < n; i++) {
		DADiskRef each = (DADiskRef)CFArrayGetValueAtIndex(g_disks, i);
		CFDictionaryRef dd = DADiskCopyDescription(each);

		print_description("DA-CLIENT-DISK", each, dd);
		if (dd != NULL)
			CFRelease(dd);
	}
	got_idle = wait_for(g_idle, 15);
	(void)printf("DA-CLIENT: %ld disk%s appeared, then DiskListComplete%s\n",
	    (long)n, n == 1 ? "" : "s", got_idle ? ", then Idle" :
	    " (no Idle within 15 s: the daemon is still staging)");

	/*
	 * The root disk: what "/" is mounted from. On live media "/" is a
	 * union mount the daemon does not adopt; then the first whole disk.
	 */
	disk = disk_named(session, "/");
	if (disk != NULL) {
		d = DADiskCopyDescription(disk);
		if (d == NULL) {
			(void)printf("DA-CLIENT: no description for %s, the "
			    "disk of /\n", DADiskGetBSDName(disk) != NULL ?
			    DADiskGetBSDName(disk) : "(no BSD name)");
			CFRelease(disk);
			disk = NULL;
		}
	} else
		(void)printf("DA-CLIENT: DADiskCreateFromVolumePath(/) "
		    "found no disk\n");
	if (disk == NULL) {
		for (i = 0; i < n && disk == NULL; i++) {
			DADiskRef each = (DADiskRef)CFArrayGetValueAtIndex(
			    g_disks, i);
			CFDictionaryRef dd = DADiskCopyDescription(each);

			if (dd != NULL && CFDictionaryGetValue(dd,
			    kDADiskDescriptionMediaWholeKey) == kCFBooleanTrue &&
			    DADiskGetBSDName(each) != NULL) {
				/* A fresh disk object: a real daemon round trip. */
				disk = DADiskCreateFromBSDName(
				    kCFAllocatorDefault, session,
				    DADiskGetBSDName(each));
				d = disk != NULL ? DADiskCopyDescription(disk) :
				    NULL;
				if (d == NULL && disk != NULL) {
					CFRelease(disk);
					disk = NULL;
				}
			}
			if (dd != NULL)
				CFRelease(dd);
		}
	}
	if (disk == NULL || d == NULL) {
		dump_daemon_log();
		(void)printf("DA-BOOT-FAIL: session up, but no root disk "
		    "description (%ld disks listed)\n", (long)n);
		return (1);
	}
	print_description("DA-CLIENT-ROOT", disk, d);
	(void)snprintf(what, sizeof(what), "%s",
	    DADiskGetBSDName(disk) != NULL ? DADiskGetBSDName(disk) : "?");
	(void)printf("DA-BOOT-OK: DASessionCreate + DADiskCopyDescription(%s) "
	    "via %s\n", what, DA_SERVICE);
	CFRelease(d);
	CFRelease(disk);
	CFRelease(session);
	return (0);
}

static int
mode_list(void)
{
	DASessionRef session;
	CFIndex i, n;

	session = session_open();
	if (session == NULL) {
		(void)fprintf(stderr, "datest: DASessionCreate failed\n");
		return (1);
	}
	if (!collect(session, 60)) {
		(void)fprintf(stderr, "datest: no DiskListComplete\n");
		return (1);
	}
	n = CFArrayGetCount(g_disks);
	for (i = 0; i < n; i++) {
		DADiskRef disk = (DADiskRef)CFArrayGetValueAtIndex(g_disks, i);
		CFDictionaryRef d = DADiskCopyDescription(disk);

		print_description("disk", disk, d);
		if (d != NULL)
			CFRelease(d);
	}
	return (0);
}

static int
mode_watch(int keepalive)
{
	DASessionRef session;

	g_watch = 1;
	session = session_open();
	if (session == NULL) {
		(void)fprintf(stderr, "datest: DASessionCreate failed\n");
		return (1);
	}
	if (keepalive && DASessionKeepAlive(session, g_queue) !=
	    kDAReturnSuccess)
		(void)fprintf(stderr, "datest: DASessionKeepAlive failed\n");
	DARegisterDiskDisappearedCallback(session, NULL, disappeared, NULL);
	DARegisterDiskDescriptionChangedCallback(session, NULL, NULL, changed,
	    NULL);
	(void)collect(session, 60);
	dispatch_main();
	/* NOTREACHED */
}

static int
mode_describe(const char *name)
{
	DASessionRef session;
	DADiskRef disk;
	CFDictionaryRef d;

	session = session_open();
	if (session == NULL) {
		(void)fprintf(stderr, "datest: DASessionCreate failed\n");
		return (1);
	}
	disk = disk_named(session, name);
	d = disk != NULL ? DADiskCopyDescription(disk) : NULL;
	if (d == NULL) {
		(void)fprintf(stderr, "datest: no disk %s\n", name);
		return (1);
	}
	print_description("disk", disk, d);
	CFRelease(d);
	CFRelease(disk);
	CFRelease(session);
	return (0);
}

static int
mode_mount(const char *name, const char *path)
{
	DASessionRef session;
	DADiskRef disk;
	CFURLRef url = NULL;

	session = session_open();
	if (session == NULL) {
		(void)fprintf(stderr, "datest: DASessionCreate failed\n");
		return (1);
	}
	disk = disk_named(session, name);
	if (disk == NULL) {
		(void)fprintf(stderr, "datest: no disk %s\n", name);
		return (1);
	}
	if (path != NULL)
		url = CFURLCreateFromFileSystemRepresentation(
		    kCFAllocatorDefault, (const UInt8 *)path,
		    (CFIndex)strlen(path), TRUE);
	DADiskMount(disk, url, kDADiskMountOptionDefault, mounted, NULL);
	if (!wait_for(g_done, 60)) {
		(void)fprintf(stderr, "datest: no mount callback in 60 s\n");
		return (1);
	}
	return (0);
}

static int
mode_unmount(const char *name, DADiskUnmountOptions options)
{
	DASessionRef session;
	DADiskRef disk;

	session = session_open();
	if (session == NULL) {
		(void)fprintf(stderr, "datest: DASessionCreate failed\n");
		return (1);
	}
	disk = disk_named(session, name);
	if (disk == NULL) {
		(void)fprintf(stderr, "datest: no disk %s\n", name);
		return (1);
	}
	DARegisterDiskDescriptionChangedCallback(session, NULL,
	    kDADiskDescriptionWatchVolumePath, path_changed, NULL);
	g_result = 1;
	DADiskUnmount(disk, options, unmounted, NULL);
	/* An approval session may take 10 s + 1 s per silent client. */
	if (!wait_for(g_done, 120)) {
		(void)fprintf(stderr, "datest: no unmount callback in 120 s\n");
		return (1);
	}
	if (g_result == 0 && !wait_for(g_changed, 5))
		(void)printf("DA-CLIENT: no DAVolumePath change within 5 s\n");
	return (g_result);
}

static int
mode_eject(const char *name)
{
	DASessionRef session;
	DADiskRef disk;

	session = session_open();
	if (session == NULL) {
		(void)fprintf(stderr, "datest: DASessionCreate failed\n");
		return (1);
	}
	disk = disk_named(session, name);
	if (disk == NULL) {
		(void)fprintf(stderr, "datest: no disk %s\n", name);
		return (1);
	}
	g_result = 1;
	DADiskEject(disk, kDADiskEjectOptionDefault, ejected, NULL);
	if (!wait_for(g_done, 300)) {
		(void)fprintf(stderr, "datest: no eject callback in 300 s\n");
		return (1);
	}
	return (g_result);
}

static int
mode_approval(void)
{
	DASessionRef session;

	session = session_open();
	if (session == NULL) {
		(void)fprintf(stderr, "datest: DASessionCreate failed\n");
		return (1);
	}
	DARegisterDiskUnmountApprovalCallback(session, NULL, unmount_approval,
	    NULL);
	DARegisterDiskEjectApprovalCallback(session, NULL, eject_approval,
	    NULL);
	(void)printf("DA-CLIENT: answering unmount and eject approvals (%s)\n",
	    g_approval == 'd' ? "dissent" : g_approval == 's' ? "stall" :
	    "approve");
	(void)fflush(stdout);
	dispatch_main();
	/* NOTREACHED */
}

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: datest [--verbose]\n"
	    "       datest [--verbose] --list\n"
	    "       datest --watch [--keepalive]\n"
	    "       datest [--verbose] --describe <bsdname | mount point>\n"
	    "       datest --mount <bsdname> [<path>]\n"
	    "       datest --unmount <bsdname | mount point> [--force] "
	    "[--whole]\n"
	    "       datest --eject <bsdname>\n"
	    "       datest --approve | --dissent [<string>] | --stall\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	int i, keepalive = 0;
	DADiskUnmountOptions unmount_options = kDADiskUnmountOptionDefault;
	const char *mode = NULL, *arg = NULL, *arg2 = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--verbose") == 0)
			g_verbose = 1;
		else if (strcmp(argv[i], "--keepalive") == 0)
			keepalive = 1;
		else if (strcmp(argv[i], "--force") == 0)
			unmount_options |= kDADiskUnmountOptionForce;
		else if (strcmp(argv[i], "--whole") == 0)
			unmount_options |= kDADiskUnmountOptionWhole;
		else if (strcmp(argv[i], "--approve") == 0 ||
		    strcmp(argv[i], "--stall") == 0) {
			mode = "--approval";
			g_approval = argv[i][2];
		} else if (strcmp(argv[i], "--dissent") == 0) {
			mode = "--approval";
			g_approval = 'd';
			if (i + 1 < argc && argv[i + 1][0] != '-')
				g_dissent = argv[++i];
		}
		else if (strcmp(argv[i], "--list") == 0 ||
		    strcmp(argv[i], "--watch") == 0)
			mode = argv[i];
		else if (strcmp(argv[i], "--describe") == 0 ||
		    strcmp(argv[i], "--mount") == 0 ||
		    strcmp(argv[i], "--unmount") == 0 ||
		    strcmp(argv[i], "--eject") == 0) {
			mode = argv[i];
			if (++i >= argc)
				usage();
			arg = argv[i];
			if (strcmp(mode, "--mount") == 0 && i + 1 < argc &&
			    argv[i + 1][0] != '-')
				arg2 = argv[++i];
		} else
			usage();
	}

	g_queue = dispatch_queue_create("org.nextbsd.datest", NULL);
	g_list_complete = dispatch_semaphore_create(0);
	g_idle = dispatch_semaphore_create(0);
	g_done = dispatch_semaphore_create(0);
	g_changed = dispatch_semaphore_create(0);

	if (mode == NULL)
		return (mode_boot());
	if (strcmp(mode, "--list") == 0)
		return (mode_list());
	if (strcmp(mode, "--watch") == 0)
		return (mode_watch(keepalive));
	if (strcmp(mode, "--describe") == 0)
		return (mode_describe(arg));
	if (strcmp(mode, "--unmount") == 0)
		return (mode_unmount(arg, unmount_options));
	if (strcmp(mode, "--eject") == 0)
		return (mode_eject(arg));
	if (strcmp(mode, "--approval") == 0)
		return (mode_approval());
	return (mode_mount(arg, arg2));
}
