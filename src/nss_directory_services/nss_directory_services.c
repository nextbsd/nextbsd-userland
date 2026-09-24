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
 * nss_directory_services: the `directory_services` source for the passwd
 * and group databases in nsswitch.conf(5). It resolves users and groups
 * straight from the DirectoryServices plists, with no daemon and no
 * socket:
 *
 *   /Network/Library/DirectoryServices/{Users,Groups}.plist when present,
 *   /Local/Library/DirectoryServices/{Users,Groups}.plist otherwise.
 *
 * The choice is made on every lookup, so joining or leaving a directory
 * server (mounting or unmounting /Network) switches sources at once. A
 * missing or unparsable plist makes this source unavailable, and the next
 * source in nsswitch.conf answers.
 *
 * Method conventions follow libc's own `files` source (lib/libc/gen/
 * getpwent.c, getgrent.c): the record goes into the caller's buffer, a
 * buffer that is too small sets *errnop = ERANGE and returns NS_RETURN so
 * the caller retries with a larger one, and enumeration keeps a per-process
 * cursor between setpwent(3) and endpwent(3).
 *
 * Adapted by Joseph Maloney from his DirectoryServices NSS module for
 * Gershwin (nss_gershwin and dshelper in gershwin-desktop/
 * gershwin-components, BSD-2-Clause), which resolves the same plists
 * through a helper daemon over a socket. This version is integrated for
 * NextBSD: it reads the plists directly through FreeBSD's nsswitch
 * interface, keeping the file format and the lookup rules (the /Network
 * over /Local choice, the passwd field rules, wheel for members of admin,
 * the hardware-access groups) so Gershwin's dshelper and this module
 * read the same files.
 */

#include <sys/param.h>

#include <errno.h>
#include <grp.h>
#include <nsswitch.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "dsdb.h"

enum lookup {
	LT_NAME,
	LT_ID,
	LT_ALL
};

enum cursor_op {
	OP_SET,
	OP_END
};

static size_t pw_cursor;
static size_t gr_cursor;

/* ---- passwd -------------------------------------------------------------- */

static int
ds_passwd(void *retval, void *mdata, va_list ap)
{
	enum lookup how = (enum lookup)(uintptr_t)mdata;
	const struct ds_user *u;
	const char *name = NULL;
	struct passwd *pwd;
	char *buffer;
	size_t bufsize;
	uid_t uid = 0;
	int *errnop, rv;

	switch (how) {
	case LT_NAME:
		name = va_arg(ap, const char *);
		break;
	case LT_ID:
		uid = va_arg(ap, uid_t);
		break;
	case LT_ALL:
		break;
	}
	pwd = va_arg(ap, struct passwd *);
	buffer = va_arg(ap, char *);
	bufsize = va_arg(ap, size_t);
	errnop = va_arg(ap, int *);

	dsdb_lock();
	if (dsdb_refresh_users() != DSDB_OK) {
		dsdb_unlock();
		return (NS_UNAVAIL);
	}
	switch (how) {
	case LT_NAME:
		u = dsdb_user_by_name(name);
		break;
	case LT_ID:
		u = dsdb_user_by_uid(uid);
		break;
	default:
		u = dsdb_user_at(pw_cursor);
		if (u != NULL)
			pw_cursor++;
		break;
	}
	if (u == NULL) {
		dsdb_unlock();
		return (NS_NOTFOUND);
	}
	rv = dsdb_pack_passwd(u, geteuid() == 0, dsdb_users_from_network(),
	    pwd, buffer, bufsize);
	if (rv != 0 && how == LT_ALL)
		pw_cursor--;		/* let the retry see this record */
	dsdb_unlock();
	if (rv != 0) {
		*errnop = rv;
		return (NS_RETURN);
	}
	if (retval != NULL)
		*(struct passwd **)retval = pwd;
	return (NS_SUCCESS);
}

static int
ds_pwcursor(void *retval __unused, void *mdata, va_list ap __unused)
{
	dsdb_lock();
	pw_cursor = 0;
	dsdb_unlock();
	(void)(enum cursor_op)(uintptr_t)mdata;
	return (NS_UNAVAIL);	/* nothing to keep open; every source is run */
}

/* ---- group --------------------------------------------------------------- */

static int
ds_group(void *retval, void *mdata, va_list ap)
{
	enum lookup how = (enum lookup)(uintptr_t)mdata;
	const struct ds_group *g;
	const char *name = NULL;
	struct group *grp;
	char *buffer;
	size_t bufsize;
	gid_t gid = 0;
	int *errnop, rv;

	switch (how) {
	case LT_NAME:
		name = va_arg(ap, const char *);
		break;
	case LT_ID:
		gid = va_arg(ap, gid_t);
		break;
	case LT_ALL:
		break;
	}
	grp = va_arg(ap, struct group *);
	buffer = va_arg(ap, char *);
	bufsize = va_arg(ap, size_t);
	errnop = va_arg(ap, int *);

	dsdb_lock();
	if (dsdb_refresh_groups() != DSDB_OK) {
		dsdb_unlock();
		return (NS_UNAVAIL);
	}
	switch (how) {
	case LT_NAME:
		g = dsdb_group_by_name(name);
		break;
	case LT_ID:
		g = dsdb_group_by_gid(gid);
		break;
	default:
		g = dsdb_group_at(gr_cursor);
		if (g != NULL)
			gr_cursor++;
		break;
	}
	if (g == NULL) {
		dsdb_unlock();
		return (NS_NOTFOUND);
	}
	rv = dsdb_pack_group(g, grp, buffer, bufsize);
	if (rv != 0 && how == LT_ALL)
		gr_cursor--;
	dsdb_unlock();
	if (rv != 0) {
		*errnop = rv;
		return (NS_RETURN);
	}
	if (retval != NULL)
		*(struct group **)retval = grp;
	return (NS_SUCCESS);
}

static int
ds_grcursor(void *retval __unused, void *mdata, va_list ap __unused)
{
	dsdb_lock();
	gr_cursor = 0;
	dsdb_unlock();
	(void)(enum cursor_op)(uintptr_t)mdata;
	return (NS_UNAVAIL);
}

/*
 * getgrouplist(3). For a directory user the answer is complete here (the
 * primary gid, the plist groups, wheel for admins, and the hardware groups
 * from /etc/group), so NS_SUCCESS ends the search. For anyone else,
 * NS_NOTFOUND lets `files` answer.
 */
static int
ds_getgroupmembership(void *retval __unused, void *mdata __unused, va_list ap)
{
	const char *uname;
	gid_t agroup, *groups;
	int maxgrp, *grpcnt, rv;

	uname = va_arg(ap, const char *);
	agroup = va_arg(ap, gid_t);
	groups = va_arg(ap, gid_t *);
	maxgrp = va_arg(ap, int);
	grpcnt = va_arg(ap, int *);

	dsdb_lock();
	if (dsdb_refresh_users() != DSDB_OK) {
		dsdb_unlock();
		return (NS_UNAVAIL);
	}
	/* Without a readable Groups.plist the user still has a primary gid. */
	(void)dsdb_refresh_groups();
	rv = dsdb_membership(uname, agroup, groups, maxgrp, grpcnt, DS_ETC_GROUP);
	dsdb_unlock();
	return (rv == 0 ? NS_SUCCESS : NS_NOTFOUND);
}

/* ---- registration -------------------------------------------------------- */

static ns_mtab methods[] = {
	{ NSDB_PASSWD, "getpwnam_r", ds_passwd, (void *)LT_NAME },
	{ NSDB_PASSWD, "getpwuid_r", ds_passwd, (void *)LT_ID },
	{ NSDB_PASSWD, "getpwent_r", ds_passwd, (void *)LT_ALL },
	{ NSDB_PASSWD, "setpwent", ds_pwcursor, (void *)OP_SET },
	{ NSDB_PASSWD, "endpwent", ds_pwcursor, (void *)OP_END },
	{ NSDB_GROUP, "getgrnam_r", ds_group, (void *)LT_NAME },
	{ NSDB_GROUP, "getgrgid_r", ds_group, (void *)LT_ID },
	{ NSDB_GROUP, "getgrent_r", ds_group, (void *)LT_ALL },
	{ NSDB_GROUP, "setgrent", ds_grcursor, (void *)OP_SET },
	{ NSDB_GROUP, "endgrent", ds_grcursor, (void *)OP_END },
	{ NSDB_GROUP, "getgroupmembership", ds_getgroupmembership, NULL },
};

/* nsswitch.h declares the type of this entry point but not the function. */
ns_mtab	*nss_module_register(const char *, unsigned int *,
	    nss_module_unregister_fn *);

static pthread_once_t atfork_once = PTHREAD_ONCE_INIT;

static void
install_atfork(void)
{
	(void)pthread_atfork(dsdb_lock, dsdb_unlock, dsdb_atfork_child);
}

ns_mtab *
nss_module_register(const char *source __unused, unsigned int *mtabsize,
    nss_module_unregister_fn *unregister)
{
	(void)pthread_once(&atfork_once, install_atfork);
	*mtabsize = nitems(methods);
	*unregister = NULL;
	return (methods);
}
