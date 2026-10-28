/*
 * kextd — push kext IOKitPersonalities into the in-kernel IOCatalogue (U1,
 * nextbsd#217).
 *
 * The Apple-faithful loader half of the in-kernel IOKit matcher (umbrella
 * #211, Option B, mechanism (a)): userland parses each kext bundle's
 * IOKitPersonalities and PUSHES them to the kernel; the kernel (K2 #215 store,
 * K3 #216 matcher) owns matching. This is the analog of Apple kextd's boot-time
 * IOCatalogueSendData.
 *
 * What it does now (push): open every kext under a repository (default
 * /System/Library/Extensions) with the OSKext engine, take each personality's
 * device-id match table (IOProviderClass / IOProbeScore / IOPCIPrimaryMatch),
 * and submit one flat record per personality to /dev/iocatalogue via
 * IOCATIOCADD. It first IOCATIOCFLUSHes, so running it again simply re-pushes
 * the current set (idempotent). The kernel never parses XML — kextd does.
 *
 * Invoked on demand for now (e.g. by the on-image IOKit test). The launchd
 * boot-time auto-push is deferred to K3 (#216): a RunAtLoad CF/OSKext job this
 * early wedges launchd's boot dispatch, so kextd grows its boot integration
 * once it is a persistent daemon whose ordering is designed.
 *
 * Not yet (lands with K3, #216): listening on /dev/devctl for the kernel's
 * "IOKIT load <bundle-id>" requests and kextload-ing the named bundle. For now
 * kextd populates the catalogue and exits; the matcher that consumes it is K3.
 *
 * It also keeps the module-name index (nextbsd-userland#439, see
 * build_module_index()); `-m <name>` resolves one name through it.
 *
 * Usage:  kextd [-r repo_dir] [-v] | -l word | -m module | -w | -t word
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>	/* kern_return_t, before OSKext.h -> OSReturn.h */
#include <mach/mach_traps.h>	/* mach_task_self, mach_host_self, mach_msg */
#include <mach/mach_port.h>	/* mach_port_allocate, mach_port_insert_right */
#include <mach/host_special_ports.h>	/* host_set_special_port, HOST_KEXTD_PORT */
#include <mach/message.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <mach/mach.h>		/* vm_allocate, vm_deallocate */
#include <mach/mig_errors.h>	/* mig_reply_error_t, MIG_BAD_* */
#include <servers/bootstrap.h>	/* bootstrap_check_in */

#include "OSKext.h"
#include "KextManagerPriv.h"	/* kKextLoad*Key: the load request's keys */
#include "kextmanager_migServer.h"	/* MIG: kextmanager_server() + handlers */
#include "iocatalogue.h"	/* vendored ABI; canonical copy in nextbsd-kernel */

#ifndef VM_FLAGS_ANYWHERE
#define	VM_FLAGS_ANYWHERE	0x0001	/* Apple's value; libmach lacks it */
#endif

#ifndef HOST_KEXTD_PORT
#define HOST_KEXTD_PORT 15
#endif

/* kernel->kextd load request wire format (mirror of the kernel's
 * iokit_kextd_load_msg_t in sys/mach/iokit_kextd.h; NDR_record_t is 8 bytes). */
#define	IOKIT_KEXTD_LOAD_MSGID	0x494f4b54
typedef struct {
	mach_msg_header_t	hdr;
	unsigned char		ndr[8];
	char			bundle_id[128];
	char			device[64];
	uint32_t		match_word;
} kextd_load_body_t;

static bool verbose;

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

static CFURLRef
url_for_path(const char *path)
{
	return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)path, (CFIndex)strlen(path), true);
}

/* Copy a CFString value into a C buffer; returns false if absent/not a string. */
static bool
dict_string(CFDictionaryRef d, CFStringRef key, char *buf, size_t buflen)
{
	CFStringRef s = CFDictionaryGetValue(d, key);

	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID())
		return (false);
	return (CFStringGetCString(s, buf, (CFIndex)buflen,
	    kCFStringEncodingUTF8) != false);
}

/* Fetch an integer CFNumber value; returns def if absent/not a number. */
static int32_t
dict_int(CFDictionaryRef d, CFStringRef key, int32_t def)
{
	CFNumberRef n = CFDictionaryGetValue(d, key);
	int32_t v;

	if (n == NULL || CFGetTypeID(n) != CFNumberGetTypeID())
		return (def);
	if (!CFNumberGetValue(n, kCFNumberSInt32Type, &v))
		return (def);
	return (v);
}

/*
 * Parse an IOPCIPrimaryMatch string ("0x24f38086 0x...") into a freshly
 * malloc'd array of match words. Returns the count (0 on none/absent); *out is
 * set to the array (caller frees) or NULL.
 */
static uint32_t
parse_match(CFDictionaryRef d, uint32_t **out)
{
	char buf[8192];
	uint32_t *words;
	uint32_t n = 0;
	char *p, *tok, *save;

	*out = NULL;
	if (!dict_string(d, CFSTR("IOPCIPrimaryMatch"), buf, sizeof(buf)))
		return (0);

	words = calloc(IOCAT_MAX_MATCH, sizeof(*words));
	if (words == NULL)
		return (0);
	for (p = buf; (tok = strtok_r(p, " ,\t", &save)) != NULL; p = NULL) {
		if (n >= IOCAT_MAX_MATCH)
			break;
		words[n++] = (uint32_t)strtoul(tok, NULL, 16);
	}
	if (n == 0) {
		free(words);
		return (0);
	}
	*out = words;
	return (n);
}

/*
 * Parse IONameMatch into a freshly malloc'd fixed-width string table
 * (ncompat * IOCAT_COMPAT_MAX), which is what IOCATIOCADDCOMPAT expects.
 *
 * IONameMatch is Apple's key for matching a device-tree node, and on a real
 * Darwin personality it is either a single string or an array of them. Both are
 * accepted here for the same reason: a personality copied from a Darwin driver
 * should work unchanged.
 *
 * Fixed width rather than packed so the kernel can bound each copyin without
 * walking user memory looking for terminators -- see struct iocat_add_compat.
 * Returns the count (0 on none/absent); *out is the table (caller frees).
 */
static uint32_t
parse_name_match(CFDictionaryRef d, char **out, const char *prefix)
{
	CFTypeRef v;
	char *tbl;
	uint32_t n = 0;
	size_t pl = prefix != NULL ? strlen(prefix) : 0;

	*out = NULL;
	v = CFDictionaryGetValue(d, CFSTR("IONameMatch"));
	if (v == NULL)
		return (0);

	tbl = calloc(IOCAT_MAX_MATCH, IOCAT_COMPAT_MAX);
	if (tbl == NULL)
		return (0);

	/* Each slot leaves room for the prefix (#329: "acpi:"), copied in below. */
	if (CFGetTypeID(v) == CFStringGetTypeID()) {
		if (CFStringGetCString((CFStringRef)v, tbl + pl,
		    IOCAT_COMPAT_MAX - pl, kCFStringEncodingUTF8))
			n = 1;
	} else if (CFGetTypeID(v) == CFArrayGetTypeID()) {
		CFArrayRef a = (CFArrayRef)v;
		CFIndex i, cnt = CFArrayGetCount(a);

		for (i = 0; i < cnt && n < IOCAT_MAX_MATCH; i++) {
			CFStringRef e = CFArrayGetValueAtIndex(a, i);

			if (e == NULL || CFGetTypeID(e) != CFStringGetTypeID())
				continue;
			if (CFStringGetCString(e,
			    tbl + (size_t)n * IOCAT_COMPAT_MAX + pl,
			    IOCAT_COMPAT_MAX - pl, kCFStringEncodingUTF8))
				n++;
		}
	}
	if (n == 0) {
		free(tbl);
		return (0);
	}
	if (pl > 0) {
		uint32_t i;

		for (i = 0; i < n; i++)
			memcpy(tbl + (size_t)i * IOCAT_COMPAT_MAX, prefix, pl);
	}
	*out = tbl;
	return (n);
}

/*
 * Push a device-tree personality (nextbsd-kernel-extensions-graphics#185).
 *
 * Separate from the PCI push because it uses a different ioctl and record --
 * struct iocat_add is a kernel ABI shared with the in-kernel catalogue, so the
 * device-tree case was added alongside it rather than by widening it.
 */
static int
push_platform_personality(int fd, CFDictionaryRef p, const char *bundle_id,
    const char *prefix)
{
	struct iocat_add_compat add;
	char *tbl;
	uint32_t n;
	int rc;

	n = parse_name_match(p, &tbl, prefix);
	if (n == 0)
		return (0);	/* no IONameMatch -> nothing to match on */

	memset(&add, 0, sizeof(add));
	strlcpy(add.bundle_id, bundle_id, sizeof(add.bundle_id));
	add.probe_score = dict_int(p, CFSTR("IOProbeScore"), 0);
	add.ncompat = n;
	add.compat = (uint64_t)(uintptr_t)tbl;

	rc = ioctl(fd, IOCATIOCADDCOMPAT, &add);
	free(tbl);
	if (rc != 0) {
		warn("IOCATIOCADDCOMPAT %s", add.bundle_id);
		return (-1);
	}
	if (verbose)
		printf("kextd: pushed %s (%s, score %d, %u name(s))\n",
		    add.bundle_id, prefix != NULL ? "IOACPIPlatformDevice" :
		    "IOPlatformDevice", add.probe_score, n);
	return (1);
}

/* Push one word table under `provider` (IOCATIOCADD). Takes ownership of words. */
static int
push_words(int fd, const char *bundle_id, uint32_t provider, int32_t score,
    uint32_t *words, uint32_t n, const char *label)
{
	struct iocat_add add;
	int rc;

	memset(&add, 0, sizeof(add));
	strlcpy(add.bundle_id, bundle_id, sizeof(add.bundle_id));
	add.provider_class = provider;
	add.probe_score = score;
	add.nmatch = n;
	add.match = (uint64_t)(uintptr_t)words;
	rc = ioctl(fd, IOCATIOCADD, &add);
	free(words);
	if (rc != 0) {
		warn("IOCATIOCADD %s (%s)", bundle_id, label);
		return (-1);
	}
	if (verbose)
		printf("kextd: pushed %s (%s, score %d, %u words)\n",
		    bundle_id, label, score, n);
	return (1);
}

/*
 * IOPCIClassMatch (#329): Darwin's "0x01080200&0xffffff00" form, one or more
 * space-separated, each a value&mask over 0x<class><subclass><progif><rev>.
 * A bare value means an exact match. Returns (value, mask) pairs.
 */
static uint32_t
parse_class_match(CFDictionaryRef d, uint32_t **out)
{
	char buf[4096];
	uint32_t *words, n = 0;
	char *p, *tok, *save, *amp;

	*out = NULL;
	if (!dict_string(d, CFSTR("IOPCIClassMatch"), buf, sizeof(buf)))
		return (0);
	words = calloc(IOCAT_MAX_MATCH, sizeof(*words));
	if (words == NULL)
		return (0);
	for (p = buf; (tok = strtok_r(p, " ,\t", &save)) != NULL; p = NULL) {
		if (n + 2 > IOCAT_MAX_MATCH)
			break;
		amp = strchr(tok, '&');
		if (amp != NULL)
			*amp++ = '\0';
		words[n++] = (uint32_t)strtoul(tok, NULL, 16);
		words[n++] = amp != NULL ? (uint32_t)strtoul(amp, NULL, 16) :
		    0xffffffff;
	}
	if (n == 0) {
		free(words);
		return (0);
	}
	*out = words;
	return (n);
}

/*
 * A USB personality (#329), in Darwin's keys: IOUSBHostDevice matches
 * idVendor + idProduct; IOUSBHostInterface matches idVendor + idProduct when
 * both are given, otherwise any of bInterfaceClass / bInterfaceSubClass /
 * bInterfaceProtocol (the ones present are significant, the rest masked out).
 */
static int
push_usb_personality(int fd, CFDictionaryRef p, const char *bundle_id,
    bool interface)
{
	int32_t vid, pid, cls, sub, proto, score;
	uint32_t *w, val = 0, mask = 0;

	score = dict_int(p, CFSTR("IOProbeScore"), 0);
	vid = dict_int(p, CFSTR("idVendor"), -1);
	pid = dict_int(p, CFSTR("idProduct"), -1);
	if (vid >= 0 && pid >= 0) {
		if ((w = calloc(1, sizeof(*w))) == NULL)
			return (-1);
		w[0] = ((uint32_t)pid << 16) | ((uint32_t)vid & 0xffff);
		return (push_words(fd, bundle_id, IOCAT_PROVIDER_IOUSBHOSTDEVICE,
		    score, w, 1, "IOUSBHostDevice"));
	}
	if (!interface)
		return (0);	/* a device personality without ids: nothing */
	cls = dict_int(p, CFSTR("bInterfaceClass"), -1);
	sub = dict_int(p, CFSTR("bInterfaceSubClass"), -1);
	proto = dict_int(p, CFSTR("bInterfaceProtocol"), -1);
	if (cls >= 0) { val |= ((uint32_t)cls & 0xff) << 16; mask |= 0xff0000; }
	if (sub >= 0) { val |= ((uint32_t)sub & 0xff) << 8; mask |= 0xff00; }
	if (proto >= 0) { val |= (uint32_t)proto & 0xff; mask |= 0xff; }
	if (mask == 0)
		return (0);
	if ((w = calloc(2, sizeof(*w))) == NULL)
		return (-1);
	w[0] = val;
	w[1] = mask;
	return (push_words(fd, bundle_id, IOCAT_PROVIDER_IOUSBHOSTINTERFACE,
	    score, w, 2, "IOUSBHostInterface"));
}

/* Push one personality dict; returns 1 if pushed, 0 if skipped, -1 on error. */
static int
push_personality(int fd, CFDictionaryRef p)
{
	struct iocat_add add;
	char provider[64];
	uint32_t *words;
	uint32_t n;
	int rc;

	memset(&add, 0, sizeof(add));
	if (!dict_string(p, CFSTR("CFBundleIdentifier"), add.bundle_id,
	    sizeof(add.bundle_id)))
		return (0);	/* no bundle id -> can't be loaded; skip */

	if (!dict_string(p, CFSTR("IOProviderClass"), provider,
	    sizeof(provider)))
		provider[0] = '\0';
	if (strcmp(provider, "IOPCIDevice") == 0)
		add.provider_class = IOCAT_PROVIDER_IOPCIDEVICE;
	else if (strcmp(provider, "IOPlatformDevice") == 0)
		/* Device tree: different record and ioctl entirely (#185). */
		return (push_platform_personality(fd, p, add.bundle_id, NULL));
	else if (strcmp(provider, "IOACPIPlatformDevice") == 0)
		/* ACPI _HID/_CID: the same string path, prefixed (#329). */
		return (push_platform_personality(fd, p, add.bundle_id,
		    IOCAT_ACPI_PREFIX));
	else if (strcmp(provider, "IOUSBHostDevice") == 0)
		return (push_usb_personality(fd, p, add.bundle_id, false));
	else if (strcmp(provider, "IOUSBHostInterface") == 0)
		return (push_usb_personality(fd, p, add.bundle_id, true));
	else
		add.provider_class = IOCAT_PROVIDER_UNKNOWN;

	/* IOPCIClassMatch (#329), alongside or instead of IOPCIPrimaryMatch. */
	if (add.provider_class == IOCAT_PROVIDER_IOPCIDEVICE) {
		uint32_t *cw, cn;

		cn = parse_class_match(p, &cw);
		if (cn > 0 && push_words(fd, add.bundle_id,
		    IOCAT_PROVIDER_IOPCICLASS,
		    dict_int(p, CFSTR("IOProbeScore"), 0), cw, cn,
		    "IOPCIClassMatch") < 0)
			return (-1);
		if (cn > 0 && !CFDictionaryContainsKey(p,
		    CFSTR("IOPCIPrimaryMatch")))
			return (1);
	}

	n = parse_match(p, &words);
	if (n == 0)
		return (0);	/* no device-id match table -> nothing to match */

	add.probe_score = dict_int(p, CFSTR("IOProbeScore"), 0);
	add.nmatch = n;
	add.match = (uint64_t)(uintptr_t)words;

	rc = ioctl(fd, IOCATIOCADD, &add);
	free(words);
	if (rc != 0) {
		warn("IOCATIOCADD %s", add.bundle_id);
		return (-1);
	}
	if (verbose)
		printf("kextd: pushed %s (%s, score %d, %u ids)\n",
		    add.bundle_id,
		    add.provider_class == IOCAT_PROVIDER_IOPCIDEVICE ?
		    "IOPCIDevice" : "?", add.probe_score, n);
	return (1);
}

/*
 * -l <matchword>: ask the kernel which driver bundle claims a PCI id
 * (0x<device><vendor>) via IOCATIOCLOOKUP — the same lookup the in-kernel
 * device_nomatch matcher (K3) uses. Lets the matcher be verified deterministically
 * (e.g. 0x24f38086 -> org.nextbsd.driver.IntelIwlwifi) without the physical device.
 */
static int
do_lookup(const char *word)
{
	struct iocat_lookup lu;
	int fd, rc;

	memset(&lu, 0, sizeof(lu));
	lu.match = (uint32_t)strtoul(word, NULL, 0);
	fd = open("/dev/iocatalogue", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/iocatalogue (is the K2 kernel loaded?)");
	rc = ioctl(fd, IOCATIOCLOOKUP, &lu);
	close(fd);
	if (rc == 0)
		printf("LOOKUP 0x%08x -> %s score %d\n", lu.match,
		    lu.bundle_id, lu.score);
	else if (errno == ENOTTY)	/* kernel without K3a's IOCATIOCLOOKUP */
		printf("LOOKUP 0x%08x -> unsupported\n", lu.match);
	else
		printf("LOOKUP 0x%08x -> (none)\n", lu.match);
	return (rc == 0 ? 0 : 1);
}

/*
 * The module-name index (E21 step 24, nextbsd-userland#439): FreeBSD module
 * name -> kext bundle ID, NextBSD's equivalent of linker.hints. Each kext built
 * from a FreeBSD module records what it provides in its Info.plist:
 * NBSDModuleFile is the .ko it wraps ("if_em.ko") and NBSDModules the module
 * and MODULE_VERSION interface names in it ("wlan", "wlan_ccmp", ...). Kernel
 * load requests by name (nextbsd-kernel#52) and the filesystem loader
 * (nextbsd-userland#436) resolve through it.
 *
 * Both kinds of name go in one dictionary: module names as they are, file
 * names under their .ko name and their bare stem, so a name resolves the way
 * kldload(2) takes it ("if_em", "if_em.ko" or "wlan_ccmp"). The first kext
 * to claim a name keeps it.
 */
static CFMutableDictionaryRef module_index;

static void
index_name(CFStringRef name, CFStringRef bundle_id)
{
	if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID() &&
	    !CFDictionaryContainsKey(module_index, name))
		CFDictionarySetValue(module_index, name, bundle_id);
}

static void
build_module_index(CFArrayRef kexts)
{
	CFIndex i, j, n;

	if (module_index != NULL)
		CFRelease(module_index);
	module_index = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (module_index == NULL || kexts == NULL)
		return;
	n = CFArrayGetCount(kexts);
	for (i = 0; i < n; i++) {
		OSKextRef k = (OSKextRef)CFArrayGetValueAtIndex(kexts, i);
		CFStringRef id = OSKextGetIdentifier(k);
		CFArrayRef mods;
		CFStringRef file;

		if (id == NULL)
			continue;
		mods = OSKextGetValueForInfoDictionaryKey(k,
		    CFSTR("NBSDModules"));
		if (mods != NULL && CFGetTypeID(mods) == CFArrayGetTypeID())
			for (j = 0; j < CFArrayGetCount(mods); j++)
				index_name(CFArrayGetValueAtIndex(mods, j), id);
		file = OSKextGetValueForInfoDictionaryKey(k,
		    CFSTR("NBSDModuleFile"));
		if (file != NULL && CFGetTypeID(file) == CFStringGetTypeID()) {
			index_name(file, id);
			if (CFStringHasSuffix(file, CFSTR(".ko"))) {
				CFStringRef stem = CFStringCreateWithSubstring(
				    kCFAllocatorDefault, file,
				    CFRangeMake(0, CFStringGetLength(file) - 3));

				if (stem != NULL) {
					index_name(stem, id);
					CFRelease(stem);
				}
			}
		}
	}
	if (verbose)
		printf("kextd: module index: %ld names\n",
		    (long)CFDictionaryGetCount(module_index));
}

/* The bundle ID providing module or file `name`, or NULL. */
static CFStringRef
lookup_module(const char *name)
{
	CFStringRef key, id;

	if (module_index == NULL)
		return (NULL);
	key = CFStringCreateWithCString(kCFAllocatorDefault, name,
	    kCFStringEncodingUTF8);
	if (key == NULL)
		return (NULL);
	id = CFDictionaryGetValue(module_index, key);
	CFRelease(key);
	return (id);
}

/* -m <name>: resolve a module or .ko name through the index and exit. */
static int
do_module_lookup(const char *repo, const char *name)
{
	CFArrayRef kexts;
	CFStringRef id;
	CFURLRef u;
	char buf[128];

	u = url_for_path(repo);
	kexts = (u != NULL) ?
	    OSKextCreateKextsFromURL(kCFAllocatorDefault, u) : NULL;
	if (u != NULL)
		CFRelease(u);
	if (kexts == NULL)
		errx(1, "%s: no kexts found", repo);
	build_module_index(kexts);
	id = lookup_module(name);
	if (id != NULL && CFStringGetCString(id, buf, sizeof(buf),
	    kCFStringEncodingUTF8))
		printf("MODULE %s -> %s\n", name, buf);
	else
		printf("MODULE %s -> (none)\n", name);
	CFRelease(kexts);
	return (id != NULL ? 0 : 1);
}

/* Load a kext by CFBundleIdentifier from the already-open repo set. */
static void
load_bundle(const char *bundle_id)
{
	CFStringRef idstr;
	OSKextRef k;
	OSReturn r;

	idstr = CFStringCreateWithCString(kCFAllocatorDefault, bundle_id,
	    kCFStringEncodingUTF8);
	if (idstr == NULL)
		return;
	k = OSKextGetKextWithIdentifier(idstr);	/* from the open repo set */
	CFRelease(idstr);
	if (k == NULL) {
		printf("kextd: no kext with identifier %s\n", bundle_id);
		return;
	}
	r = OSKextLoad(k);
	if (r == kOSReturnSuccess)
		printf("kextd: loaded %s\n", bundle_id);
	else {
		printf("kextd: load %s failed (OSReturn 0x%x)\n", bundle_id,
		    (unsigned)r);
		OSKextLogDiagnostics(k, kOSKextDiagnosticsFlagAll);
	}
}

/*
 * ---- KextManager server (nextbsd-userland#438) ----
 *
 * kextd serves the kextmanager MIG subsystem (kext.subproj's
 * kextmanager_mig.defs) under org.nextbsd.KernelExtensionServer, the
 * MachServices name its launchd plist declares; Apple's is
 * com.apple.KernelExtensionServer. KextManager in libIOKit is the client:
 * KextManagerLoadKextWithIdentifier() and friends. With the kernel no longer
 * loading modules by name (E21 D9), this is how userland asks for a kext,
 * e.g. a mount helper for its filesystem (#436).
 *
 * The handlers follow Apple's kextd_request.c of the same kext_tools release:
 * loads are by identifier or path, allowed for root or for a kext whose
 * Info.plist sets OSBundleAllowUserLoad. NextBSD has no kernel caches to
 * rebuild, so the cache, volume and reboot locks always succeed at once.
 */
static const char *repo_path = "/System/Library/Extensions";
static CFArrayRef repo_kexts;	/* non-retaining registry: hold alive */

/* (Re)open the repository, so kexts installed since start are found, and
 * rebuild the module-name index from it. */
static void
refresh_repo(void)
{
	CFArrayRef kexts;
	CFURLRef u;

	u = url_for_path(repo_path);
	if (u == NULL)
		return;
	kexts = OSKextCreateKextsFromURL(kCFAllocatorDefault, u);
	CFRelease(u);
	if (kexts == NULL)
		return;
	if (repo_kexts != NULL)
		CFRelease(repo_kexts);
	repo_kexts = kexts;
	build_module_index(repo_kexts);
}

static bool
allow_user_load(OSKextRef k)
{
	CFTypeRef v = OSKextGetValueForInfoDictionaryKey(k,
	    CFSTR("OSBundleAllowUserLoad"));

	return (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue((CFBooleanRef)v));
}

kern_return_t
_kextmanager_load_kext(mach_port_t server __unused, audit_token_t remote_creds,
    xmlDataIn_t load_data, mach_msg_type_number_t load_dataCnt)
{
	OSReturn result = kOSReturnError;
	CFDataRef data = NULL;
	CFPropertyListRef plist = NULL;
	CFMutableArrayRef held = NULL;	/* kexts created for this request */
	CFStringRef ident, path;
	CFArrayRef deps;
	OSKextRef kext = NULL;
	uid_t euid = (uid_t)remote_creds.val[1];
	char what[MAXPATHLEN] = "?";
	CFIndex i;

	held = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)load_data,
	    (CFIndex)load_dataCnt);
	if (held == NULL || data == NULL) {
		result = kOSKextReturnNoMemory;
		goto finish;
	}
	plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
	    kCFPropertyListImmutable, NULL, NULL);
	if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		result = kOSKextReturnSerialization;
		goto finish;
	}

	/* Kexts and folders the caller wants its kext's dependencies found in. */
	deps = CFDictionaryGetValue(plist, kKextLoadDependenciesKey);
	if (deps != NULL && CFGetTypeID(deps) == CFArrayGetTypeID()) {
		for (i = 0; i < CFArrayGetCount(deps); i++) {
			CFStringRef p = CFArrayGetValueAtIndex(deps, i);
			CFURLRef u;
			CFArrayRef ks;

			if (p == NULL || CFGetTypeID(p) != CFStringGetTypeID())
				continue;
			u = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, p,
			    kCFURLPOSIXPathStyle, true);
			if (u == NULL)
				continue;
			ks = OSKextCreateKextsFromURL(kCFAllocatorDefault, u);
			CFRelease(u);
			if (ks != NULL) {
				CFArrayAppendValue(held, ks);
				CFRelease(ks);
			}
		}
	}

	ident = CFDictionaryGetValue(plist, kKextLoadIdentifierKey);
	path = CFDictionaryGetValue(plist, kKextLoadPathKey);
	if (ident != NULL && CFGetTypeID(ident) == CFStringGetTypeID()) {
		CFStringGetCString(ident, what, sizeof(what),
		    kCFStringEncodingUTF8);
		refresh_repo();
		kext = OSKextGetKextWithIdentifier(ident);
	} else if (path != NULL && CFGetTypeID(path) == CFStringGetTypeID()) {
		CFURLRef u;

		CFStringGetCString(path, what, sizeof(what),
		    kCFStringEncodingUTF8);
		u = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, path,
		    kCFURLPOSIXPathStyle, true);
		if (u != NULL) {
			kext = OSKextCreate(kCFAllocatorDefault, u);
			CFRelease(u);
			if (kext != NULL) {
				CFArrayAppendValue(held, kext);
				CFRelease(kext);
			}
		}
	} else {
		result = kOSKextReturnInvalidArgument;
		goto finish;
	}
	if (kext == NULL) {
		result = kOSKextReturnNotFound;
		goto finish;
	}
	if (euid != 0 && !allow_user_load(kext)) {
		result = kOSKextReturnNotPrivileged;
		goto finish;
	}
	result = OSKextLoad(kext);
	if (result != kOSReturnSuccess)
		OSKextLogDiagnostics(kext, kOSKextDiagnosticsFlagAll);

finish:
	printf("kextd: KextManager load %s for uid %u: %s (0x%x)\n", what,
	    (unsigned)euid, result == kOSReturnSuccess ? "loaded" : "failed",
	    (unsigned)result);
	if (plist != NULL)
		CFRelease(plist);
	if (data != NULL)
		CFRelease(data);
	if (held != NULL)
		CFRelease(held);
	/* The request arrived out of line; it's ours to free. */
	vm_deallocate(mach_task_self(), (vm_address_t)load_data, load_dataCnt);
	return (result);
}

kern_return_t
_kextmanager_path_for_bundle_id(mach_port_t server __unused,
    kext_bundle_id_t bundle_id, posix_path_t path, kext_result_t *kext_result)
{
	CFStringRef ident;
	OSKextRef k;
	CFURLRef u;

	path[0] = '\0';
	*kext_result = kOSKextReturnNotFound;
	bundle_id[sizeof(kext_bundle_id_t) - 1] = '\0';
	ident = CFStringCreateWithCString(kCFAllocatorDefault, bundle_id,
	    kCFStringEncodingUTF8);
	if (ident == NULL)
		return (KERN_SUCCESS);
	refresh_repo();
	k = OSKextGetKextWithIdentifier(ident);
	CFRelease(ident);
	if (k == NULL || (u = OSKextGetURL(k)) == NULL)
		return (KERN_SUCCESS);
	u = CFURLCopyAbsoluteURL(u);
	if (u != NULL) {
		if (CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path,
		    sizeof(posix_path_t)))
			*kext_result = kOSReturnSuccess;
		CFRelease(u);
	}
	return (KERN_SUCCESS);
}

/* Every kext whose Info.plist has `prop_key`, as an array of dictionaries
 * holding its CFBundleIdentifier and the key's value (binary plist). */
kern_return_t
_kextmanager_create_property_value_array(mach_port_t server __unused,
    property_key_t prop_key, xmlDataOut_t *data,
    mach_msg_type_number_t *dataCnt)
{
	CFMutableArrayRef out;
	CFStringRef key;
	CFDataRef bytes = NULL;
	vm_address_t buf = 0;
	CFIndex i, n;

	*data = NULL;
	*dataCnt = 0;
	prop_key[sizeof(property_key_t) - 1] = '\0';
	key = CFStringCreateWithCString(kCFAllocatorDefault, prop_key,
	    kCFStringEncodingUTF8);
	out = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	if (key == NULL || out == NULL)
		goto finish;
	refresh_repo();
	n = repo_kexts != NULL ? CFArrayGetCount(repo_kexts) : 0;
	for (i = 0; i < n; i++) {
		OSKextRef k = (OSKextRef)CFArrayGetValueAtIndex(repo_kexts, i);
		CFTypeRef v = OSKextGetValueForInfoDictionaryKey(k, key);
		CFMutableDictionaryRef d;

		if (v == NULL)
			continue;
		d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (d == NULL)
			continue;
		CFDictionarySetValue(d, kCFBundleIdentifierKey,
		    OSKextGetIdentifier(k));
		CFDictionarySetValue(d, key, v);
		CFArrayAppendValue(out, d);
		CFRelease(d);
	}
	bytes = CFPropertyListCreateData(kCFAllocatorDefault, out,
	    kCFPropertyListBinaryFormat_v1_0, 0, NULL);
	if (bytes == NULL || vm_allocate(mach_task_self(), &buf,
	    (vm_size_t)CFDataGetLength(bytes), VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
		goto finish;
	memcpy((void *)buf, CFDataGetBytePtr(bytes), (size_t)CFDataGetLength(bytes));
	*data = (xmlDataOut_t)buf;
	*dataCnt = (mach_msg_type_number_t)CFDataGetLength(bytes);
finish:
	if (bytes != NULL)
		CFRelease(bytes);
	if (out != NULL)
		CFRelease(out);
	if (key != NULL)
		CFRelease(key);
	return (KERN_SUCCESS);
}

/* No kernel caches on NextBSD: the locks kextcache and reboot take are
 * granted at once. Rights the client sent are released. */
kern_return_t
_kextmanager_lock_reboot(mach_port_t server __unused, mach_port_t reply __unused,
    mach_port_t reaper, int waitForLock __unused, mountpoint_t busyVol,
    int *busyStatus)
{
	if (MACH_PORT_VALID(reaper))
		mach_port_deallocate(mach_task_self(), reaper);
	busyVol[0] = '\0';
	*busyStatus = 0;
	return (KERN_SUCCESS);
}

kern_return_t
_kextmanager_lock_volume(mach_port_t server __unused, mach_port_t reply __unused,
    mach_port_t reaper, uuid_t vol_uuid __unused, int waitForLock __unused,
    int *lockStatus)
{
	if (MACH_PORT_VALID(reaper))
		mach_port_deallocate(mach_task_self(), reaper);
	*lockStatus = 0;
	return (KERN_SUCCESS);
}

kern_return_t
_kextmanager_unlock_volume(mach_port_t server __unused, mach_port_t clientIdent,
    uuid_t vol_uuid __unused, int exitStatus __unused)
{
	if (MACH_PORT_VALID(clientIdent))
		mach_port_deallocate(mach_task_self(), clientIdent);
	return (KERN_SUCCESS);
}

kern_return_t
_kextmanager_lock_kextload(mach_port_t server __unused, mach_port_t clientPort,
    int *lockstatus)
{
	if (MACH_PORT_VALID(clientPort))
		mach_port_deallocate(mach_task_self(), clientPort);
	*lockstatus = 0;
	return (KERN_SUCCESS);
}

kern_return_t
_kextmanager_unlock_kextload(mach_port_t server __unused, mach_port_t clientPort)
{
	if (MACH_PORT_VALID(clientPort))
		mach_port_deallocate(mach_task_self(), clientPort);
	return (KERN_SUCCESS);
}

/* Receive options: the audit trailer carries the caller's identity, which
 * load_kext's ServerAuditToken argument reads (MIG rejects it without). */
#define	KEXTD_RCV_TRAILER						\
	(MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |		\
	 MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT))
#define	KEXTD_MSG_BUFSZ	16384

/* Serve one KextManager request and send its reply, as configd does. */
static void
serve_kextmanager(mach_msg_header_t *req)
{
	union {
		mach_msg_header_t hdr;
		char buf[KEXTD_MSG_BUFSZ];
	} rep;
	mach_msg_return_t mr;
	kern_return_t code;
	int rejected;

	memset(&rep, 0, sizeof(rep));
	if (!kextmanager_server(req, &rep.hdr)) {
		mach_msg_destroy(req);
		return;
	}
	/*
	 * The handlers above always take ownership of what a request carries,
	 * even when they fail; only a request MIG refused before calling one
	 * (bad id, bad arguments, no audit trailer) still holds out-of-line
	 * memory and rights. Release those, keeping the reply right.
	 */
	code = ((mig_reply_error_t *)(void *)&rep.hdr)->RetCode;
	rejected = !(rep.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
	    (code == MIG_BAD_ID || code == MIG_BAD_ARGUMENTS ||
	    code == MIG_TRAILER_ERROR);
	if (rejected) {
		req->msgh_remote_port = MACH_PORT_NULL;
		mach_msg_destroy(req);
	}
	if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
		mr = mach_msg(&rep.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
		    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS)
			mach_msg_destroy(&rep.hdr);
	}
}

/*
 * Watch mode (the real daemon, U1/K3b): register HOST_KEXTD_PORT so the kernel
 * matcher can reach us, push the repo's personalities into the IOCatalogue
 * (which, with push-triggers-match, fires load requests for already-unmatched
 * devices), then serve kernel load requests forever — loading each named bundle
 * via OSKext (kldload re-probes and the device attaches). No matching here.
 */
static int
do_watch(int fd, const char *repo)
{
	mach_port_name_t task = mach_task_self();
	mach_port_name_t host = mach_host_self();
	mach_port_name_t port = MACH_PORT_NULL;
	mach_port_t service = MACH_PORT_NULL, pset = MACH_PORT_NULL;
	CFArrayRef personalities;
	kern_return_t kr;
	union {
		kextd_load_body_t body;
		char raw[KEXTD_MSG_BUFSZ];
	} buf;

	kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS)
		errx(1, "mach_port_allocate: 0x%x", (unsigned)kr);
	kr = mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS)
		errx(1, "mach_port_insert_right: 0x%x", (unsigned)kr);
	kr = host_set_special_port(host, HOST_KEXTD_PORT, port);
	if (kr != KERN_SUCCESS)
		errx(1, "host_set_special_port(HOST_KEXTD_PORT): 0x%x", (unsigned)kr);
	/* Startup step markers are verbose-only — under -v they bracket each
	 * call so a hang's last line pinpoints the blocker; at default the
	 * daemon is quiet and logs only meaningful events (loaded, errors). */
	if (verbose)
		printf("kextd: listening on HOST_KEXTD_PORT (port 0x%x)\n", port);

	/* KextManager requests arrive on the launchd service port; kernel
	 * load requests on HOST_KEXTD_PORT. One port set receives both. */
	kr = mach_port_allocate(task, MACH_PORT_RIGHT_PORT_SET, &pset);
	if (kr != KERN_SUCCESS)
		errx(1, "mach_port_allocate(port set): 0x%x", (unsigned)kr);
	kr = mach_port_move_member(task, port, pset);
	if (kr != KERN_SUCCESS)
		errx(1, "mach_port_move_member: 0x%x", (unsigned)kr);
	kr = bootstrap_check_in(bootstrap_port, KEXTD_SERVER_NAME, &service);
	if (kr == KERN_SUCCESS)
		kr = mach_port_move_member(task, service, pset);
	if (kr != KERN_SUCCESS)
		printf("kextd: not serving %s (0x%x); kernel requests only\n",
		    KEXTD_SERVER_NAME, (unsigned)kr);
	else if (verbose)
		printf("kextd: serving %s\n", KEXTD_SERVER_NAME);

	if (verbose)
		printf("kextd: opening repo %s\n", repo);
	repo_path = repo;
	refresh_repo();
	if (repo_kexts == NULL)
		errx(1, "%s: no kexts found", repo);
	if (verbose)
		printf("kextd: repo opened; copying personalities\n");
	(void) ioctl(fd, IOCATIOCFLUSH);
	personalities = OSKextCopyPersonalitiesOfKexts(NULL);
	if (personalities != NULL) {
		CFIndex n = CFArrayGetCount(personalities), i;
		int pushed = 0;

		for (i = 0; i < n; i++) {
			CFDictionaryRef p = CFArrayGetValueAtIndex(personalities, i);
			if (p != NULL && CFGetTypeID(p) == CFDictionaryGetTypeID() &&
			    push_personality(fd, p) > 0)
				pushed++;
		}
		if (verbose)
			printf("kextd: pushed %d personalities\n", pushed);
		CFRelease(personalities);
	}

	if (verbose)
		printf("kextd: ready\n");
	for (;;) {
		mach_msg_return_t mr;

		memset(&buf, 0, sizeof(buf));
		mr = mach_msg(&buf.body.hdr, MACH_RCV_MSG | KEXTD_RCV_TRAILER, 0,
		    sizeof(buf), pset, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS) {
			fprintf(stderr, "kextd: mach_msg(RCV) 0x%x\n", (unsigned)mr);
			continue;
		}
		if (buf.body.hdr.msgh_local_port == service) {
			serve_kextmanager(&buf.body.hdr);
			continue;
		}
		if (buf.body.hdr.msgh_id != IOKIT_KEXTD_LOAD_MSGID)
			continue;
		buf.body.bundle_id[sizeof(buf.body.bundle_id) - 1] = '\0';
		if (verbose)
			printf("kextd: load request bundle=%s device=%s match=0x%08x\n",
			    buf.body.bundle_id, buf.body.device, buf.body.match_word);
		load_bundle(buf.body.bundle_id);
	}
	/* NOTREACHED */
}

/* -t <word>: drive the kernel matcher's send for a PCI id (IOCATIOCTESTSEND),
 * WITHOUT registering HOST_KEXTD_PORT — so it injects a request to a separately
 * running `kextd -w`. CI uses this to exercise the daemon's receive+load. */
static int
do_test_send(int fd, const char *word)
{
	uint32_t mw = (uint32_t)strtoul(word, NULL, 0);
	int rc = ioctl(fd, IOCATIOCTESTSEND, &mw);

	printf("kextd: test-send 0x%08x rc=%d errno=%d\n", mw, rc,
	    rc != 0 ? errno : 0);
	return (rc == 0 ? 0 : 1);
}

int
main(int argc, char *argv[])
{
	const char *repo = "/System/Library/Extensions";
	const char *lookup = NULL;
	const char *testsend = NULL;
	const char *module = NULL;
	bool watch = false;
	CFArrayRef repoKexts = NULL;	/* non-retaining registry: hold alive */
	CFArrayRef personalities = NULL;
	CFURLRef u;
	int fd, ch;
	int pushed = 0, skipped = 0, failed = 0;
	CFIndex i, count;

	/* Line-buffer stdout: in -w (daemon) mode our output is redirected to a
	 * file (fully buffered), and the test kill()s us — buffered diagnostics
	 * would be lost. Line buffering flushes each message as it's printed. */
	setlinebuf(stdout);

	while ((ch = getopt(argc, argv, "r:vl:m:wt:")) != -1) {
		switch (ch) {
		case 'r':
			repo = optarg;
			break;
		case 'v':
			verbose = true;
			break;
		case 'l':
			lookup = optarg;
			break;
		case 'm':
			module = optarg;
			break;
		case 'w':
			watch = true;
			break;
		case 't':
			testsend = optarg;
			break;
		default:
			errx(2, "usage: kextd [-r repo] [-v] | -l word | -m module | -w | -t word");
		}
	}

	/* Query mode: just resolve one PCI id against the catalogue and exit. */
	if (lookup != NULL)
		return (do_lookup(lookup));

	OSKextSetLogOutputFunction(&tool_log);
	if (verbose)
		OSKextSetLogFilter(kOSKextLogDetailLevel |
		    kOSKextLogVerboseFlagsMask, false);

	/* Query mode: resolve one module name through the index and exit. */
	if (module != NULL)
		return (do_module_lookup(repo, module));

	fd = open("/dev/iocatalogue", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/iocatalogue (is the K2 kernel loaded?)");

	/* Trigger a synthetic load request to a running `kextd -w` (CI helper). */
	if (testsend != NULL) {
		int rc = do_test_send(fd, testsend);
		close(fd);
		return (rc);
	}

	/* Watch mode: register HOST_KEXTD_PORT, push, then serve load requests. */
	if (watch)
		return (do_watch(fd, repo));	/* does not return */

	/* Idempotent: drop the prior set, then re-push the current repo. */
	if (ioctl(fd, IOCATIOCFLUSH) != 0)
		warn("IOCATIOCFLUSH");

	u = url_for_path(repo);
	if (u == NULL)
		errx(1, "%s: cannot form URL", repo);
	repoKexts = OSKextCreateKextsFromURL(kCFAllocatorDefault, u);
	CFRelease(u);
	if (repoKexts == NULL)
		errx(1, "%s: no kexts found", repo);

	/* All personalities across all open kexts, processed for the kernel. */
	personalities = OSKextCopyPersonalitiesOfKexts(NULL);
	if (personalities == NULL)
		errx(1, "no personalities in %s", repo);

	count = CFArrayGetCount(personalities);
	for (i = 0; i < count; i++) {
		CFDictionaryRef p = CFArrayGetValueAtIndex(personalities, i);
		int r;

		if (p == NULL || CFGetTypeID(p) != CFDictionaryGetTypeID())
			continue;
		r = push_personality(fd, p);
		if (r > 0)
			pushed++;
		else if (r == 0)
			skipped++;
		else
			failed++;
	}

	printf("kextd: %d personalities pushed, %d skipped, %d failed (repo %s)\n",
	    pushed, skipped, failed, repo);

	CFRelease(personalities);
	CFRelease(repoKexts);
	close(fd);
	return (failed > 0 ? 1 : 0);
}
