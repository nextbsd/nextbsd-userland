/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_users.c — CreateUser, DeleteUser, SetPassword and SetAdmin: Users &
 * Login.app's account operations (nextbsd/nextbsd#639).
 *
 * The accounts are the local directory's, /Local/Library/
 * DirectoryServices/{Users,Groups}.plist, written through libds as the
 * account tools write them, and the rules are theirs too (src/accounts/
 * common/acct.c, compiled in here as adduser and rmuser compile it):
 *
 *   CreateUser    adduser's: a valid, unused short name that isn't a
 *                 service account's ("_..."), the next free uid from
 *                 ACCT_FIRST_ID, a private group with the same name and
 *                 id, the default shell, then the home by createhomedir.
 *   DeleteUser    rmuser's: the account's processes, crontab and at jobs,
 *                 its memberships, its private group (only when it is
 *                 still private), the record, then the home. The record
 *                 goes before the home, so a failed write never leaves an
 *                 account without one.
 *   SetPassword   passwd's: the password hashed in login.conf's format,
 *                 or none (noPassword). A locked account is unlocked by a
 *                 new password, as passwd's own does.
 *   SetAdmin      membership of group admin.
 *
 * Two refusals the tools leave to their operator: the console user isn't
 * deleted from under their own session, and the last administrator is
 * never deleted or demoted (rmuser asks for -y; here there's no one to
 * ask). A machine joined to a directory server refuses all four, as the
 * tools do: its accounts belong on the server.
 *
 * Every write takes libds' domain lock, so these never interleave with an
 * account tool's run.
 */
#include <sys/types.h>

#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

#include "SettingsHelper.h"
#include "acct.h"
#include "libds.h"

/* Open the local directory for writing, unless this machine is joined. */
static int
open_local(struct ds_handle **h)
{
	char server[256];
	enum ds_error err;

	if (acct_bound_server(server, sizeof(server)))
		return (SH_ERR_UNSUPPORTED);
	if ((err = ds_open(DS_LOCAL, DS_RDWR, h)) != DS_OK) {
		sh_log("%s: %s", DS_LOCAL_DIR, ds_strerror(err));
		return (SH_ERR_FAILED);
	}
	return (SH_OK);
}

static int
commit(struct ds_handle *h)
{
	enum ds_error err;

	if ((err = ds_commit(h)) != DS_OK) {
		sh_log("%s: %s", ds_dir(h), ds_strerror(err));
		return (SH_ERR_FAILED);
	}
	return (SH_OK);
}

/*
 * A regular account in the local directory: what these operations may
 * touch. root, service accounts and anything in the system range stay
 * pw(8)'s.
 */
static int
get_regular(struct ds_handle *h, const char *name, struct ds_userrec *u)
{
	if (name[0] == '\0' || strcmp(name, "root") == 0 ||
	    acct_is_system_name(name) || ds_user_get(h, name, u) != DS_OK ||
	    acct_is_system_id(u->uid))
		return (SH_ERR_INVALID);
	return (SH_OK);
}

/* `name` is group admin's only member. */
static bool
last_admin(struct ds_handle *h, const char *name)
{
	char member[DS_NAME_MAX];
	size_t n, i;
	bool in = false, other = false;

	n = ds_group_member_count(h, ACCT_ADMIN_GROUP);
	for (i = 0; i < n; i++) {
		if (ds_group_member_at(h, ACCT_ADMIN_GROUP, i, member,
		    sizeof(member)) != DS_OK)
			continue;
		if (strcmp(member, name) == 0)
			in = true;
		else
			other = true;
	}
	return (in && !other);
}

/*
 * A full name for realName: printable, and without ':' or a newline,
 * which would split the passwd line getpwent(3) builds from it.
 */
static bool
full_name_ok(const char *s)
{
	const unsigned char *p;

	if (strlen(s) >= DS_REAL_MAX)
		return (false);
	for (p = (const unsigned char *)s; *p != '\0'; p++)
		if (*p < 0x20 || *p == 0x7f || *p == ':')
			return (false);
	return (true);
}

/* Hash `password` into u, or mark u as having none. */
static int
set_hash(struct ds_userrec *u, const char *password)
{
	if (password[0] == '\0') {
		u->noPassword = true;
		u->hasHash = false;
		u->passwordHash[0] = '\0';
		return (SH_OK);
	}
	if (!acct_hash_password(password, u->passwordHash,
	    sizeof(u->passwordHash))) {
		sh_log("the password couldn't be hashed");
		return (SH_ERR_FAILED);
	}
	u->hasHash = true;
	u->noPassword = false;
	return (SH_OK);
}

int
sh_user_create(const char *name, const char *full_name, const char *password,
    bool admin)
{
	struct ds_handle *h;
	struct ds_userrec u;
	struct ds_grouprec g;
	uid_t uid;
	int rc;

	if (acct_name_problem(name) != NULL || strlen(name) >= DS_NAME_MAX ||
	    acct_is_system_name(name) || !full_name_ok(full_name))
		return (SH_ERR_INVALID);
	if ((rc = open_local(&h)) != SH_OK)
		return (rc);
	/* The name, in the directory or in master.passwd and /etc/group. */
	if (ds_user_get(h, name, &u) == DS_OK || getpwnam(name) != NULL ||
	    ds_group_get(h, name, &g) == DS_OK || getgrnam(name) != NULL) {
		ds_close(h);
		return (SH_ERR_EXISTS);
	}
	if (ds_user_next_uid(h, ACCT_FIRST_ID, ACCT_LAST_ID, true, &uid) !=
	    DS_OK) {
		sh_log("no free uid from %d to %d", ACCT_FIRST_ID, ACCT_LAST_ID);
		ds_close(h);
		return (SH_ERR_FAILED);
	}
	/* The private group takes the uid as its gid. */
	if (ds_group_get_by_gid(h, (gid_t)uid, &g) == DS_OK ||
	    getgrgid((gid_t)uid) != NULL) {
		sh_log("%s: gid %u is taken", name, (unsigned)uid);
		ds_close(h);
		return (SH_ERR_EXISTS);
	}
	if (admin && ds_group_get(h, ACCT_ADMIN_GROUP, &g) != DS_OK) {
		sh_log("no %s group in %s", ACCT_ADMIN_GROUP, DS_LOCAL_DIR);
		ds_close(h);
		return (SH_ERR_FAILED);
	}

	memset(&u, 0, sizeof(u));
	(void)strlcpy(u.username, name, sizeof(u.username));
	(void)strlcpy(u.realName, full_name, sizeof(u.realName));
	(void)strlcpy(u.shell, ACCT_DEFAULT_SHELL, sizeof(u.shell));
	u.uid = uid;
	u.gid = (gid_t)uid;
	rc = set_hash(&u, password);
	memset(&g, 0, sizeof(g));
	(void)strlcpy(g.groupname, name, sizeof(g.groupname));
	g.gid = (gid_t)uid;
	if (rc == SH_OK && (ds_group_add(h, &g) != DS_OK ||
	    ds_user_add(h, &u) != DS_OK ||
	    (admin && ds_group_add_member(h, ACCT_ADMIN_GROUP, name) != DS_OK)))
		rc = SH_ERR_FAILED;
	if (rc == SH_OK)
		rc = commit(h);
	acct_zero(u.passwordHash, sizeof(u.passwordHash));
	ds_close(h);
	if (rc != SH_OK)
		return (rc);

	/* The account exists now; a home that failed can be made later
	 * (createhomedir also runs at login). */
	(void)acct_make_home(name, true);
	return (SH_OK);
}

int
sh_user_delete(const char *name, bool keep_home)
{
	struct ds_handle *h;
	struct ds_userrec u;
	struct ds_grouprec g, pg;
	struct sh_console console;
	size_t i;
	bool member;
	int rc;

	if ((rc = open_local(&h)) != SH_OK)
		return (rc);
	if ((rc = get_regular(h, name, &u)) != SH_OK) {
		ds_close(h);
		return (rc);
	}
	if (sh_console_session(&console) && console.uid == u.uid) {
		ds_close(h);
		return (SH_ERR_LOGGED_IN);
	}
	if (last_admin(h, name)) {
		ds_close(h);
		return (SH_ERR_LAST_ADMIN);
	}

	/* What runs as the account goes first, as with rmuser. */
	(void)acct_kill_uid(u.uid);
	if (acct_remove_cron(name) == -1)
		sh_log("%s: the crontab couldn't be removed", name);
	if (acct_remove_at(name) == -1)
		sh_log("%s: the at jobs couldn't be removed", name);

	for (i = 0; i < ds_group_count(h) && rc == SH_OK; i++)
		if (ds_group_at(h, i, &g) == DS_OK &&
		    ds_group_has_member(h, g.groupname, name, &member) ==
		    DS_OK && member &&
		    ds_group_del_member(h, g.groupname, name) != DS_OK)
			rc = SH_ERR_FAILED;
	/* adduser's private group: same name, the account's gid, and nobody
	 * in it. A group that only shares the name is somebody else's. */
	if (rc == SH_OK && ds_group_get(h, name, &pg) == DS_OK &&
	    pg.gid == u.gid && ds_group_member_count(h, name) == 0 &&
	    ds_group_del(h, name) != DS_OK)
		rc = SH_ERR_FAILED;
	if (rc == SH_OK && ds_user_del(h, name) != DS_OK)
		rc = SH_ERR_FAILED;
	if (rc == SH_OK)
		rc = commit(h);
	ds_close(h);
	if (rc != SH_OK)
		return (rc);

	if (!keep_home && acct_remove_home(name) == -1)
		sh_log("%s: the home couldn't be removed", name);
	/* An account that's gone can't log in automatically. */
	(void)sh_login_forget_user(name);
	return (SH_OK);
}

int
sh_user_set_password(const char *name, const char *password)
{
	struct ds_handle *h;
	struct ds_userrec u;
	int rc;

	if ((rc = open_local(&h)) != SH_OK)
		return (rc);
	if ((rc = get_regular(h, name, &u)) == SH_OK &&
	    (rc = set_hash(&u, password)) == SH_OK) {
		if (ds_user_set(h, &u) != DS_OK)
			rc = SH_ERR_FAILED;
		else
			rc = commit(h);
	}
	acct_zero(u.passwordHash, sizeof(u.passwordHash));
	ds_close(h);
	return (rc);
}

int
sh_user_set_admin(const char *name, bool admin)
{
	struct ds_handle *h;
	struct ds_userrec u;
	struct ds_grouprec g;
	bool member = false;
	int rc;

	if ((rc = open_local(&h)) != SH_OK)
		return (rc);
	if ((rc = get_regular(h, name, &u)) != SH_OK) {
		ds_close(h);
		return (rc);
	}
	if (ds_group_get(h, ACCT_ADMIN_GROUP, &g) != DS_OK) {
		sh_log("no %s group in %s", ACCT_ADMIN_GROUP, DS_LOCAL_DIR);
		ds_close(h);
		return (SH_ERR_FAILED);
	}
	(void)ds_group_has_member(h, ACCT_ADMIN_GROUP, name, &member);
	if (member == admin) {
		ds_close(h);
		return (SH_OK);
	}
	if (!admin && last_admin(h, name)) {
		ds_close(h);
		return (SH_ERR_LAST_ADMIN);
	}
	if ((admin ? ds_group_add_member(h, ACCT_ADMIN_GROUP, name) :
	    ds_group_del_member(h, ACCT_ADMIN_GROUP, name)) != DS_OK)
		rc = SH_ERR_FAILED;
	else
		rc = commit(h);
	ds_close(h);
	return (rc);
}

/*
 * Has the local account `name` no password? SetAutoLogin's check: only
 * such an account may log in automatically.
 */
bool
sh_user_is_passwordless(const char *name)
{
	struct ds_handle *h;
	struct ds_userrec u;
	bool yes;

	if (ds_open(DS_LOCAL, DS_RDONLY, &h) != DS_OK)
		return (false);
	yes = get_regular(h, name, &u) == SH_OK && u.noPassword &&
	    (!u.hasHash || u.passwordHash[0] == '\0');
	acct_zero(u.passwordHash, sizeof(u.passwordHash));
	ds_close(h);
	return (yes);
}
