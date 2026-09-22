/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
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
 * Routing for the account tools (E18 U9): which database an account is in,
 * and where a new one goes. The system files are parsed directly, never
 * through nsswitch, because on NextBSD the switch may answer from the
 * plists and the point here is to tell the two apart.
 */

#include <sys/param.h>
#include <sys/stat.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "ds.h"

/*
 * Scan a passwd(5)/group(5)-style file for a name or an id (field 0 and
 * field 2). Returns 0 and fills namebuf/idp when found.
 */
static int
scan_colon_file(const char *path, const char *name, unsigned long id,
    char *namebuf, size_t len, unsigned long *idp)
{
	FILE *fp;
	char *line = NULL, *p, *f0, *f2, *ep;
	size_t cap = 0;
	ssize_t got;
	unsigned long v;
	int rv = -1;

	fp = fopen(path, "re");
	if (fp == NULL)
		return (-1);
	while ((got = getline(&line, &cap, fp)) != -1) {
		if (got > 0 && line[got - 1] == '\n')
			line[got - 1] = '\0';
		if (line[0] == '#' || line[0] == '\0' || line[0] == '+' ||
		    line[0] == '-')
			continue;
		p = line;
		f0 = strsep(&p, ":");
		(void)strsep(&p, ":");
		f2 = strsep(&p, ":");
		if (f0 == NULL || f2 == NULL)
			continue;
		errno = 0;
		v = strtoul(f2, &ep, 10);
		if (f2[0] == '\0' || *ep != '\0' || errno != 0)
			continue;
		if (name != NULL ? strcmp(f0, name) != 0 : v != id)
			continue;
		if (namebuf != NULL)
			(void)strlcpy(namebuf, f0, len);
		if (idp != NULL)
			*idp = v;
		rv = 0;
		break;
	}
	free(line);
	(void)fclose(fp);
	if (rv == -1)
		errno = ENOENT;
	return (rv);
}

int
ds_system_user(const char *name, uid_t uid, char *namebuf, size_t len,
    uid_t *uidp)
{
	char path[PATH_MAX];
	unsigned long id = 0;
	int rv;

	/* master.passwd is 0600; unprivileged callers read passwd instead. */
	rv = scan_colon_file(ds_syspath(path, sizeof(path), DS_MASTER_PASSWD),
	    name, uid, namebuf, len, &id);
	if (rv == -1 && errno != ENOENT)
		rv = scan_colon_file(ds_syspath(path, sizeof(path), DS_PASSWD),
		    name, uid, namebuf, len, &id);
	if (rv == 0 && uidp != NULL)
		*uidp = (uid_t)id;
	return (rv);
}

int
ds_system_group(const char *name, gid_t gid, char *namebuf, size_t len,
    gid_t *gidp)
{
	char path[PATH_MAX];
	unsigned long id = 0;
	int rv;

	rv = scan_colon_file(ds_syspath(path, sizeof(path), DS_GROUP),
	    name, gid, namebuf, len, &id);
	if (rv == 0 && gidp != NULL)
		*gidp = (gid_t)id;
	return (rv);
}

static int
plist_user(const char *name, uid_t uid, char *namebuf, size_t len)
{
	struct ds_users us;
	struct ds_user *u;
	char path[PATH_MAX];
	int rv = -1;

	if (ds_path(path, sizeof(path), ds_dir(), DS_USERS_PLIST) == -1)
		return (-1);
	if (ds_users_load(&us, path) == -1)
		return (-1);
	u = name != NULL ? ds_user_find(&us, name) : ds_user_find_uid(&us, uid);
	if (u != NULL) {
		if (namebuf != NULL)
			(void)strlcpy(namebuf, u->name, len);
		rv = 0;
	}
	ds_users_free(&us);
	if (rv == -1)
		errno = ENOENT;
	return (rv);
}

static int
plist_group(const char *name, gid_t gid, char *namebuf, size_t len)
{
	struct ds_groups gs;
	struct ds_group *g;
	char path[PATH_MAX];
	int rv = -1;

	if (ds_path(path, sizeof(path), ds_dir(), DS_GROUPS_PLIST) == -1)
		return (-1);
	if (ds_groups_load(&gs, path) == -1)
		return (-1);
	g = name != NULL ? ds_group_find(&gs, name) : ds_group_find_gid(&gs, gid);
	if (g != NULL) {
		if (namebuf != NULL)
			(void)strlcpy(namebuf, g->name, len);
		rv = 0;
	}
	ds_groups_free(&gs);
	if (rv == -1)
		errno = ENOENT;
	return (rv);
}

enum ds_where
ds_route_user(const char *name)
{
	if (plist_user(name, 0, NULL, 0) == 0)
		return (DS_WHERE_PLIST);
	if (ds_system_user(name, 0, NULL, 0, NULL) == 0)
		return (DS_WHERE_SYSTEM);
	return (DS_WHERE_NONE);
}

enum ds_where
ds_route_uid(uid_t uid, char *name, size_t len)
{
	if (plist_user(NULL, uid, name, len) == 0)
		return (DS_WHERE_PLIST);
	if (ds_system_user(NULL, uid, name, len, NULL) == 0)
		return (DS_WHERE_SYSTEM);
	return (DS_WHERE_NONE);
}

enum ds_where
ds_route_group(const char *name)
{
	if (plist_group(name, 0, NULL, 0) == 0)
		return (DS_WHERE_PLIST);
	if (ds_system_group(name, 0, NULL, 0, NULL) == 0)
		return (DS_WHERE_SYSTEM);
	return (DS_WHERE_NONE);
}

enum ds_where
ds_route_gid(gid_t gid, char *name, size_t len)
{
	if (plist_group(NULL, gid, name, len) == 0)
		return (DS_WHERE_PLIST);
	if (ds_system_group(NULL, gid, name, len, NULL) == 0)
		return (DS_WHERE_SYSTEM);
	return (DS_WHERE_NONE);
}

enum ds_where
ds_route_new_user(bool have_uid, uid_t uid, const char *shell, const char *home)
{
	if (have_uid && uid <= DS_SYSTEM_UID_MAX)
		return (DS_WHERE_SYSTEM);
	if (shell != NULL && strcmp(shell, DS_NOLOGIN) == 0 &&
	    (home == NULL || home[0] == '\0' || strcmp(home, DS_NONEXISTENT) == 0))
		return (DS_WHERE_SYSTEM);
	return (DS_WHERE_PLIST);
}

enum ds_where
ds_route_new_group(bool have_gid, gid_t gid, const char *name)
{
	if (have_gid)
		return (gid <= DS_SYSTEM_UID_MAX ? DS_WHERE_SYSTEM : DS_WHERE_PLIST);
	return (name != NULL && name[0] == '_' ? DS_WHERE_SYSTEM : DS_WHERE_PLIST);
}

bool
ds_shell_listed(const char *shell)
{
	FILE *fp;
	char path[PATH_MAX];
	char *line = NULL, *p;
	size_t cap = 0;
	ssize_t got;
	bool found = false;

	fp = fopen(ds_syspath(path, sizeof(path), DS_SHELLS), "re");
	if (fp == NULL)
		return (false);
	while (!found && (got = getline(&line, &cap, fp)) != -1) {
		if (got > 0 && line[got - 1] == '\n')
			line[got - 1] = '\0';
		p = line + strspn(line, " \t");
		if (*p == '#' || *p == '\0')
			continue;
		p[strcspn(p, " \t")] = '\0';
		found = strcmp(p, shell) == 0;
	}
	free(line);
	(void)fclose(fp);
	return (found);
}

/* The running program's path, for the "never exec ourselves" check. */
static int
self_path(char *buf, size_t len)
{
#if defined(__FreeBSD__)
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
	size_t n = len;

	return (sysctl(mib, 4, buf, &n, NULL, 0));
#elif defined(__APPLE__)
	uint32_t n = (uint32_t)len;

	return (_NSGetExecutablePath(buf, &n) == 0 ? 0 : -1);
#else
	(void)buf;
	(void)len;
	errno = ENOSYS;
	return (-1);
#endif
}

int
ds_bsd_tool(const char *name, const char *legacy, char *buf, size_t len)
{
	struct stat st, self;
	char me[PATH_MAX], path[PATH_MAX];
	int rv;

	rv = snprintf(path, sizeof(path), "%s/%s", DS_BSD_DIR, name);
	if (rv > 0 && (size_t)rv < sizeof(path) &&
	    stat(ds_syspath(buf, len, path), &st) == 0 && S_ISREG(st.st_mode) &&
	    (st.st_mode & S_IXUSR) != 0)
		return (0);
	if (legacy != NULL && stat(ds_syspath(buf, len, legacy), &st) == 0 &&
	    S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR) != 0) {
		if (self_path(me, sizeof(me)) == 0 && stat(me, &self) == 0 &&
		    self.st_dev == st.st_dev && self.st_ino == st.st_ino) {
			errno = ENOENT;	/* that is this wrapper, not FreeBSD's */
			return (-1);
		}
		return (0);
	}
	errno = ENOENT;
	return (-1);
}
