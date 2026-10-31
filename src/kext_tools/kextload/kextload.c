/*
 * kextload — load a kext and its dependencies, in order (NextBSD #182/#196).
 *
 * Graduated from the minimal #183 kld loader to Apple's OSKext engine: it
 * resolves the kext's OSBundleLibraries dependency graph and kldload(2)s the
 * dependency-ordered load list (OSKextLoad). The system extensions folders
 * (/System/Library/Extensions, /Local/Library/Extensions), plus any
 * -repository directories and -dependency kexts, supply the libraries the
 * kext names. kld performs each load; there is no codesign and personalities
 * are kextd's job (#177). For a self-contained kext with no dependencies this
 * is equivalent to the old single kldload, but it now loads libraries first,
 * correctly.
 *
 * Kexts are named by bundle path, or by CFBundleIdentifier with -bundle-id
 * (nextbsd#270), looked up among the same known kexts, as in Apple's kextload.
 * Both go through OSKextLoad, so every load gets the same checks (validation,
 * architecture, and the kernel check: a kext's NBSDKernelABIVersion must match
 * the running kernel's ABI, major.minor of kern.osrelease; a kext without it
 * falls back to the NBSDKernelBuildID match against System.kext).
 *
 * Usage:  kextload [-b bundle_id] [-d kext] [-r repo_dir] [-q] [-h] [--]
 *             [bundle.kext ...]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>	/* kern_return_t, before OSKext.h -> OSReturn.h */

#include <err.h>
#include <getopt.h>
#include <limits.h>	/* PATH_MAX */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "OSKext.h"

static int quiet;

static const struct option longopts[] = {
	{ "bundle-id",	required_argument,	NULL,	'b' },
	{ "dependency",	required_argument,	NULL,	'd' },
	{ "help",	no_argument,		NULL,	'h' },
	{ "quiet",	no_argument,		NULL,	'q' },
	{ "repository",	required_argument,	NULL,	'r' },
	{ NULL,		0,			NULL,	0 }
};

static void
usage(int status)
{
	fprintf(status == 0 ? stdout : stderr,
	    "usage: kextload [-b bundle_id] [-d kext] [-r repo_dir] [-q] [-h]"
	    " [--] [bundle.kext ...]\n"
	    "  -b, -bundle-id id    load the kext with CFBundleIdentifier id\n"
	    "  -d, -dependency kext add kext to the known kexts\n"
	    "  -r, -repository dir  add the kexts in dir to the known kexts\n"
	    "  -q, -quiet           print no informational or error messages\n"
	    "  -h, -help            print this message and exit\n"
	    "The kexts in /System/Library/Extensions and"
	    " /Local/Library/Extensions are always known.\n");
	exit(status);
}

static CFURLRef
url_for_path(const char *path)
{
	return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)path, (CFIndex)strlen(path), true);
}

/*
 * Route OSKext library log/diagnostic output to stderr so a failed load
 * explains itself (which property/executable check failed) instead of just
 * an opaque OSReturn. Mirrors what Apple's kextload does.
 */
static void
tool_log(OSKextRef aKext __unused, OSKextLogSpec spec __unused,
    const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static void
quiet_log(OSKextRef aKext __unused, OSKextLogSpec spec __unused,
    const char *format __unused, ...)
{
}

/*
 * Open the kexts under `path` (a repository directory) into OSKext's
 * registry. The registry is non-retaining, so the kexts are kept in `held`
 * until the loads are done. A missing directory is skipped.
 */
static void
add_repository(CFMutableArrayRef held, const char *path, int must_exist)
{
	CFURLRef   u;
	CFArrayRef kexts;

	if (access(path, F_OK) != 0) {
		if (must_exist && !quiet)
			warn("%s", path);
		return;
	}
	if ((u = url_for_path(path)) == NULL)
		return;
	kexts = OSKextCreateKextsFromURL(kCFAllocatorDefault, u);
	CFRelease(u);
	if (kexts != NULL) {
		CFArrayAppendArray(held, kexts,
		    CFRangeMake(0, CFArrayGetCount(kexts)));
		CFRelease(kexts);
	} else if (must_exist && !quiet)
		warnx("%s: cannot read kexts", path);
}

/* Open one kext bundle (and its plugins) into the registry; NULL on error. */
static OSKextRef
open_kext(CFMutableArrayRef held, const char *path)
{
	CFURLRef  u = url_for_path(path);
	OSKextRef k = (u != NULL) ? OSKextCreate(kCFAllocatorDefault, u) : NULL;

	if (u != NULL)
		CFRelease(u);
	if (k == NULL) {
		if (!quiet)
			warnx("%s: cannot open kext bundle", path);
		return (NULL);
	}
	CFArrayAppendValue(held, k);
	CFRelease(k);
	return (k);
}

/* OSKextLoad `k` (named `name` in messages); 0 on success, else 1. */
static int
load_kext(OSKextRef k, const char *name)
{
	OSReturn r = OSKextLoad(k);

	if (r == kOSReturnSuccess) {
		if (!quiet)
			printf("kextload: loaded %s (dependency-ordered)\n",
			    name);
		return (0);
	}
	if (!quiet) {
		warnx("%s: load failed (OSReturn 0x%x)", name, (unsigned)r);
		/* Print the specific validation/dependency problems. */
		OSKextLogDiagnostics(k, kOSKextDiagnosticsFlagAll);
	}
	return (1);
}

int
main(int argc, char *argv[])
{
	CFMutableArrayRef held;		/* registry is non-retaining */
	CFArrayRef        sysFolders;
	const char      **ids, **deps, **repos;
	int               nids = 0, ndeps = 0, nrepos = 0;
	int               ch, rc = 0, i;

	/* Each option can appear at most argc times. */
	ids = calloc(argc, sizeof(*ids));
	deps = calloc(argc, sizeof(*deps));
	repos = calloc(argc, sizeof(*repos));
	if (ids == NULL || deps == NULL || repos == NULL)
		err(1, "calloc");

	/* Apple's long options take one dash (-bundle-id) as well as two. */
	while ((ch = getopt_long_only(argc, argv, "b:d:hqr:", longopts,
	    NULL)) != -1) {
		switch (ch) {
		case 'b':
			ids[nids++] = optarg;
			break;
		case 'd':
			deps[ndeps++] = optarg;
			break;
		case 'h':
			usage(0);
			break;
		case 'q':
			quiet = 1;
			break;
		case 'r':
			repos[nrepos++] = optarg;
			break;
		default:
			usage(2);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 && nids < 1)
		usage(2);

	/*
	 * Record diagnostics and route library messages to stderr, so a
	 * validation/dependency failure prints the specific reason.
	 */
	OSKextSetLogOutputFunction(quiet ? &quiet_log : &tool_log);
	OSKextSetLogFilter(kOSKextLogDetailLevel | kOSKextLogVerboseFlagsMask,
	    /* kernelFlag */ false);
	OSKextSetRecordsDiagnostics(kOSKextDiagnosticsFlagAll);

	held = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	if (held == NULL)
		errx(1, "out of memory");

	/*
	 * The known kexts: the system extensions folders, then -repository
	 * directories and -dependency kexts. They resolve OSBundleLibraries
	 * and -bundle-id lookups; System.kext among them is what OSKextLoad
	 * checks a legacy kext's NBSDKernelBuildID against, and supplies the
	 * kernel ABI version when kern.osrelease doesn't carry one.
	 */
	sysFolders = OSKextGetSystemExtensionsFolderURLs();
	for (i = 0; sysFolders != NULL && i < CFArrayGetCount(sysFolders); i++) {
		char path[PATH_MAX];

		if (CFURLGetFileSystemRepresentation(
		    CFArrayGetValueAtIndex(sysFolders, i), true,
		    (UInt8 *)path, sizeof(path)))
			add_repository(held, path, 0);
	}
	for (i = 0; i < nrepos; i++)
		add_repository(held, repos[i], 1);
	for (i = 0; i < ndeps; i++)
		if (open_kext(held, deps[i]) == NULL)
			rc = 1;

	/* Kexts named by path: open each, so it also joins the known set. */
	OSKextRef *pathKexts = calloc(argc + 1, sizeof(*pathKexts));
	if (pathKexts == NULL)
		err(1, "calloc");
	for (i = 0; i < argc; i++)
		if ((pathKexts[i] = open_kext(held, argv[i])) == NULL)
			rc = 1;

	/*
	 * Kexts named by identifier: the highest CFBundleVersion among the
	 * known kexts. Loaded through the same OSKextLoad as path kexts.
	 */
	for (i = 0; i < nids; i++) {
		CFStringRef id = CFStringCreateWithCString(kCFAllocatorDefault,
		    ids[i], kCFStringEncodingUTF8);
		OSKextRef   k = (id != NULL)
		    ? OSKextGetKextWithIdentifier(id) : NULL;

		if (id != NULL)
			CFRelease(id);
		if (k == NULL) {
			if (!quiet)
				warnx("%s: no kext with this bundle identifier"
				    " found", ids[i]);
			rc = 1;
			continue;
		}
		rc |= load_kext(k, ids[i]);
	}

	for (i = 0; i < argc; i++)
		if (pathKexts[i] != NULL)
			rc |= load_kext(pathKexts[i], argv[i]);

	CFRelease(held);
	free(pathKexts);
	free(ids);
	free(deps);
	free(repos);
	return (rc);
}
