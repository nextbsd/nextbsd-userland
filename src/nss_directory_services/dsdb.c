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

#include <sys/param.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dsdb.h"
#include "plist.h"

#ifndef nitems
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif
/* The lock is taken and released in separate calls by design; clang's
 * thread-safety analysis cannot follow that through these wrappers. */
#ifndef __no_lock_analysis
#define __no_lock_analysis
#endif
#if defined(__APPLE__) && !defined(st_mtim)
#define st_mtim		st_mtimespec	/* host-side tests only */
#endif

/* A plist bigger than this is not an account database. */
#define DS_MAX_FILE	(16 * 1024 * 1024)

static const char *local_dir = DS_LOCAL_DIR;
static const char *network_dir = DS_NETWORK_DIR;

static pthread_mutex_t dsdb_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * One cached plist. The identity of the loaded file (path, device, inode,
 * modification time, size) is kept so a lookup can tell whether the file
 * on disk is still the one that was parsed; dscli replaces the plist with
 * rename(2), which changes the inode.
 */
struct ds_file {
	const char	*base;
	char		 path[PATH_MAX];
	int		 network;	/* loaded from network_dir */
	int		 loaded;
	dev_t		 dev;
	ino_t		 ino;
	struct timespec	 mtim;
	off_t		 size;
};

static struct ds_file users_file = { .base = DS_USERS_PLIST };
static struct ds_file groups_file = { .base = DS_GROUPS_PLIST };

static struct ds_user *users;
static size_t nusers;
static struct ds_group *groups;
static size_t ngroups;

void __no_lock_analysis
dsdb_lock(void)
{
	(void)pthread_mutex_lock(&dsdb_mutex);
}

void __no_lock_analysis
dsdb_unlock(void)
{
	(void)pthread_mutex_unlock(&dsdb_mutex);
}

/*
 * After fork(2) the child has one thread and inherits the mutex in
 * whatever state it was in; make it usable again.
 */
void __no_lock_analysis
dsdb_atfork_child(void)
{
	pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;

	memcpy(&dsdb_mutex, &fresh, sizeof(dsdb_mutex));
}

#ifdef DSDB_TEST
void
dsdb_set_dirs(const char *local, const char *network)
{
	local_dir = local;
	network_dir = network;
	users_file.loaded = 0;
	groups_file.loaded = 0;
}
#endif

/* ---- record storage ------------------------------------------------------ */

static void
free_users(void)
{
	size_t i;

	for (i = 0; i < nusers; i++) {
		free(users[i].name);
		free(users[i].real);
		free(users[i].shell);
		free(users[i].hash);
	}
	free(users);
	users = NULL;
	nusers = 0;
}

static void
free_groups(void)
{
	size_t i, j;

	for (i = 0; i < ngroups; i++) {
		free(groups[i].name);
		for (j = 0; j < groups[i].nmembers; j++)
			free(groups[i].members[j]);
		free(groups[i].members);
	}
	free(groups);
	groups = NULL;
	ngroups = 0;
}

/*
 * The record's own `username`/`groupname` key wins; the dictionary key it
 * is filed under is the fallback. Empty names are skipped.
 */
static const char *
record_name(const struct pl_node *rec, const char *namekey)
{
	const char *n;

	n = pl_dict_string(rec, namekey);
	if (n == NULL || n[0] == '\0')
		n = rec->key;
	if (n == NULL || n[0] == '\0' || strchr(n, ':') != NULL)
		return (NULL);
	return (n);
}

static int
id_from(const struct pl_node *rec, const char *key, unsigned long long *out)
{
	long long v;

	if (pl_dict_integer(rec, key, &v) == -1 || v < 0 ||
	    (unsigned long long)v > UINT32_MAX)
		return (-1);
	*out = (unsigned long long)v;
	return (0);
}

static int
build_users(const struct pl_node *root)
{
	struct ds_user *list, *u;
	const struct pl_node *rec;
	const char *name, *s;
	unsigned long long id;
	size_t i, n = 0;
	int b;

	if (root->type != PL_DICT)
		return (-1);
	list = calloc(root->nchildren + 1, sizeof(*list));
	if (list == NULL)
		return (-1);
	for (i = 0; i < root->nchildren; i++) {
		rec = root->children[i];
		if (rec->type != PL_DICT)
			continue;
		name = record_name(rec, "username");
		if (name == NULL)
			continue;
		u = &list[n];
		u->name = strdup(name);
		s = pl_dict_string(rec, "realName");
		u->real = strdup(s != NULL ? s : "");
		s = pl_dict_string(rec, "shell");
		u->shell = strdup(s != NULL && s[0] != '\0' ? s : DS_DEFAULT_SHELL);
		s = pl_dict_string(rec, "passwordHash");
		u->hash = s != NULL ? strdup(s) : NULL;
		if (u->name == NULL || u->real == NULL || u->shell == NULL ||
		    (s != NULL && u->hash == NULL))
			goto fail;
		u->uid = id_from(rec, "uid", &id) == 0 ? (uid_t)id : DS_NOBODY_ID;
		u->gid = id_from(rec, "gid", &id) == 0 ? (gid_t)id : DS_NOBODY_ID;
		u->nopass = (pl_dict_bool(rec, "noPassword", &b) == 0 && b);
		n++;
	}
	free_users();
	users = list;
	nusers = n;
	return (0);
fail:
	nusers = n + 1;
	users = list;
	free_users();
	return (-1);
}

static int
build_groups(const struct pl_node *root)
{
	struct ds_group *list, *g;
	const struct pl_node *rec, *mem, *m;
	const char *name;
	unsigned long long id;
	size_t i, j, n = 0;

	if (root->type != PL_DICT)
		return (-1);
	list = calloc(root->nchildren + 1, sizeof(*list));
	if (list == NULL)
		return (-1);
	for (i = 0; i < root->nchildren; i++) {
		rec = root->children[i];
		if (rec->type != PL_DICT)
			continue;
		name = record_name(rec, "groupname");
		if (name == NULL)
			continue;
		g = &list[n];
		g->name = strdup(name);
		if (g->name == NULL)
			goto fail;
		g->gid = id_from(rec, "gid", &id) == 0 ? (gid_t)id : DS_NOBODY_ID;
		mem = pl_dict_get(rec, "members");
		if (mem != NULL && mem->type == PL_ARRAY && mem->nchildren > 0) {
			g->members = calloc(mem->nchildren, sizeof(char *));
			if (g->members == NULL)
				goto fail;
			for (j = 0; j < mem->nchildren; j++) {
				m = mem->children[j];
				if (m->type != PL_STRING || m->str[0] == '\0' ||
				    strchr(m->str, ',') != NULL)
					continue;
				g->members[g->nmembers] = strdup(m->str);
				if (g->members[g->nmembers] == NULL)
					goto fail;
				g->nmembers++;
			}
		}
		n++;
	}
	free_groups();
	groups = list;
	ngroups = n;
	return (0);
fail:
	ngroups = n + 1;
	groups = list;
	free_groups();
	return (-1);
}

/* ---- file selection and cache -------------------------------------------- */

static int
read_file(int fd, off_t size, char **out)
{
	char *buf;
	ssize_t got;
	size_t have = 0;

	if (size < 0 || size > DS_MAX_FILE)
		return (-1);
	buf = malloc((size_t)size + 1);
	if (buf == NULL)
		return (-1);
	while (have < (size_t)size) {
		got = read(fd, buf + have, (size_t)size - have);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		have += (size_t)got;
	}
	buf[have] = '\0';
	*out = buf;
	return ((int)have == size ? 0 : -1);
}

static void
unload(struct ds_file *f)
{
	f->loaded = 0;
	if (f == &users_file)
		free_users();
	else
		free_groups();
}

/*
 * Pick the file (the /Network copy when it exists), and reparse it when it
 * is not the one already cached. Every lookup calls this, so mounting or
 * unmounting /Network switches sources at once.
 */
static enum dsdb_status
refresh(struct ds_file *f)
{
	struct stat st;
	struct pl_node *root;
	char path[PATH_MAX];
	char *buf;
	int fd, network, rv;

	rv = snprintf(path, sizeof(path), "%s/%s", network_dir, f->base);
	if (rv < 0 || (size_t)rv >= sizeof(path))
		goto unavail;
	network = 1;
	if (stat(path, &st) == -1 || !S_ISREG(st.st_mode)) {
		rv = snprintf(path, sizeof(path), "%s/%s", local_dir, f->base);
		if (rv < 0 || (size_t)rv >= sizeof(path))
			goto unavail;
		network = 0;
		if (stat(path, &st) == -1 || !S_ISREG(st.st_mode))
			goto unavail;
	}
	if (f->loaded && strcmp(path, f->path) == 0 && st.st_dev == f->dev &&
	    st.st_ino == f->ino && st.st_size == f->size &&
	    st.st_mtim.tv_sec == f->mtim.tv_sec &&
	    st.st_mtim.tv_nsec == f->mtim.tv_nsec)
		return (DSDB_OK);

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		goto unavail;
	/* Record the identity of what is actually read, not what was stat'ed. */
	if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
	    read_file(fd, st.st_size, &buf) == -1) {
		(void)close(fd);
		goto unavail;
	}
	(void)close(fd);
	root = pl_parse(buf, (size_t)st.st_size);
	free(buf);
	if (root == NULL)
		goto unavail;
	rv = (f == &users_file) ? build_users(root) : build_groups(root);
	pl_free(root);
	if (rv == -1)
		goto unavail;
	strlcpy(f->path, path, sizeof(f->path));
	f->network = network;
	f->dev = st.st_dev;
	f->ino = st.st_ino;
	f->mtim = st.st_mtim;
	f->size = st.st_size;
	f->loaded = 1;
	return (DSDB_OK);
unavail:
	unload(f);
	return (DSDB_UNAVAIL);
}

enum dsdb_status
dsdb_refresh_users(void)
{
	return (refresh(&users_file));
}

enum dsdb_status
dsdb_refresh_groups(void)
{
	return (refresh(&groups_file));
}

int
dsdb_users_from_network(void)
{
	return (users_file.loaded && users_file.network);
}

/* ---- lookups ------------------------------------------------------------- */

const struct ds_user *
dsdb_user_by_name(const char *name)
{
	size_t i;

	for (i = 0; i < nusers; i++)
		if (strcmp(users[i].name, name) == 0)
			return (&users[i]);
	return (NULL);
}

const struct ds_user *
dsdb_user_by_uid(uid_t uid)
{
	size_t i;

	for (i = 0; i < nusers; i++)
		if (users[i].uid == uid)
			return (&users[i]);
	return (NULL);
}

const struct ds_user *
dsdb_user_at(size_t index)
{
	return (index < nusers ? &users[index] : NULL);
}

size_t
dsdb_user_count(void)
{
	return (nusers);
}

const struct ds_group *
dsdb_group_by_name(const char *name)
{
	size_t i;

	for (i = 0; i < ngroups; i++)
		if (strcmp(groups[i].name, name) == 0)
			return (&groups[i]);
	return (NULL);
}

const struct ds_group *
dsdb_group_by_gid(gid_t gid)
{
	size_t i;

	for (i = 0; i < ngroups; i++)
		if (groups[i].gid == gid)
			return (&groups[i]);
	return (NULL);
}

const struct ds_group *
dsdb_group_at(size_t index)
{
	return (index < ngroups ? &groups[index] : NULL);
}

size_t
dsdb_group_count(void)
{
	return (ngroups);
}

/* ---- packing ------------------------------------------------------------- */

/* Copy s into the buffer cursor; NULL when it does not fit. */
static char *
put(char **cur, char *end, const char *s)
{
	size_t n = strlen(s) + 1;
	char *at = *cur;

	if ((size_t)(end - at) < n)
		return (NULL);
	memcpy(at, s, n);
	*cur += n;
	return (at);
}

int
dsdb_pack_passwd(const struct ds_user *u, int privileged, int network,
    struct passwd *pwd, char *buf, size_t buflen)
{
	const char *passwd;
	char *cur = buf, *end = buf + buflen;
	int rv;

	if (privileged)
		passwd = u->nopass ? "" : (u->hash != NULL ? u->hash : "*");
	else
		passwd = "*";

	memset(pwd, 0, sizeof(*pwd));
	if ((pwd->pw_name = put(&cur, end, u->name)) == NULL ||
	    (pwd->pw_passwd = put(&cur, end, passwd)) == NULL ||
	    (pwd->pw_class = put(&cur, end, "")) == NULL ||
	    (pwd->pw_gecos = put(&cur, end, u->real)) == NULL)
		return (ERANGE);
	rv = snprintf(cur, (size_t)(end - cur), "%s/Users/%s",
	    network ? "/Network" : "/Local", u->name);
	if (rv < 0 || (size_t)rv >= (size_t)(end - cur))
		return (ERANGE);
	pwd->pw_dir = cur;
	cur += rv + 1;
	if ((pwd->pw_shell = put(&cur, end, u->shell)) == NULL)
		return (ERANGE);
	pwd->pw_uid = u->uid;
	pwd->pw_gid = u->gid;
	pwd->pw_change = 0;
	pwd->pw_expire = 0;
#ifdef _PWF_NAME
	pwd->pw_fields = _PWF_NAME | _PWF_PASSWD | _PWF_UID | _PWF_GID |
	    _PWF_GECOS | _PWF_DIR | _PWF_SHELL;
#endif
	return (0);
}

int
dsdb_pack_group(const struct ds_group *g, struct group *grp, char *buf,
    size_t buflen)
{
	char **mem;
	char *cur, *end = buf + buflen;
	size_t i, align, need;

	/* The member pointer array goes first, aligned for pointers. */
	align = (size_t)(-(uintptr_t)buf) & (sizeof(char *) - 1);
	need = align + (g->nmembers + 1) * sizeof(char *);
	if (need > buflen)
		return (ERANGE);
	mem = (char **)(void *)(buf + align);
	cur = buf + need;

	memset(grp, 0, sizeof(*grp));
	if ((grp->gr_name = put(&cur, end, g->name)) == NULL ||
	    (grp->gr_passwd = put(&cur, end, "x")) == NULL)
		return (ERANGE);
	for (i = 0; i < g->nmembers; i++)
		if ((mem[i] = put(&cur, end, g->members[i])) == NULL)
			return (ERANGE);
	mem[g->nmembers] = NULL;
	grp->gr_mem = mem;
	grp->gr_gid = g->gid;
	return (0);
}

/* ---- membership ---------------------------------------------------------- */

/*
 * The group set is collected into a local, deduplicated list first, so the
 * count reported to the caller is exact even for a sizing call with
 * maxgrp 0 (getgrouplist(3) callers allocate from that count and retry).
 */
struct gidset {
	gid_t	list[NGROUPS_MAX + 1];
	int	count;
};

static void
add_gid(struct gidset *set, gid_t gid)
{
	int i;

	for (i = 0; i < set->count; i++)
		if (set->list[i] == gid)
			return;
	if (set->count < (int)nitems(set->list))
		set->list[set->count++] = gid;
}

/*
 * Gershwin gives every directory user the hardware-access groups, where
 * the base system defines them. They live in /etc/group, which is read
 * directly here: resolving them through getgrnam(3) would re-enter the
 * name-service switch from inside a name-service method.
 */
static void
add_etc_groups(const char *etcgroup, struct gidset *set)
{
	static const char *const wanted[] = { "audio", "video", "render", "input" };
	FILE *fp;
	char *line = NULL, *p, *name, *gidstr, *ep;
	size_t cap = 0, i;
	ssize_t len;
	unsigned long gid;

	fp = fopen(etcgroup, "re");
	if (fp == NULL)
		return;
	while ((len = getline(&line, &cap, fp)) != -1) {
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		p = line;
		name = strsep(&p, ":");
		(void)strsep(&p, ":");		/* password */
		gidstr = strsep(&p, ":");
		if (name == NULL || gidstr == NULL || gidstr[0] == '\0')
			continue;
		for (i = 0; i < nitems(wanted); i++) {
			if (strcmp(name, wanted[i]) != 0)
				continue;
			errno = 0;
			gid = strtoul(gidstr, &ep, 10);
			if (*ep == '\0' && errno == 0 && gid <= UINT32_MAX)
				add_gid(set, (gid_t)gid);
			break;
		}
	}
	free(line);
	(void)fclose(fp);
}

int
dsdb_membership(const char *name, gid_t agroup, gid_t *groups_out,
    int maxgrp, int *grpcnt, const char *etcgroup)
{
	struct gidset set;
	const struct ds_user *u;
	const struct ds_group *g, *admin;
	size_t i, j;
	int n, is_admin = 0;

	u = dsdb_user_by_name(name);
	if (u == NULL)
		return (-1);
	set.count = 0;
	add_gid(&set, agroup);
	add_gid(&set, u->gid);
	admin = dsdb_group_by_name("admin");
	for (i = 0; i < ngroups; i++) {
		g = &groups[i];
		for (j = 0; j < g->nmembers; j++) {
			if (strcmp(g->members[j], name) != 0)
				continue;
			add_gid(&set, g->gid);
			if (g == admin)
				is_admin = 1;
			break;
		}
	}
	if (is_admin)
		add_gid(&set, 0);			/* wheel */
	if (etcgroup != NULL)
		add_etc_groups(etcgroup, &set);
	n = MIN(set.count, maxgrp);
	if (n > 0)
		memcpy(groups_out, set.list, (size_t)n * sizeof(gid_t));
	*grpcnt = set.count;
	return (0);
}
