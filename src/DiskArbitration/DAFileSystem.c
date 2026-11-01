/*
 * DAFileSystem.c — the filesystem bundle list and its probe candidates
 * (E15 C4, nextbsd-userland#194).
 *
 * Port of the bundle half of Apple's DASupport.m (DAFileSystemListRefresh,
 * the FSMediaTypes probe list sorted by FSProbeOrder) and the matching in
 * DAProbe.c. The daemon knows no filesystem itself: adding one is a bundle
 * with an Info.plist and a <name>.util helper, in /System/Library/
 * Filesystems or /Library/Filesystems.
 */
#include "DAFileSystem.h"
#include "DASupport.h"
#include "loadable_fs.h"

#include <sys/param.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const fs_dirs[] = { FS_DIR_LOCATION, FS_DIR_LOCATION_LOCAL };

static struct da_probe_candidate	*g_list;
static size_t				 g_count;
static struct timespec			 g_mtime[nitems(fs_dirs)];
static int				 g_loaded;

static void
candidate_clear(struct da_probe_candidate *c)
{
	free(c->bundle);
	free(c->kind);
	free(c->probe_exec);
	free(c->probe_args);
	free(c->repair_exec);
	free(c->mount_exec);
	if (c->properties != NULL)
		CFRelease(c->properties);
	memset(c, 0, sizeof(*c));
}

void
DAFileSystemCandidatesFree(struct da_probe_candidate *c, size_t count)
{
	size_t i;

	if (c == NULL)
		return;
	for (i = 0; i < count; i++)
		candidate_clear(&c[i]);
	free(c);
}

/* CFString -> malloc'd UTF-8, NULL if `v` is not a string. */
static char *
cf_cstring(CFTypeRef v)
{
	char buf[MAXPATHLEN];

	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return (NULL);
	if (!CFStringGetCString((CFStringRef)v, buf, sizeof(buf),
	    kCFStringEncodingUTF8))
		return (NULL);
	return (strdup(buf));
}

/* Relative helper names live in Contents/Resources (Apple DAFileSystem.c). */
static char *
resolve_exec(const char *bundle, const char *name)
{
	char *path;

	if (name == NULL || name[0] == '\0')
		return (NULL);
	if (name[0] == '/')
		return (strdup(name));
	if (asprintf(&path, "%s/Contents/Resources/%s", bundle, name) < 0)
		return (NULL);
	return (path);
}

/* The first personality's executable under `key`, resolved. */
static char *
personality_exec(const char *bundle, CFDictionaryRef info, CFStringRef key)
{
	CFDictionaryRef pers;
	CFIndex i, n;
	const void **vals;
	char *name, *path = NULL;

	pers = CFDictionaryGetValue(info, CFSTR("FSPersonalities"));
	if (pers == NULL || CFGetTypeID(pers) != CFDictionaryGetTypeID())
		return (NULL);
	n = CFDictionaryGetCount(pers);
	vals = calloc((size_t)n + 1, sizeof(*vals));
	if (vals == NULL)
		return (NULL);
	CFDictionaryGetKeysAndValues(pers, NULL, vals);
	for (i = 0; i < n && path == NULL; i++) {
		if (CFGetTypeID(vals[i]) != CFDictionaryGetTypeID())
			continue;
		name = cf_cstring(CFDictionaryGetValue(vals[i], key));
		path = resolve_exec(bundle, name);
		free(name);
	}
	free(vals);
	return (path);
}

/* Append every FSMediaTypes entry of one bundle to `list`. */
static void
load_bundle(const char *bundle, struct da_probe_candidate **list,
    size_t *count, size_t *cap)
{
	CFDictionaryRef info, types;
	CFArrayRef impl;
	char path[MAXPATHLEN], *kind, *repair, *mount;
	int user_impl = 0;
	const void **vals;
	CFIndex i, n;

	(void)snprintf(path, sizeof(path), "%s/Contents/Info.plist", bundle);
	info = DAPropertyListCreateFromFile(path);
	if (info == NULL) {
		DALog("DA-FS: %s: no readable Contents/Info.plist", bundle);
		return;
	}
	kind = cf_cstring(CFDictionaryGetValue(info, CFSTR("CFBundleName")));
	types = CFDictionaryGetValue(info, CFSTR("FSMediaTypes"));
	if (kind == NULL || types == NULL ||
	    CFGetTypeID(types) != CFDictionaryGetTypeID()) {
		DALog("DA-FS: %s: no CFBundleName or FSMediaTypes", bundle);
		free(kind);
		CFRelease(info);
		return;
	}
	repair = personality_exec(bundle, info, CFSTR("FSRepairExecutable"));
	mount = personality_exec(bundle, info, CFSTR("FSMountExecutable"));
	impl = CFDictionaryGetValue(info, CFSTR("FSImplementation"));
	if (impl != NULL && CFGetTypeID(impl) == CFArrayGetTypeID() &&
	    CFArrayContainsValue(impl, CFRangeMake(0, CFArrayGetCount(impl)),
	    CFSTR("user")) && !CFArrayContainsValue(impl,
	    CFRangeMake(0, CFArrayGetCount(impl)), CFSTR("kernel")))
		user_impl = 1;

	n = CFDictionaryGetCount(types);
	vals = calloc((size_t)n + 1, sizeof(*vals));
	if (vals != NULL)
		CFDictionaryGetKeysAndValues(types, NULL, vals);
	for (i = 0; vals != NULL && i < n; i++) {
		CFDictionaryRef t = vals[i], props;
		CFTypeRef order, adm;
		struct da_probe_candidate *c;
		char *exec, *args;
		int ord = 0;

		if (CFGetTypeID(t) != CFDictionaryGetTypeID())
			continue;
		props = CFDictionaryGetValue(t, CFSTR("FSMediaProperties"));
		exec = cf_cstring(CFDictionaryGetValue(t,
		    CFSTR("FSProbeExecutable")));
		if (props == NULL || CFGetTypeID(props) !=
		    CFDictionaryGetTypeID() || exec == NULL) {
			free(exec);
			continue;
		}
		if (*count == *cap) {
			size_t ncap = *cap != 0 ? *cap * 2 : 16;
			struct da_probe_candidate *nl;

			nl = realloc(*list, ncap * sizeof(**list));
			if (nl == NULL) {
				free(exec);
				break;
			}
			*list = nl;
			*cap = ncap;
		}
		c = &(*list)[(*count)++];
		memset(c, 0, sizeof(*c));
		c->bundle = strdup(bundle);
		c->kind = strdup(kind);
		c->probe_exec = resolve_exec(bundle, exec);
		free(exec);
		args = cf_cstring(CFDictionaryGetValue(t,
		    CFSTR("FSProbeArguments")));
		c->probe_args = args != NULL ? args : strdup("-p");
		c->repair_exec = repair != NULL ? strdup(repair) : NULL;
		c->mount_exec = mount != NULL ? strdup(mount) : NULL;
		c->user_impl = user_impl;
		order = CFDictionaryGetValue(t, CFSTR("FSProbeOrder"));
		if (order != NULL && CFGetTypeID(order) == CFNumberGetTypeID())
			(void)CFNumberGetValue(order, kCFNumberIntType, &ord);
		c->order = ord;
		adm = CFDictionaryGetValue(t, CFSTR("autodiskmount"));
		c->autodiskmount = !(adm != NULL &&
		    CFGetTypeID(adm) == CFBooleanGetTypeID() &&
		    !CFBooleanGetValue(adm));
		c->properties = CFRetain(props);
	}
	free(vals);
	free(repair);
	free(mount);
	free(kind);
	CFRelease(info);
}

static int
candidate_cmp(const void *a, const void *b)
{
	const struct da_probe_candidate *ca = a, *cb = b;
	int r;

	if (ca->order != cb->order)
		return (ca->order < cb->order ? -1 : 1);
	if ((r = strcmp(ca->bundle, cb->bundle)) != 0)
		return (r);
	return (0);
}

void
DAFileSystemListRefresh(void)
{
	struct da_probe_candidate *list = NULL;
	struct timespec mt[nitems(fs_dirs)];
	size_t count = 0, cap = 0, i, nbundles = 0;
	struct stat st;
	struct dirent *de;
	DIR *d;
	int changed = !g_loaded;

	for (i = 0; i < nitems(fs_dirs); i++) {
		memset(&mt[i], 0, sizeof(mt[i]));
		if (stat(fs_dirs[i], &st) == 0)
			mt[i] = st.st_mtim;
		if (mt[i].tv_sec != g_mtime[i].tv_sec ||
		    mt[i].tv_nsec != g_mtime[i].tv_nsec)
			changed = 1;
	}
	if (!changed)
		return;

	for (i = 0; i < nitems(fs_dirs); i++) {
		g_mtime[i] = mt[i];
		d = opendir(fs_dirs[i]);
		if (d == NULL)
			continue;
		while ((de = readdir(d)) != NULL) {
			size_t len = strlen(de->d_name);
			char bundle[MAXPATHLEN];

			if (de->d_name[0] == '.' ||
			    len <= strlen(FS_DIR_SUFFIX) ||
			    strcmp(de->d_name + len - strlen(FS_DIR_SUFFIX),
			    FS_DIR_SUFFIX) != 0)
				continue;
			(void)snprintf(bundle, sizeof(bundle), "%s/%s",
			    fs_dirs[i], de->d_name);
			load_bundle(bundle, &list, &count, &cap);
			nbundles++;
		}
		(void)closedir(d);
	}
	if (count > 1)
		qsort(list, count, sizeof(*list), candidate_cmp);

	DAFileSystemCandidatesFree(g_list, g_count);
	g_list = list;
	g_count = count;
	g_loaded = 1;
	DALog("DA-FS: %zu filesystem bundles, %zu probe candidates", nbundles,
	    count);
}

/*
 * FSMediaProperties keys are IOMedia property names; map the ones the
 * GEOM model carries onto its description keys. An unknown key never
 * matches, as on Darwin where the property would be missing.
 */
static CFStringRef
media_key(CFStringRef prop)
{
	char buf[64];

	if (!CFStringGetCString(prop, buf, sizeof(buf), kCFStringEncodingUTF8))
		return (NULL);
	if (strcmp(buf, "Content Hint") == 0)
		return (kDADiskDescriptionMediaContentKey);
	if (strcmp(buf, "Leaf") == 0)
		return (kDADiskDescriptionMediaLeafKey);
	if (strcmp(buf, "Whole") == 0)
		return (kDADiskDescriptionMediaWholeKey);
	if (strcmp(buf, "Writable") == 0)
		return (kDADiskDescriptionMediaWritableKey);
	if (strcmp(buf, "Removable") == 0)
		return (kDADiskDescriptionMediaRemovableKey);
	if (strcmp(buf, "Ejectable") == 0)
		return (kDADiskDescriptionMediaEjectableKey);
	return (NULL);
}

static int
candidate_matches(const struct da_probe_candidate *c,
    const struct da_disk *disk)
{
	const void **keys, **vals;
	CFIndex i, n;
	int match = 1;

	n = CFDictionaryGetCount(c->properties);
	keys = calloc((size_t)n + 1, sizeof(*keys));
	vals = calloc((size_t)n + 1, sizeof(*vals));
	if (keys == NULL || vals == NULL) {
		free(keys);
		free(vals);
		return (0);
	}
	CFDictionaryGetKeysAndValues(c->properties, keys, vals);
	for (i = 0; i < n && match; i++) {
		CFStringRef key;
		CFTypeRef have;

		if (CFGetTypeID(keys[i]) != CFStringGetTypeID() ||
		    (key = media_key(keys[i])) == NULL) {
			match = 0;
			break;
		}
		have = DADiskGetDescription(disk, key);
		if (have == NULL || !CFEqual(have, vals[i]))
			match = 0;
	}
	free(keys);
	free(vals);
	return (match);
}

static void
candidate_copy(struct da_probe_candidate *o, const struct da_probe_candidate *c)
{
	o->bundle = strdup(c->bundle);
	o->kind = strdup(c->kind);
	o->probe_exec = strdup(c->probe_exec);
	o->probe_args = strdup(c->probe_args);
	o->repair_exec = c->repair_exec != NULL ? strdup(c->repair_exec) : NULL;
	o->mount_exec = c->mount_exec != NULL ? strdup(c->mount_exec) : NULL;
	o->user_impl = c->user_impl;
	o->order = c->order;
	o->autodiskmount = c->autodiskmount;
	o->properties = CFRetain(c->properties);
}

struct da_probe_candidate *
DAFileSystemCandidateCopy(const struct da_probe_candidate *c)
{
	struct da_probe_candidate *o;

	o = calloc(1, sizeof(*o));
	if (o != NULL)
		candidate_copy(o, c);
	return (o);
}

struct da_probe_candidate *
DAFileSystemCopyCandidates(const struct da_disk *disk, size_t *count)
{
	struct da_probe_candidate *out;
	size_t i, n = 0;

	*count = 0;
	out = calloc(g_count + 1, sizeof(*out));
	if (out == NULL)
		return (NULL);
	for (i = 0; i < g_count; i++) {
		const struct da_probe_candidate *c = &g_list[i];

		if (c->probe_exec == NULL || !candidate_matches(c, disk))
			continue;
		candidate_copy(&out[n++], c);
	}
	*count = n;
	return (out);
}
