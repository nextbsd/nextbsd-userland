/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * libds — the DirectoryServices plists, through CoreFoundation.
 * See libds.h for the two rules callers must know (domains do not fall
 * back on write; a commit writes Groups.plist before Users.plist).
 *
 * Both plists are a dictionary keyed by record name, whose values are
 * dictionaries. The record also carries its own name under `username` or
 * `groupname`; we write both and prefer the inner key on read, which is
 * what nss_directory_services' record_name() does.
 *
 * XML is the only format written. The NSS module's own parser reads XML
 * and nothing else, so a binary plist here would make accounts invisible.
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include "libds.h"

#define DS_MAX_FILE	(16 * 1024 * 1024)

#ifdef LIBDS_TEST
static char	 test_local[PATH_MAX];
static char	 test_network[PATH_MAX];

void
ds_set_dirs(const char *local, const char *network)
{
	if (local != NULL)
		strlcpy(test_local, local, sizeof(test_local));
	if (network != NULL)
		strlcpy(test_network, network, sizeof(test_network));
}
#endif

struct ds_handle {
	char			 dir[PATH_MAX];
	enum ds_mode		 mode;
	int			 lockfd;
	CFMutableDictionaryRef	 users;
	CFMutableDictionaryRef	 groups;
	CFMutableArrayRef	 ukeys;		/* sorted, for *_at() */
	CFMutableArrayRef	 gkeys;
	struct stat		 ust;
	struct stat		 gst;
	bool			 uexisted;
	bool			 gexisted;
	bool			 udirty;
	bool			 gdirty;
};

const char *
ds_strerror(enum ds_error err)
{
	switch (err) {
	case DS_OK:	 return ("no error");
	case DS_ENOENT:	 return ("no such record");
	case DS_EEXIST:	 return ("record already exists");
	case DS_EPARSE:	 return ("file is not a property list dictionary");
	case DS_EIO:	 return ("read or write failed");
	case DS_ELOCK:	 return ("could not lock the directory");
	case DS_EINVAL:	 return ("invalid argument");
	case DS_ERANGE:	 return ("result does not fit");
	}
	return ("unknown error");
}

/* ---- small CoreFoundation helpers ----------------------------------- */

static CFStringRef
mkstr(const char *s)
{
	return (CFStringCreateWithCString(kCFAllocatorDefault, s,
	    kCFStringEncodingUTF8));
}

static CFNumberRef
mknum(long long v)
{
	return (CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &v));
}

/* Copy a CFString into buf. False when absent, not a string, or too long. */
static bool
getstr(CFTypeRef v, char *buf, size_t buflen)
{
	buf[0] = '\0';
	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return (false);
	return (CFStringGetCString((CFStringRef)v, buf, (CFIndex)buflen,
	    kCFStringEncodingUTF8) ? true : false);
}

static bool
getnum(CFTypeRef v, long long *out)
{
	if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID())
		return (false);
	return (CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, out) ?
	    true : false);
}

static bool
getbool(CFTypeRef v)
{
	return (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue((CFBooleanRef)v));
}

static CFMutableDictionaryRef
newdict(void)
{
	return (CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

static void
dict_set_str(CFMutableDictionaryRef d, const char *key, const char *val)
{
	CFStringRef k, v;

	k = mkstr(key);
	v = mkstr(val);
	if (k != NULL && v != NULL)
		CFDictionarySetValue(d, k, v);
	if (k != NULL) CFRelease(k);
	if (v != NULL) CFRelease(v);
}

static void
dict_set_num(CFMutableDictionaryRef d, const char *key, long long val)
{
	CFStringRef k;
	CFNumberRef v;

	k = mkstr(key);
	v = mknum(val);
	if (k != NULL && v != NULL)
		CFDictionarySetValue(d, k, v);
	if (k != NULL) CFRelease(k);
	if (v != NULL) CFRelease(v);
}

static void
dict_set_bool(CFMutableDictionaryRef d, const char *key, bool val)
{
	CFStringRef k;

	if ((k = mkstr(key)) == NULL)
		return;
	CFDictionarySetValue(d, k, val ? kCFBooleanTrue : kCFBooleanFalse);
	CFRelease(k);
}

static void
dict_unset(CFMutableDictionaryRef d, const char *key)
{
	CFStringRef k;

	if ((k = mkstr(key)) == NULL)
		return;
	CFDictionaryRemoveValue(d, k);
	CFRelease(k);
}

static CFTypeRef
dict_get(CFDictionaryRef d, const char *key)
{
	CFStringRef k;
	CFTypeRef v;

	if (d == NULL || (k = mkstr(key)) == NULL)
		return (NULL);
	v = CFDictionaryGetValue(d, k);
	CFRelease(k);
	return (v);
}

/* ---- name validation ------------------------------------------------ */

/*
 * A record name has to survive being written into master.passwd and
 * /etc/group by the other half of the account tools, and has to be a
 * usable plist key, so the rules are the strict intersection: printable,
 * no colon, no newline, no slash, no leading dash, and short enough.
 */
static bool
name_ok(const char *n)
{
	size_t i;

	if (n == NULL || n[0] == '\0' || n[0] == '-' || n[0] == '.')
		return (false);
	if (strlen(n) >= DS_NAME_MAX)
		return (false);
	for (i = 0; n[i] != '\0'; i++) {
		unsigned char c = (unsigned char)n[i];

		if (c <= ' ' || c == 0x7f || c == ':' || c == '/' || c == ',')
			return (false);
	}
	return (true);
}

/* ---- reading and writing a file ------------------------------------- */

static CFMutableDictionaryRef
read_plist(const char *path, struct stat *st, bool *existed, enum ds_error *err)
{
	CFMutableDictionaryRef d = NULL;
	CFDataRef data;
	CFPropertyListRef pl;
	CFErrorRef cferr = NULL;
	CFPropertyListFormat fmt;
	unsigned char *buf = NULL;
	ssize_t n;
	size_t got = 0;
	int fd;

	*err = DS_OK;
	*existed = false;
	memset(st, 0, sizeof(*st));

	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) == -1) {
		if (errno == ENOENT)
			return (newdict());	/* absent reads as empty */
		*err = DS_EIO;
		return (NULL);
	}
	if (fstat(fd, st) == -1 || !S_ISREG(st->st_mode)) {
		close(fd);
		*err = DS_EIO;
		return (NULL);
	}
	*existed = true;
	if (st->st_size == 0) {			/* empty file, not an error */
		close(fd);
		return (newdict());
	}
	if (st->st_size > DS_MAX_FILE) {
		close(fd);
		*err = DS_EPARSE;
		return (NULL);
	}
	if ((buf = malloc((size_t)st->st_size)) == NULL) {
		close(fd);
		*err = DS_EIO;
		return (NULL);
	}
	while (got < (size_t)st->st_size) {
		n = read(fd, buf + got, (size_t)st->st_size - got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	if (got != (size_t)st->st_size) {
		free(buf);
		*err = DS_EIO;
		return (NULL);
	}

	data = CFDataCreate(kCFAllocatorDefault, buf, (CFIndex)got);
	free(buf);
	if (data == NULL) {
		*err = DS_EIO;
		return (NULL);
	}
	pl = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
	    kCFPropertyListMutableContainersAndLeaves, &fmt, &cferr);
	CFRelease(data);
	if (pl == NULL) {
		if (cferr != NULL)
			CFRelease(cferr);
		*err = DS_EPARSE;
		return (NULL);
	}
	if (CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
		CFRelease(pl);
		*err = DS_EPARSE;
		return (NULL);
	}
	d = (CFMutableDictionaryRef)pl;
	return (d);
}

static enum ds_error
write_plist(const char *dir, const char *name, CFDictionaryRef d,
    const struct stat *st, bool existed)
{
	char path[PATH_MAX], tmp[PATH_MAX];
	CFDataRef data;
	CFErrorRef cferr = NULL;
	const UInt8 *p;
	CFIndex len;
	size_t off = 0;
	ssize_t n;
	int fd, dfd;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >=
	    (int)sizeof(path) ||
	    snprintf(tmp, sizeof(tmp), "%s/.%s.XXXXXX", dir, name) >=
	    (int)sizeof(tmp))
		return (DS_EINVAL);

	data = CFPropertyListCreateData(kCFAllocatorDefault, d,
	    kCFPropertyListXMLFormat_v1_0, 0, &cferr);
	if (data == NULL) {
		if (cferr != NULL)
			CFRelease(cferr);
		return (DS_EIO);
	}
	p = CFDataGetBytePtr(data);
	len = CFDataGetLength(data);

	if ((fd = mkstemp(tmp)) == -1) {
		CFRelease(data);
		return (DS_EIO);
	}
	/*
	 * Carry mode and owner over from the file being replaced, so an
	 * admin who tightened it keeps that. A file created for the first
	 * time gets 0644, and its owner from the directory.
	 */
	if (existed) {
		(void)fchmod(fd, st->st_mode & 07777);
		(void)fchown(fd, st->st_uid, st->st_gid);
	} else {
		struct stat ds;

		(void)fchmod(fd, 0644);
		if (stat(dir, &ds) == 0)
			(void)fchown(fd, ds.st_uid, ds.st_gid);
	}

	while (off < (size_t)len) {
		n = write(fd, p + off, (size_t)len - off);
		if (n <= 0)
			goto fail;
		off += (size_t)n;
	}
	if (fsync(fd) == -1)
		goto fail;
	if (close(fd) == -1) {
		fd = -1;
		goto fail;
	}
	CFRelease(data);
	if (rename(tmp, path) == -1) {
		(void)unlink(tmp);
		return (DS_EIO);
	}
	/* Make the rename itself durable. */
	if ((dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) != -1) {
		(void)fsync(dfd);
		(void)close(dfd);
	}
	return (DS_OK);

fail:
	if (fd != -1)
		(void)close(fd);
	(void)unlink(tmp);
	CFRelease(data);
	return (DS_EIO);
}

/* ---- the sorted key index used by *_at() ---------------------------- */

static CFComparisonResult
keycmp(const void *a, const void *b, void *ctx __unused)
{
	return (CFStringCompare((CFStringRef)a, (CFStringRef)b, 0));
}

static void
rebuild_keys(CFDictionaryRef d, CFMutableArrayRef *out)
{
	CFIndex n, i;
	const void **keys;

	if (*out != NULL) {
		CFRelease(*out);
		*out = NULL;
	}
	n = CFDictionaryGetCount(d);
	*out = CFArrayCreateMutable(kCFAllocatorDefault, n,
	    &kCFTypeArrayCallBacks);
	if (*out == NULL || n == 0)
		return;
	if ((keys = calloc((size_t)n, sizeof(*keys))) == NULL)
		return;
	CFDictionaryGetKeysAndValues(d, keys, NULL);
	for (i = 0; i < n; i++)
		if (CFGetTypeID(keys[i]) == CFStringGetTypeID())
			CFArrayAppendValue(*out, keys[i]);
	free(keys);
	CFArraySortValues(*out, CFRangeMake(0, CFArrayGetCount(*out)),
	    keycmp, NULL);
}

static CFMutableArrayRef
ukeys_of(struct ds_handle *h)
{
	if (h->ukeys == NULL ||
	    CFArrayGetCount(h->ukeys) != CFDictionaryGetCount(h->users))
		rebuild_keys(h->users, &h->ukeys);
	return (h->ukeys);
}

static CFMutableArrayRef
gkeys_of(struct ds_handle *h)
{
	if (h->gkeys == NULL ||
	    CFArrayGetCount(h->gkeys) != CFDictionaryGetCount(h->groups))
		rebuild_keys(h->groups, &h->gkeys);
	return (h->gkeys);
}

/* ---- open and close ------------------------------------------------- */

static const char *
domain_dir(enum ds_domain d)
{
#ifdef LIBDS_TEST
	if (d == DS_LOCAL && test_local[0] != '\0')
		return (test_local);
	if (d == DS_NETWORK && test_network[0] != '\0')
		return (test_network);
#endif
	return (d == DS_NETWORK ? DS_NETWORK_DIR : DS_LOCAL_DIR);
}

enum ds_error
ds_open(enum ds_domain domain, enum ds_mode mode, struct ds_handle **out)
{
	struct ds_handle *h;
	char path[PATH_MAX];
	enum ds_error err;

	if (out == NULL)
		return (DS_EINVAL);
	*out = NULL;
	if ((h = calloc(1, sizeof(*h))) == NULL)
		return (DS_EIO);
	h->lockfd = -1;
	h->mode = mode;
	if (strlcpy(h->dir, domain_dir(domain), sizeof(h->dir)) >=
	    sizeof(h->dir)) {
		free(h);
		return (DS_EINVAL);
	}

	/*
	 * Take the lock before reading, so the snapshot cannot change
	 * underneath a read-modify-write. Reads need no lock: each file is
	 * replaced by rename, so a reader sees one whole version or the
	 * other.
	 */
	if (mode == DS_RDWR) {
		if (snprintf(path, sizeof(path), "%s/%s", h->dir,
		    DS_LOCK_FILE) >= (int)sizeof(path)) {
			free(h);
			return (DS_EINVAL);
		}
		h->lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (h->lockfd == -1) {
			free(h);
			return (DS_ELOCK);
		}
		if (flock(h->lockfd, LOCK_EX) == -1) {
			(void)close(h->lockfd);
			free(h);
			return (DS_ELOCK);
		}
	}

	(void)snprintf(path, sizeof(path), "%s/%s", h->dir, DS_USERS_PLIST);
	h->users = read_plist(path, &h->ust, &h->uexisted, &err);
	if (h->users == NULL)
		goto fail;
	(void)snprintf(path, sizeof(path), "%s/%s", h->dir, DS_GROUPS_PLIST);
	h->groups = read_plist(path, &h->gst, &h->gexisted, &err);
	if (h->groups == NULL)
		goto fail;

	*out = h;
	return (DS_OK);

fail:
	ds_close(h);
	return (err == DS_OK ? DS_EIO : err);
}

void
ds_close(struct ds_handle *h)
{
	if (h == NULL)
		return;
	if (h->users != NULL) CFRelease(h->users);
	if (h->groups != NULL) CFRelease(h->groups);
	if (h->ukeys != NULL) CFRelease(h->ukeys);
	if (h->gkeys != NULL) CFRelease(h->gkeys);
	if (h->lockfd != -1)
		(void)close(h->lockfd);	/* releases the flock */
	free(h);
}

const char *
ds_dir(const struct ds_handle *h)
{
	return (h == NULL ? "" : h->dir);
}

bool
ds_dirty(const struct ds_handle *h)
{
	return (h != NULL && (h->udirty || h->gdirty));
}

/* ---- users ---------------------------------------------------------- */

/* The record dictionary for a name, or NULL. */
static CFMutableDictionaryRef
urec(struct ds_handle *h, const char *name)
{
	CFTypeRef v = dict_get(h->users, name);

	if (v == NULL || CFGetTypeID(v) != CFDictionaryGetTypeID())
		return (NULL);
	return ((CFMutableDictionaryRef)v);
}

static CFMutableDictionaryRef
grec(struct ds_handle *h, const char *name)
{
	CFTypeRef v = dict_get(h->groups, name);

	if (v == NULL || CFGetTypeID(v) != CFDictionaryGetTypeID())
		return (NULL);
	return ((CFMutableDictionaryRef)v);
}

static void
fill_user(const char *key, CFDictionaryRef rec, struct ds_userrec *out)
{
	long long v;

	memset(out, 0, sizeof(*out));
	/* Prefer the inner name, fall back to the dictionary key. */
	if (!getstr(dict_get(rec, "username"), out->username,
	    sizeof(out->username)) || out->username[0] == '\0')
		(void)strlcpy(out->username, key, sizeof(out->username));
	(void)getstr(dict_get(rec, "realName"), out->realName,
	    sizeof(out->realName));
	(void)getstr(dict_get(rec, "shell"), out->shell, sizeof(out->shell));
	out->hasHash = getstr(dict_get(rec, "passwordHash"),
	    out->passwordHash, sizeof(out->passwordHash)) &&
	    out->passwordHash[0] != '\0';
	out->uid = getnum(dict_get(rec, "uid"), &v) ? (uid_t)v : (uid_t)-1;
	out->gid = getnum(dict_get(rec, "gid"), &v) ? (gid_t)v : (gid_t)-1;
	out->noPassword = getbool(dict_get(rec, "noPassword"));
}

enum ds_error
ds_user_get(struct ds_handle *h, const char *name, struct ds_userrec *out)
{
	CFDictionaryRef rec;

	if (h == NULL || name == NULL || out == NULL)
		return (DS_EINVAL);
	if ((rec = urec(h, name)) == NULL)
		return (DS_ENOENT);
	fill_user(name, rec, out);
	return (DS_OK);
}

size_t
ds_user_count(struct ds_handle *h)
{
	if (h == NULL)
		return (0);
	return ((size_t)CFArrayGetCount(ukeys_of(h)));
}

enum ds_error
ds_user_at(struct ds_handle *h, size_t i, struct ds_userrec *out)
{
	CFArrayRef keys;
	CFStringRef k;
	char name[DS_NAME_MAX];
	CFDictionaryRef rec;

	if (h == NULL || out == NULL)
		return (DS_EINVAL);
	keys = ukeys_of(h);
	if (keys == NULL || (CFIndex)i >= CFArrayGetCount(keys))
		return (DS_ENOENT);
	k = (CFStringRef)CFArrayGetValueAtIndex(keys, (CFIndex)i);
	if (!getstr(k, name, sizeof(name)))
		return (DS_EPARSE);
	if ((rec = urec(h, name)) == NULL)
		return (DS_ENOENT);
	fill_user(name, rec, out);
	return (DS_OK);
}

enum ds_error
ds_user_get_by_uid(struct ds_handle *h, uid_t uid, struct ds_userrec *out)
{
	size_t i, n;

	if (h == NULL || out == NULL)
		return (DS_EINVAL);
	n = ds_user_count(h);
	for (i = 0; i < n; i++)
		if (ds_user_at(h, i, out) == DS_OK && out->uid == uid)
			return (DS_OK);
	return (DS_ENOENT);
}

/* Build the record dictionary for a user. */
static enum ds_error
put_user(struct ds_handle *h, const struct ds_userrec *u, bool replace)
{
	CFMutableDictionaryRef rec;
	CFStringRef key;

	if (!name_ok(u->username))
		return (DS_EINVAL);
	if (strlen(u->realName) >= DS_REAL_MAX ||
	    strlen(u->shell) >= DS_SHELL_MAX ||
	    strlen(u->passwordHash) >= DS_HASH_MAX)
		return (DS_EINVAL);

	rec = newdict();
	if (rec == NULL)
		return (DS_EIO);
	dict_set_str(rec, "username", u->username);
	dict_set_num(rec, "uid", (long long)u->uid);
	dict_set_num(rec, "gid", (long long)u->gid);
	if (u->realName[0] != '\0')
		dict_set_str(rec, "realName", u->realName);
	if (u->shell[0] != '\0')
		dict_set_str(rec, "shell", u->shell);
	if (u->hasHash && u->passwordHash[0] != '\0')
		dict_set_str(rec, "passwordHash", u->passwordHash);
	if (u->noPassword)
		dict_set_bool(rec, "noPassword", true);

	if ((key = mkstr(u->username)) == NULL) {
		CFRelease(rec);
		return (DS_EIO);
	}
	CFDictionarySetValue(h->users, key, rec);
	CFRelease(key);
	CFRelease(rec);
	h->udirty = true;
	if (!replace)
		rebuild_keys(h->users, &h->ukeys);
	return (DS_OK);
}

enum ds_error
ds_user_add(struct ds_handle *h, const struct ds_userrec *u)
{
	if (h == NULL || u == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);
	if (urec(h, u->username) != NULL)
		return (DS_EEXIST);
	return (put_user(h, u, false));
}

enum ds_error
ds_user_set(struct ds_handle *h, const struct ds_userrec *u)
{
	if (h == NULL || u == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);
	if (urec(h, u->username) == NULL)
		return (DS_ENOENT);
	return (put_user(h, u, true));
}

enum ds_error
ds_user_del(struct ds_handle *h, const char *name)
{
	if (h == NULL || name == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);
	if (urec(h, name) == NULL)
		return (DS_ENOENT);
	dict_unset(h->users, name);
	h->udirty = true;
	rebuild_keys(h->users, &h->ukeys);
	return (DS_OK);
}

enum ds_error
ds_user_next_uid(struct ds_handle *h, uid_t from, uid_t to, bool skip_pwd,
    uid_t *out)
{
	struct ds_userrec u;
	uid_t id;
	size_t i, n;
	bool taken;

	if (h == NULL || out == NULL || from > to)
		return (DS_EINVAL);
	n = ds_user_count(h);
	for (id = from;; id++) {
		taken = false;
		for (i = 0; i < n; i++)
			if (ds_user_at(h, i, &u) == DS_OK && u.uid == id) {
				taken = true;
				break;
			}
		if (!taken && skip_pwd && getpwuid(id) != NULL)
			taken = true;
		if (!taken) {
			*out = id;
			return (DS_OK);
		}
		if (id == to)
			break;
	}
	return (DS_ENOENT);
}

/* ---- groups --------------------------------------------------------- */

static void
fill_group(const char *key, CFDictionaryRef rec, struct ds_grouprec *out)
{
	long long v;

	memset(out, 0, sizeof(*out));
	if (!getstr(dict_get(rec, "groupname"), out->groupname,
	    sizeof(out->groupname)) || out->groupname[0] == '\0')
		(void)strlcpy(out->groupname, key, sizeof(out->groupname));
	out->gid = getnum(dict_get(rec, "gid"), &v) ? (gid_t)v : (gid_t)-1;
}

enum ds_error
ds_group_get(struct ds_handle *h, const char *name, struct ds_grouprec *out)
{
	CFDictionaryRef rec;

	if (h == NULL || name == NULL || out == NULL)
		return (DS_EINVAL);
	if ((rec = grec(h, name)) == NULL)
		return (DS_ENOENT);
	fill_group(name, rec, out);
	return (DS_OK);
}

size_t
ds_group_count(struct ds_handle *h)
{
	if (h == NULL)
		return (0);
	return ((size_t)CFArrayGetCount(gkeys_of(h)));
}

enum ds_error
ds_group_at(struct ds_handle *h, size_t i, struct ds_grouprec *out)
{
	CFArrayRef keys;
	CFStringRef k;
	char name[DS_NAME_MAX];
	CFDictionaryRef rec;

	if (h == NULL || out == NULL)
		return (DS_EINVAL);
	keys = gkeys_of(h);
	if (keys == NULL || (CFIndex)i >= CFArrayGetCount(keys))
		return (DS_ENOENT);
	k = (CFStringRef)CFArrayGetValueAtIndex(keys, (CFIndex)i);
	if (!getstr(k, name, sizeof(name)))
		return (DS_EPARSE);
	if ((rec = grec(h, name)) == NULL)
		return (DS_ENOENT);
	fill_group(name, rec, out);
	return (DS_OK);
}

enum ds_error
ds_group_get_by_gid(struct ds_handle *h, gid_t gid, struct ds_grouprec *out)
{
	size_t i, n;

	if (h == NULL || out == NULL)
		return (DS_EINVAL);
	n = ds_group_count(h);
	for (i = 0; i < n; i++)
		if (ds_group_at(h, i, out) == DS_OK && out->gid == gid)
			return (DS_OK);
	return (DS_ENOENT);
}

enum ds_error
ds_group_add(struct ds_handle *h, const struct ds_grouprec *g)
{
	CFMutableDictionaryRef rec;
	CFMutableArrayRef members;
	CFStringRef key, mk;

	if (h == NULL || g == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);
	if (!name_ok(g->groupname))
		return (DS_EINVAL);
	if (grec(h, g->groupname) != NULL)
		return (DS_EEXIST);

	if ((rec = newdict()) == NULL)
		return (DS_EIO);
	dict_set_str(rec, "groupname", g->groupname);
	dict_set_num(rec, "gid", (long long)g->gid);
	members = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	if (members != NULL) {
		if ((mk = mkstr("members")) != NULL) {
			CFDictionarySetValue(rec, mk, members);
			CFRelease(mk);
		}
		CFRelease(members);
	}
	if ((key = mkstr(g->groupname)) == NULL) {
		CFRelease(rec);
		return (DS_EIO);
	}
	CFDictionarySetValue(h->groups, key, rec);
	CFRelease(key);
	CFRelease(rec);
	h->gdirty = true;
	rebuild_keys(h->groups, &h->gkeys);
	return (DS_OK);
}

enum ds_error
ds_group_del(struct ds_handle *h, const char *name)
{
	if (h == NULL || name == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);
	if (grec(h, name) == NULL)
		return (DS_ENOENT);
	dict_unset(h->groups, name);
	h->gdirty = true;
	rebuild_keys(h->groups, &h->gkeys);
	return (DS_OK);
}

/* The members array of a group, creating it when absent. */
static CFMutableArrayRef
members_of(struct ds_handle *h, const char *group, bool create)
{
	CFMutableDictionaryRef rec;
	CFTypeRef v;
	CFMutableArrayRef a;
	CFStringRef mk;

	if ((rec = grec(h, group)) == NULL)
		return (NULL);
	v = dict_get(rec, "members");
	if (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID())
		return ((CFMutableArrayRef)v);
	if (!create)
		return (NULL);
	a = CFArrayCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeArrayCallBacks);
	if (a == NULL)
		return (NULL);
	if ((mk = mkstr("members")) != NULL) {
		CFDictionarySetValue(rec, mk, a);
		CFRelease(mk);
	}
	CFRelease(a);
	v = dict_get(rec, "members");
	return (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID() ?
	    (CFMutableArrayRef)v : NULL);
}

static CFIndex
member_index(CFArrayRef a, const char *user)
{
	CFIndex i, n;
	char buf[DS_NAME_MAX];

	if (a == NULL)
		return (-1);
	n = CFArrayGetCount(a);
	for (i = 0; i < n; i++)
		if (getstr(CFArrayGetValueAtIndex(a, i), buf, sizeof(buf)) &&
		    strcmp(buf, user) == 0)
			return (i);
	return (-1);
}

size_t
ds_group_member_count(struct ds_handle *h, const char *group)
{
	CFArrayRef a;

	if (h == NULL || group == NULL)
		return (0);
	a = members_of(h, group, false);
	return (a == NULL ? 0 : (size_t)CFArrayGetCount(a));
}

enum ds_error
ds_group_member_at(struct ds_handle *h, const char *group, size_t i,
    char *buf, size_t buflen)
{
	CFArrayRef a;

	if (h == NULL || group == NULL || buf == NULL || buflen == 0)
		return (DS_EINVAL);
	if (grec(h, group) == NULL)
		return (DS_ENOENT);
	a = members_of(h, group, false);
	if (a == NULL || (CFIndex)i >= CFArrayGetCount(a))
		return (DS_ENOENT);
	if (!getstr(CFArrayGetValueAtIndex(a, (CFIndex)i), buf, buflen))
		return (DS_ERANGE);
	return (DS_OK);
}

enum ds_error
ds_group_has_member(struct ds_handle *h, const char *group, const char *user,
    bool *out)
{
	if (h == NULL || group == NULL || user == NULL || out == NULL)
		return (DS_EINVAL);
	if (grec(h, group) == NULL)
		return (DS_ENOENT);
	*out = member_index(members_of(h, group, false), user) >= 0;
	return (DS_OK);
}

enum ds_error
ds_group_add_member(struct ds_handle *h, const char *group, const char *user)
{
	CFMutableArrayRef a;
	CFStringRef s;

	if (h == NULL || group == NULL || user == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);
	if (!name_ok(user))
		return (DS_EINVAL);
	if (grec(h, group) == NULL)
		return (DS_ENOENT);
	if ((a = members_of(h, group, true)) == NULL)
		return (DS_EIO);
	if (member_index(a, user) >= 0)
		return (DS_OK);		/* already a member: nothing to do */
	if ((s = mkstr(user)) == NULL)
		return (DS_EIO);
	CFArrayAppendValue(a, s);
	CFRelease(s);
	h->gdirty = true;
	return (DS_OK);
}

enum ds_error
ds_group_del_member(struct ds_handle *h, const char *group, const char *user)
{
	CFMutableArrayRef a;
	CFIndex idx;

	if (h == NULL || group == NULL || user == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);
	if (grec(h, group) == NULL)
		return (DS_ENOENT);
	a = members_of(h, group, false);
	if ((idx = member_index(a, user)) < 0)
		return (DS_ENOENT);
	CFArrayRemoveValueAtIndex(a, idx);
	h->gdirty = true;
	return (DS_OK);
}

/* ---- commit --------------------------------------------------------- */

enum ds_error
ds_commit(struct ds_handle *h)
{
	enum ds_error err;

	if (h == NULL)
		return (DS_EINVAL);
	if (h->mode != DS_RDWR)
		return (DS_EINVAL);

	/*
	 * Groups first, always. See libds.h: on an add the membership
	 * lands before the user exists, which is inert; on a removal
	 * privilege is dropped before the identity. Either way an
	 * interrupted commit never leaves a privilege half-granted.
	 */
	if (h->gdirty) {
		err = write_plist(h->dir, DS_GROUPS_PLIST, h->groups,
		    &h->gst, h->gexisted);
		if (err != DS_OK)
			return (err);
		h->gdirty = false;
		h->gexisted = true;
	}
	if (h->udirty) {
		err = write_plist(h->dir, DS_USERS_PLIST, h->users,
		    &h->ust, h->uexisted);
		if (err != DS_OK)
			return (err);
		h->udirty = false;
		h->uexisted = true;
	}
	return (DS_OK);
}
