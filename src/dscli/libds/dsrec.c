/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
 * Ported from Gershwin's dscli by Joe Maloney
 * (gershwin-desktop/gershwin-components, DirectoryServices/dscli, BSD-2-Clause).
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
 * Records: the user and group model, reading and writing the plists in
 * Gershwin's format (a dictionary keyed by name; keys username, uid, gid,
 * realName, shell, passwordHash, noPassword; groupname, gid, members).
 */

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds.h"
#include "plist.h"

/* ---- directories --------------------------------------------------------- */

static const char *local_dir = DS_LOCAL_DIR;
static const char *network_dir = DS_NETWORK_DIR;
static const char *system_root = "";

void
ds_set_dirs(const char *local, const char *network)
{
	local_dir = local;
	network_dir = network;
}

void
ds_set_system_root(const char *root)
{
	system_root = root != NULL ? root : "";
}

const char *
ds_local_dir(void)
{
	return (local_dir);
}

const char *
ds_network_dir(void)
{
	return (network_dir);
}

int
ds_path(char *buf, size_t len, const char *dir, const char *file)
{
	int rv;

	rv = snprintf(buf, len, "%s/%s", dir, file);
	if (rv < 0 || (size_t)rv >= len) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	return (0);
}

const char *
ds_syspath(char *buf, size_t len, const char *path)
{
	(void)snprintf(buf, len, "%s%s", system_root, path);
	return (buf);
}

bool
ds_from_network(void)
{
	struct stat st;
	char path[PATH_MAX];

	if (ds_path(path, sizeof(path), network_dir, DS_USERS_PLIST) == -1)
		return (false);
	return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

const char *
ds_dir(void)
{
	return (ds_from_network() ? network_dir : local_dir);
}

/* ---- small helpers ------------------------------------------------------- */

int
ds_set_string(char **field, const char *value)
{
	char *copy;

	copy = strdup(value != NULL ? value : "");
	if (copy == NULL)
		return (-1);
	free(*field);
	*field = copy;
	return (0);
}

bool
ds_valid_name(const char *name)
{
	size_t i, n;

	if (name == NULL)
		return (false);
	n = strlen(name);
	if (n == 0 || n > 32 || name[0] == '-')
		return (false);
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];
		if (!isalnum(c) && c != '.' && c != '_' && c != '-')
			return (false);
	}
	return (true);
}

char *
ds_xml_escape(const char *s)
{
	size_t n = 0, i;
	char *out, *p;

	for (i = 0; s[i] != '\0'; i++) {
		switch (s[i]) {
		case '&': n += 5; break;
		case '<': case '>': n += 4; break;
		default: n++;
		}
	}
	out = malloc(n + 1);
	if (out == NULL)
		return (NULL);
	for (i = 0, p = out; s[i] != '\0'; i++) {
		switch (s[i]) {
		case '&': memcpy(p, "&amp;", 5); p += 5; break;
		case '<': memcpy(p, "&lt;", 4); p += 4; break;
		case '>': memcpy(p, "&gt;", 4); p += 4; break;
		default: *p++ = s[i];
		}
	}
	*p = '\0';
	return (out);
}

/* A growable output buffer for the plist writers. */
struct out {
	char	*buf;
	size_t	 len, cap;
	int	 err;
};

static void
out_add(struct out *o, const char *s)
{
	size_t n = strlen(s);
	char *nb;

	if (o->err)
		return;
	if (o->len + n + 1 > o->cap) {
		size_t cap = o->cap ? o->cap * 2 : 1024;
		while (cap < o->len + n + 1)
			cap *= 2;
		nb = realloc(o->buf, cap);
		if (nb == NULL) {
			o->err = 1;
			return;
		}
		o->buf = nb;
		o->cap = cap;
	}
	memcpy(o->buf + o->len, s, n + 1);
	o->len += n;
}

static void
out_escaped(struct out *o, const char *s)
{
	char *e = ds_xml_escape(s);

	if (e == NULL) {
		o->err = 1;
		return;
	}
	out_add(o, e);
	free(e);
}

static void
out_key_string(struct out *o, const char *indent, const char *key, const char *val)
{
	out_add(o, indent); out_add(o, "<key>"); out_add(o, key); out_add(o, "</key>\n");
	out_add(o, indent); out_add(o, "<string>"); out_escaped(o, val); out_add(o, "</string>\n");
}

static void
out_key_integer(struct out *o, const char *indent, const char *key, long long val)
{
	char num[32];

	snprintf(num, sizeof(num), "%lld", val);
	out_add(o, indent); out_add(o, "<key>"); out_add(o, key); out_add(o, "</key>\n");
	out_add(o, indent); out_add(o, "<integer>"); out_add(o, num); out_add(o, "</integer>\n");
}

static const char plist_head[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n<dict>\n";
static const char plist_tail[] = "</dict>\n</plist>\n";

/* ---- users --------------------------------------------------------------- */

static void
user_clear(struct ds_user *u)
{
	free(u->name);
	free(u->real);
	free(u->shell);
	free(u->hash);
	memset(u, 0, sizeof(*u));
}

void
ds_users_free(struct ds_users *us)
{
	size_t i;

	for (i = 0; i < us->n; i++)
		user_clear(&us->v[i]);
	free(us->v);
	us->v = NULL;
	us->n = 0;
}

static int
id_from(const struct pl_node *rec, const char *key, long long *out)
{
	long long v;

	if (pl_dict_integer(rec, key, &v) == -1 || v < 0 || v > UINT32_MAX)
		return (-1);
	*out = v;
	return (0);
}

static const char *
record_name(const struct pl_node *rec, const char *namekey)
{
	const char *n;

	n = pl_dict_string(rec, namekey);
	if (n == NULL || n[0] == '\0')
		n = rec->key;
	return (ds_valid_name(n) ? n : NULL);
}

static int
load_root(const char *path, struct pl_node **root)
{
	char *buf;
	size_t len;

	*root = NULL;
	buf = ds_read_file(path, &len);
	if (buf == NULL)
		return (errno == ENOENT ? 0 : -1);
	*root = pl_parse(buf, len);
	free(buf);
	if (*root == NULL || (*root)->type != PL_DICT) {
		pl_free(*root);
		*root = NULL;
		errno = EINVAL;
		return (-1);
	}
	return (0);
}

int
ds_users_load(struct ds_users *us, const char *path)
{
	struct pl_node *root;
	const struct pl_node *rec;
	struct ds_user *u;
	const char *name, *s;
	long long id;
	size_t i;
	int b, saved;

	us->v = NULL;
	us->n = 0;
	if (load_root(path, &root) == -1)
		return (-1);
	if (root == NULL)
		return (0);	/* missing: empty, errno ENOENT */
	saved = errno;
	for (i = 0; i < root->nchildren; i++) {
		rec = root->children[i];
		if (rec->type != PL_DICT)
			continue;
		name = record_name(rec, "username");
		if (name == NULL || ds_user_find(us, name) != NULL)
			continue;
		u = ds_user_add(us, name);
		if (u == NULL)
			goto fail;
		s = pl_dict_string(rec, "realName");
		if (s != NULL && ds_set_string(&u->real, s) == -1)
			goto fail;
		s = pl_dict_string(rec, "shell");
		if (s != NULL && s[0] != '\0' && ds_set_string(&u->shell, s) == -1)
			goto fail;
		s = pl_dict_string(rec, "passwordHash");
		if (s != NULL && s[0] != '\0') {
			u->hash = strdup(s);
			if (u->hash == NULL)
				goto fail;
		}
		u->uid = id_from(rec, "uid", &id) == 0 ? (uid_t)id : 0;
		u->gid = id_from(rec, "gid", &id) == 0 ? (gid_t)id : 0;
		u->nopass = (pl_dict_bool(rec, "noPassword", &b) == 0 && b);
	}
	pl_free(root);
	errno = saved;
	return (0);
fail:
	saved = errno;
	pl_free(root);
	ds_users_free(us);
	errno = saved;
	return (-1);
}

static int
cmp_users(const void *a, const void *b)
{
	return (strcmp(((const struct ds_user *)a)->name,
	    ((const struct ds_user *)b)->name));
}

int
ds_users_save(const struct ds_users *us, const char *path)
{
	struct out o = { NULL, 0, 0, 0 };
	struct ds_user *sorted;
	size_t i;
	int rv;

	sorted = malloc(us->n * sizeof(*sorted) + 1);
	if (sorted == NULL)
		return (-1);
	memcpy(sorted, us->v, us->n * sizeof(*sorted));
	qsort(sorted, us->n, sizeof(*sorted), cmp_users);

	out_add(&o, plist_head);
	for (i = 0; i < us->n; i++) {
		const struct ds_user *u = &sorted[i];
		out_add(&o, "\t<key>"); out_escaped(&o, u->name); out_add(&o, "</key>\n");
		out_add(&o, "\t<dict>\n");
		out_key_integer(&o, "\t\t", "gid", u->gid);
		if (u->nopass) {
			out_add(&o, "\t\t<key>noPassword</key>\n");
			out_add(&o, "\t\t<true/>\n");
		}
		if (u->hash != NULL)
			out_key_string(&o, "\t\t", "passwordHash", u->hash);
		if (u->real != NULL && u->real[0] != '\0')
			out_key_string(&o, "\t\t", "realName", u->real);
		out_key_string(&o, "\t\t", "shell", u->shell);
		out_key_integer(&o, "\t\t", "uid", u->uid);
		out_key_string(&o, "\t\t", "username", u->name);
		out_add(&o, "\t</dict>\n");
	}
	out_add(&o, plist_tail);
	free(sorted);
	if (o.err) {
		free(o.buf);
		errno = ENOMEM;
		return (-1);
	}
	rv = ds_write_atomic(path, o.buf, o.len, 0644);
	free(o.buf);
	return (rv);
}

struct ds_user *
ds_user_find(const struct ds_users *us, const char *name)
{
	size_t i;

	for (i = 0; i < us->n; i++)
		if (strcmp(us->v[i].name, name) == 0)
			return (&us->v[i]);
	return (NULL);
}

struct ds_user *
ds_user_find_uid(const struct ds_users *us, uid_t uid)
{
	size_t i;

	for (i = 0; i < us->n; i++)
		if (us->v[i].uid == uid)
			return (&us->v[i]);
	return (NULL);
}

struct ds_user *
ds_user_add(struct ds_users *us, const char *name)
{
	struct ds_user *nv, *u;

	nv = realloc(us->v, (us->n + 1) * sizeof(*nv));
	if (nv == NULL)
		return (NULL);
	us->v = nv;
	u = &us->v[us->n];
	memset(u, 0, sizeof(*u));
	u->name = strdup(name);
	u->real = strdup("");
	u->shell = strdup(DS_DEFAULT_SHELL);
	if (u->name == NULL || u->real == NULL || u->shell == NULL) {
		user_clear(u);
		return (NULL);
	}
	us->n++;
	return (u);
}

int
ds_user_remove(struct ds_users *us, const char *name)
{
	struct ds_user *u;
	size_t idx;

	u = ds_user_find(us, name);
	if (u == NULL) {
		errno = ENOENT;
		return (-1);
	}
	idx = (size_t)(u - us->v);
	user_clear(u);
	memmove(&us->v[idx], &us->v[idx + 1], (us->n - idx - 1) * sizeof(*u));
	us->n--;
	return (0);
}

/* ---- groups -------------------------------------------------------------- */

static void
group_clear(struct ds_group *g)
{
	size_t i;

	free(g->name);
	for (i = 0; i < g->nmembers; i++)
		free(g->members[i]);
	free(g->members);
	memset(g, 0, sizeof(*g));
}

void
ds_groups_free(struct ds_groups *gs)
{
	size_t i;

	for (i = 0; i < gs->n; i++)
		group_clear(&gs->v[i]);
	free(gs->v);
	gs->v = NULL;
	gs->n = 0;
}

int
ds_groups_load(struct ds_groups *gs, const char *path)
{
	struct pl_node *root;
	const struct pl_node *rec, *mem, *m;
	struct ds_group *g;
	const char *name;
	long long id;
	size_t i, j;
	int saved;

	gs->v = NULL;
	gs->n = 0;
	if (load_root(path, &root) == -1)
		return (-1);
	if (root == NULL)
		return (0);
	saved = errno;
	for (i = 0; i < root->nchildren; i++) {
		rec = root->children[i];
		if (rec->type != PL_DICT)
			continue;
		name = record_name(rec, "groupname");
		if (name == NULL || ds_group_find(gs, name) != NULL)
			continue;
		g = ds_group_add(gs, name,
		    id_from(rec, "gid", &id) == 0 ? (gid_t)id : 0);
		if (g == NULL)
			goto fail;
		mem = pl_dict_get(rec, "members");
		if (mem == NULL || mem->type != PL_ARRAY)
			continue;
		for (j = 0; j < mem->nchildren; j++) {
			m = mem->children[j];
			if (m->type != PL_STRING || !ds_valid_name(m->str))
				continue;
			if (ds_group_add_member(g, m->str) == -1)
				goto fail;
		}
	}
	pl_free(root);
	errno = saved;
	return (0);
fail:
	saved = errno;
	pl_free(root);
	ds_groups_free(gs);
	errno = saved;
	return (-1);
}

static int
cmp_groups(const void *a, const void *b)
{
	return (strcmp(((const struct ds_group *)a)->name,
	    ((const struct ds_group *)b)->name));
}

int
ds_groups_save(const struct ds_groups *gs, const char *path)
{
	struct out o = { NULL, 0, 0, 0 };
	struct ds_group *sorted;
	size_t i, j;
	int rv;

	sorted = malloc(gs->n * sizeof(*sorted) + 1);
	if (sorted == NULL)
		return (-1);
	memcpy(sorted, gs->v, gs->n * sizeof(*sorted));
	qsort(sorted, gs->n, sizeof(*sorted), cmp_groups);

	out_add(&o, plist_head);
	for (i = 0; i < gs->n; i++) {
		const struct ds_group *g = &sorted[i];
		out_add(&o, "\t<key>"); out_escaped(&o, g->name); out_add(&o, "</key>\n");
		out_add(&o, "\t<dict>\n");
		out_key_integer(&o, "\t\t", "gid", g->gid);
		out_key_string(&o, "\t\t", "groupname", g->name);
		out_add(&o, "\t\t<key>members</key>\n");
		if (g->nmembers == 0)
			out_add(&o, "\t\t<array/>\n");
		else {
			out_add(&o, "\t\t<array>\n");
			for (j = 0; j < g->nmembers; j++) {
				out_add(&o, "\t\t\t<string>");
				out_escaped(&o, g->members[j]);
				out_add(&o, "</string>\n");
			}
			out_add(&o, "\t\t</array>\n");
		}
		out_add(&o, "\t</dict>\n");
	}
	out_add(&o, plist_tail);
	free(sorted);
	if (o.err) {
		free(o.buf);
		errno = ENOMEM;
		return (-1);
	}
	rv = ds_write_atomic(path, o.buf, o.len, 0644);
	free(o.buf);
	return (rv);
}

struct ds_group *
ds_group_find(const struct ds_groups *gs, const char *name)
{
	size_t i;

	for (i = 0; i < gs->n; i++)
		if (strcmp(gs->v[i].name, name) == 0)
			return (&gs->v[i]);
	return (NULL);
}

struct ds_group *
ds_group_find_gid(const struct ds_groups *gs, gid_t gid)
{
	size_t i;

	for (i = 0; i < gs->n; i++)
		if (gs->v[i].gid == gid)
			return (&gs->v[i]);
	return (NULL);
}

struct ds_group *
ds_group_add(struct ds_groups *gs, const char *name, gid_t gid)
{
	struct ds_group *nv, *g;

	nv = realloc(gs->v, (gs->n + 1) * sizeof(*nv));
	if (nv == NULL)
		return (NULL);
	gs->v = nv;
	g = &gs->v[gs->n];
	memset(g, 0, sizeof(*g));
	g->name = strdup(name);
	if (g->name == NULL)
		return (NULL);
	g->gid = gid;
	gs->n++;
	return (g);
}

int
ds_group_remove(struct ds_groups *gs, const char *name)
{
	struct ds_group *g;
	size_t idx;

	g = ds_group_find(gs, name);
	if (g == NULL) {
		errno = ENOENT;
		return (-1);
	}
	idx = (size_t)(g - gs->v);
	group_clear(g);
	memmove(&gs->v[idx], &gs->v[idx + 1], (gs->n - idx - 1) * sizeof(*g));
	gs->n--;
	return (0);
}

bool
ds_group_has_member(const struct ds_group *g, const char *user)
{
	size_t i;

	for (i = 0; i < g->nmembers; i++)
		if (strcmp(g->members[i], user) == 0)
			return (true);
	return (false);
}

int
ds_group_add_member(struct ds_group *g, const char *user)
{
	char **nm, *copy;

	if (ds_group_has_member(g, user))
		return (0);
	copy = strdup(user);
	if (copy == NULL)
		return (-1);
	nm = realloc(g->members, (g->nmembers + 1) * sizeof(*nm));
	if (nm == NULL) {
		free(copy);
		return (-1);
	}
	g->members = nm;
	g->members[g->nmembers++] = copy;
	return (0);
}

int
ds_group_remove_member(struct ds_group *g, const char *user)
{
	size_t i;

	for (i = 0; i < g->nmembers; i++) {
		if (strcmp(g->members[i], user) != 0)
			continue;
		free(g->members[i]);
		memmove(&g->members[i], &g->members[i + 1],
		    (g->nmembers - i - 1) * sizeof(char *));
		g->nmembers--;
		return (0);
	}
	errno = ENOENT;
	return (-1);
}

int
ds_groups_drop_member(struct ds_groups *gs, const char *user)
{
	size_t i;
	int changed = 0;

	for (i = 0; i < gs->n; i++)
		if (ds_group_remove_member(&gs->v[i], user) == 0)
			changed++;
	return (changed);
}

/* ---- ids ----------------------------------------------------------------- */

uid_t
ds_next_id(const struct ds_users *us, const struct ds_groups *gs, uid_t start)
{
	uid_t id;

	for (id = start; id < 60000; id++) {
		if (ds_user_find_uid(us, id) != NULL)
			continue;
		if (ds_group_find_gid(gs, (gid_t)id) != NULL)
			continue;
		if (getpwuid(id) != NULL || getgrgid((gid_t)id) != NULL)
			continue;
		return (id);
	}
	return (0);
}
