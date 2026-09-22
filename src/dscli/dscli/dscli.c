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
 * dscli: NextBSD's DirectoryServices tool. Local users and groups in the
 * plists that nss_directory_services reads, and the promote, demote, join
 * and leave verbs that turn a machine into a directory server or a client
 * of one. No GNUstep, no gdomap, no fstab: launchd jobs, NFS and Bonjour.
 */

#include <sys/param.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <dns_sd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"

#ifndef nitems
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif
#define ARGV(s)		((char *)(uintptr_t)(s))

static const char *progname = "dscli";
static int dry_run;
static int assume_yes;

static void usage(void);

#define ARG(i)	((i) < argc ? argv[i] : NULL)

/* ---- output helpers ------------------------------------------------------ */

static void
warnx_(const char *fmt, const char *arg)
{
	fprintf(stderr, "%s: ", progname);
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
}

static void
warn_(const char *what)
{
	fprintf(stderr, "%s: %s: %s\n", progname, what, strerror(errno));
}

static int
need_root(void)
{
	if (geteuid() != 0) {
		fprintf(stderr, "%s: must be root\n", progname);
		return (-1);
	}
	return (0);
}

/*
 * Edits go to the database in use. A joined client reads the server's
 * copy over a read-only mount, so its accounts are edited on the server.
 */
static int
need_writable(void)
{
	if (ds_from_network()) {
		fprintf(stderr, "%s: the directory in use is %s, mounted from "
		    "the server; edit accounts there\n", progname, ds_network_dir());
		return (-1);
	}
	return (0);
}

/* ---- the database -------------------------------------------------------- */

struct db {
	struct ds_users		 users;
	struct ds_groups	 groups;
	char			 upath[PATH_MAX];
	char			 gpath[PATH_MAX];
	int			 lockfd;
};

static int
db_open(struct db *db, int forwrite)
{
	const char *dir = ds_dir();

	memset(db, 0, sizeof(*db));
	db->lockfd = -1;
	if (ds_path(db->upath, sizeof(db->upath), dir, DS_USERS_PLIST) == -1 ||
	    ds_path(db->gpath, sizeof(db->gpath), dir, DS_GROUPS_PLIST) == -1) {
		warn_(dir);
		return (-1);
	}
	if (forwrite) {
		if (ds_mkdirs(dir, 0755) == -1) {
			warn_(dir);
			return (-1);
		}
		db->lockfd = ds_lock(dir);
		if (db->lockfd == -1) {
			warn_("lock");
			return (-1);
		}
	}
	if (ds_users_load(&db->users, db->upath) == -1) {
		warn_(db->upath);
		goto fail;
	}
	if (ds_groups_load(&db->groups, db->gpath) == -1) {
		warn_(db->gpath);
		goto fail;
	}
	return (0);
fail:
	ds_users_free(&db->users);
	ds_groups_free(&db->groups);
	ds_unlock(db->lockfd);
	return (-1);
}

static int
db_save(struct db *db, int users, int groups)
{
	if (users && ds_users_save(&db->users, db->upath) == -1) {
		warn_(db->upath);
		return (-1);
	}
	if (groups && ds_groups_save(&db->groups, db->gpath) == -1) {
		warn_(db->gpath);
		return (-1);
	}
	return (0);
}

static void
db_close(struct db *db)
{
	ds_users_free(&db->users);
	ds_groups_free(&db->groups);
	ds_unlock(db->lockfd);
}

/* ---- input --------------------------------------------------------------- */

static char *
read_line(FILE *fp)
{
	char buf[1024];
	size_t n;

	if (fgets(buf, sizeof(buf), fp) == NULL)
		return (NULL);
	n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return (strdup(buf));
}

/*
 * A password: from the terminal, twice, when stdin is a tty; otherwise
 * one line from stdin (scripts, and the boot test).
 */
static char *
read_password(const char *what, int confirm)
{
	char prompt[128], *p1, *p2;

	if (!isatty(STDIN_FILENO))
		return (read_line(stdin));
	snprintf(prompt, sizeof(prompt), "%s: ", what);
	p1 = getpass(prompt);
	if (p1 == NULL)
		return (NULL);
	p1 = strdup(p1);
	if (p1 == NULL || !confirm)
		return (p1);
	snprintf(prompt, sizeof(prompt), "Retype %s: ", what);
	p2 = getpass(prompt);
	if (p2 == NULL || strcmp(p1, p2) != 0) {
		fprintf(stderr, "%s: passwords do not match\n", progname);
		free(p1);
		return (NULL);
	}
	return (p1);
}

/* Scrub a password from memory before freeing it. */
static void
wipe(char *s)
{
	volatile char *p = s;

	if (s == NULL)
		return;
	while (*p != '\0')
		*p++ = '\0';
	free(s);
}

static int
parse_id(const char *s, unsigned long *out)
{
	char *ep;

	errno = 0;
	*out = strtoul(s, &ep, 10);
	if (s[0] == '\0' || *ep != '\0' || errno != 0 || *out > 60000) {
		fprintf(stderr, "%s: bad id '%s'\n", progname, s);
		return (-1);
	}
	return (0);
}

/* Set the password fields of a record from a string ("" = noPassword). */
static int
set_password(struct ds_user *u, const char *password)
{
	if (password[0] == '\0') {
		free(u->hash);
		u->hash = NULL;
		u->nopass = true;
		return (0);
	}
	free(u->hash);
	u->hash = ds_hash_password(password);
	if (u->hash == NULL) {
		warn_("crypt");
		return (-1);
	}
	u->nopass = false;
	return (0);
}

static const char *
home_of(const char *name, char *buf, size_t len)
{
	snprintf(buf, len, "%s/%s",
	    ds_from_network() ? DS_NETWORK_USERS : DS_LOCAL_USERS, name);
	return (buf);
}

static void
say(const char *fmt, const char *a, const char *b)
{
	printf(fmt, a, b);
	fputc('\n', stdout);
}

/* ---- user ---------------------------------------------------------------- */

static void
print_user(const struct db *db, const struct ds_user *u)
{
	char home[PATH_MAX];
	size_t i;
	int first = 1;

	printf("Username:   %s\n", u->name);
	printf("UID:        %u\n", (unsigned)u->uid);
	printf("GID:        %u\n", (unsigned)u->gid);
	printf("Real name:  %s\n", u->real);
	printf("Home:       %s\n", home_of(u->name, home, sizeof(home)));
	printf("Shell:      %s\n", u->shell);
	printf("Password:   %s\n", u->nopass ? "none required" :
	    (u->hash != NULL ? "set" : "not set (login disabled)"));
	printf("Groups:    ");
	for (i = 0; i < db->groups.n; i++) {
		if (!ds_group_has_member(&db->groups.v[i], u->name))
			continue;
		printf("%s%s", first ? " " : ", ", db->groups.v[i].name);
		first = 0;
	}
	printf("%s\n", first ? " (none)" : "");
}

static int
cmp_uid(const void *a, const void *b)
{
	uid_t x = (*(const struct ds_user *const *)a)->uid;
	uid_t y = (*(const struct ds_user *const *)b)->uid;

	return (x < y ? -1 : x > y);
}

static int
cmd_user_list(void)
{
	struct db db;
	const struct ds_user **sorted;
	size_t i;

	if (db_open(&db, 0) == -1)
		return (1);
	sorted = calloc(db.users.n + 1, sizeof(*sorted));
	if (sorted == NULL) {
		db_close(&db);
		return (1);
	}
	for (i = 0; i < db.users.n; i++)
		sorted[i] = &db.users.v[i];
	qsort(sorted, db.users.n, sizeof(*sorted), cmp_uid);
	printf("%-20s %-6s %-6s %s\n", "USERNAME", "UID", "GID", "REAL NAME");
	for (i = 0; i < db.users.n; i++)
		printf("%-20s %-6u %-6u %s\n", sorted[i]->name,
		    (unsigned)sorted[i]->uid, (unsigned)sorted[i]->gid,
		    sorted[i]->real);
	free(sorted);
	db_close(&db);
	return (0);
}

static int
cmd_user_show(const char *name)
{
	struct db db;
	const struct ds_user *u;

	if (name == NULL) {
		usage();
		return (2);
	}
	if (db_open(&db, 0) == -1)
		return (1);
	u = ds_user_find(&db.users, name);
	if (u == NULL) {
		warnx_("user not found: %s", name);
		db_close(&db);
		return (1);
	}
	print_user(&db, u);
	db_close(&db);
	return (0);
}

/*
 * user add NAME [--admin] [--uid N] [--gid N] [--shell PATH]
 *     [--real-name STR] [--password STR | --no-password]
 */
static int
cmd_user_add(int argc, char **argv)
{
	struct db db;
	struct ds_user *u;
	struct ds_group *g;
	const char *name = ARG(0), *shell = DS_DEFAULT_SHELL, *real = "";
	const char *password = NULL;
	char home[PATH_MAX];
	unsigned long id;
	uid_t uid = 0;
	gid_t gid = 0;
	int i, admin = 0, nopass = 0, rv = 1;
	char *pw = NULL;

	if (name == NULL) {
		usage();
		return (2);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--admin") == 0)
			admin = 1;
		else if (strcmp(argv[i], "--no-password") == 0)
			nopass = 1;
		else if ((strcmp(argv[i], "--uid") == 0) && i + 1 < argc) {
			if (parse_id(argv[++i], &id) == -1)
				return (2);
			uid = (uid_t)id;
		} else if ((strcmp(argv[i], "--gid") == 0) && i + 1 < argc) {
			if (parse_id(argv[++i], &id) == -1)
				return (2);
			gid = (gid_t)id;
		} else if (strcmp(argv[i], "--shell") == 0 && i + 1 < argc)
			shell = argv[++i];
		else if ((strcmp(argv[i], "--real-name") == 0 ||
		    strcmp(argv[i], "--realname") == 0) && i + 1 < argc)
			real = argv[++i];
		else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc)
			password = argv[++i];
		else {
			usage();
			return (2);
		}
	}
	if (!ds_valid_name(name)) {
		warnx_("bad user name '%s'", name);
		return (2);
	}
	if (need_root() == -1 || need_writable() == -1)
		return (1);
	if (db_open(&db, 1) == -1)
		return (1);
	if (ds_user_find(&db.users, name) != NULL) {
		warnx_("user already exists: %s", name);
		goto out;
	}
	if (uid == 0) {
		uid = ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
		if (uid == 0) {
			warnx_("no free uid%s", "");
			goto out;
		}
	} else if (ds_user_find_uid(&db.users, uid) != NULL) {
		warnx_("uid already in use: %s", argv[0]);
		goto out;
	}
	if (gid == 0)
		gid = (ds_group_find(&db.groups, name) != NULL) ?
		    ds_group_find(&db.groups, name)->gid : (gid_t)uid;

	/* The password: flag, --no-password, or prompted. */
	if (nopass)
		pw = strdup("");
	else if (password != NULL)
		pw = strdup(password);
	else
		pw = read_password("New password", 1);
	if (pw == NULL)
		goto out;

	u = ds_user_add(&db.users, name);
	if (u == NULL) {
		warn_("user add");
		goto out;
	}
	u->uid = uid;
	u->gid = gid;
	if (ds_set_string(&u->shell, shell) == -1 ||
	    ds_set_string(&u->real, real) == -1 || set_password(u, pw) == -1)
		goto out;

	/* The private group, and admin when asked. */
	g = ds_group_find(&db.groups, name);
	if (g == NULL)
		g = ds_group_add(&db.groups, name, gid);
	if (g == NULL || ds_group_add_member(g, name) == -1) {
		warn_("group add");
		goto out;
	}
	if (admin) {
		g = ds_group_find(&db.groups, DS_ADMIN_GROUP);
		if (g == NULL)
			g = ds_group_add(&db.groups, DS_ADMIN_GROUP, DS_ADMIN_GID);
		if (g == NULL || ds_group_add_member(g, name) == -1) {
			warn_("admin group");
			goto out;
		}
	}
	if (db_save(&db, 1, 1) == -1)
		goto out;

	home_of(name, home, sizeof(home));
	if (ds_make_home(home, uid, gid) == -1)
		warn_(home);
	else
		say("Created home %s%s", home, "");
	printf("User created: %s (uid %u, gid %u%s)\n", name, (unsigned)uid,
	    (unsigned)gid, admin ? ", admin" : "");
	rv = 0;
out:
	wipe(pw);
	db_close(&db);
	return (rv);
}

static int
cmd_user_delete(int argc, char **argv)
{
	struct db db;
	struct ds_group *admin;
	const char *name = ARG(0);
	char home[PATH_MAX];
	int i, remove_home = 0, rv = 1;

	if (name == NULL) {
		usage();
		return (2);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--remove-home") == 0)
			remove_home = 1;
		else {
			usage();
			return (2);
		}
	}
	if (need_root() == -1 || need_writable() == -1)
		return (1);
	if (db_open(&db, 1) == -1)
		return (1);
	if (ds_user_find(&db.users, name) == NULL) {
		warnx_("user not found: %s", name);
		goto out;
	}
	admin = ds_group_find(&db.groups, DS_ADMIN_GROUP);
	if (admin != NULL && admin->nmembers == 1 &&
	    ds_group_has_member(admin, name)) {
		warnx_("%s is the last member of admin; add another admin first", name);
		goto out;
	}
	home_of(name, home, sizeof(home));
	(void)ds_user_remove(&db.users, name);
	(void)ds_groups_drop_member(&db.groups, name);
	/* The private group goes with the user when nothing else uses it. */
	if (ds_group_find(&db.groups, name) != NULL &&
	    ds_group_find(&db.groups, name)->nmembers == 0)
		(void)ds_group_remove(&db.groups, name);
	if (db_save(&db, 1, 1) == -1)
		goto out;
	if (remove_home) {
		if (ds_remove_home(home) == -1)
			warn_(home);
		else
			say("Removed home %s%s", home, "");
	} else
		say("User deleted: %s (home %s kept)", name, home);
	if (remove_home)
		say("User deleted: %s%s", name, "");
	rv = 0;
out:
	db_close(&db);
	return (rv);
}

/* user passwd NAME [--no-password] [--no-prompt]; the password from stdin
 * when not on a terminal. */
static int
cmd_user_passwd(int argc, char **argv)
{
	struct db db;
	struct ds_user *u;
	const char *name = ARG(0);
	char *pw = NULL;
	int i, nopass = 0, rv = 1;

	if (name == NULL) {
		usage();
		return (2);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-password") == 0)
			nopass = 1;
		else if (strcmp(argv[i], "--no-prompt") == 0)
			;	/* stdin is used whenever it is not a tty */
		else {
			usage();
			return (2);
		}
	}
	if (need_root() == -1 || need_writable() == -1)
		return (1);
	if (db_open(&db, 1) == -1)
		return (1);
	u = ds_user_find(&db.users, name);
	if (u == NULL) {
		warnx_("user not found: %s", name);
		goto out;
	}
	pw = nopass ? strdup("") : read_password("New password", 1);
	if (pw == NULL || set_password(u, pw) == -1)
		goto out;
	if (db_save(&db, 1, 0) == -1)
		goto out;
	say(u->nopass ? "Password removed for %s%s" : "Password set for %s%s",
	    name, "");
	rv = 0;
out:
	wipe(pw);
	db_close(&db);
	return (rv);
}

/* user verify NAME: the password from the terminal or stdin; exit 0 if
 * it matches. For scripts and for Gershwin's SudoAskPass. */
static int
cmd_user_verify(const char *name)
{
	struct db db;
	const struct ds_user *u;
	char *pw;
	int ok;

	if (name == NULL) {
		usage();
		return (2);
	}
	if (db_open(&db, 0) == -1)
		return (1);
	u = ds_user_find(&db.users, name);
	if (u == NULL) {
		warnx_("user not found: %s", name);
		db_close(&db);
		return (1);
	}
	if (geteuid() != 0) {
		/* Hashes are in the file; verifying needs to read them. */
		fprintf(stderr, "%s: must be root to verify\n", progname);
		db_close(&db);
		return (1);
	}
	pw = read_password("Password", 0);
	if (pw == NULL) {
		db_close(&db);
		return (1);
	}
	ok = ds_verify_password(u, pw);
	wipe(pw);
	db_close(&db);
	printf("%s\n", ok ? "Authentication successful" : "Authentication failed");
	return (ok ? 0 : 1);
}

/* user edit NAME [--real-name STR] [--shell PATH] [--uid N] [--gid N] */
static int
cmd_user_edit(int argc, char **argv)
{
	struct db db;
	struct ds_user *u;
	const char *name = ARG(0), *shell = NULL, *real = NULL;
	unsigned long id;
	uid_t uid = 0;
	gid_t gid = 0;
	int i, rv = 1, set_uid = 0, set_gid = 0;

	if (name == NULL) {
		usage();
		return (2);
	}
	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "--real-name") == 0 ||
		    strcmp(argv[i], "--realname") == 0) && i + 1 < argc)
			real = argv[++i];
		else if (strcmp(argv[i], "--shell") == 0 && i + 1 < argc)
			shell = argv[++i];
		else if (strcmp(argv[i], "--uid") == 0 && i + 1 < argc) {
			if (parse_id(argv[++i], &id) == -1)
				return (2);
			uid = (uid_t)id;
			set_uid = 1;
		} else if (strcmp(argv[i], "--gid") == 0 && i + 1 < argc) {
			if (parse_id(argv[++i], &id) == -1)
				return (2);
			gid = (gid_t)id;
			set_gid = 1;
		} else {
			usage();
			return (2);
		}
	}
	if (need_root() == -1 || need_writable() == -1)
		return (1);
	if (db_open(&db, 1) == -1)
		return (1);
	u = ds_user_find(&db.users, name);
	if (u == NULL) {
		warnx_("user not found: %s", name);
		goto out;
	}
	if (set_uid && ds_user_find_uid(&db.users, uid) != NULL &&
	    ds_user_find_uid(&db.users, uid) != u) {
		warnx_("uid already in use%s", "");
		goto out;
	}
	if (real != NULL && ds_set_string(&u->real, real) == -1)
		goto out;
	if (shell != NULL && ds_set_string(&u->shell, shell) == -1)
		goto out;
	if (set_uid)
		u->uid = uid;
	if (set_gid)
		u->gid = gid;
	if (db_save(&db, 1, 0) == -1)
		goto out;
	say("User updated: %s%s", name, "");
	rv = 0;
out:
	db_close(&db);
	return (rv);
}

/* ---- group --------------------------------------------------------------- */

static int
cmp_gid(const void *a, const void *b)
{
	gid_t x = (*(const struct ds_group *const *)a)->gid;
	gid_t y = (*(const struct ds_group *const *)b)->gid;

	return (x < y ? -1 : x > y);
}

static void
print_members(const struct ds_group *g)
{
	size_t i;

	for (i = 0; i < g->nmembers; i++)
		printf("%s%s", i ? "," : "", g->members[i]);
}

static int
cmd_group_list(void)
{
	struct db db;
	const struct ds_group **sorted;
	size_t i;

	if (db_open(&db, 0) == -1)
		return (1);
	sorted = calloc(db.groups.n + 1, sizeof(*sorted));
	if (sorted == NULL) {
		db_close(&db);
		return (1);
	}
	for (i = 0; i < db.groups.n; i++)
		sorted[i] = &db.groups.v[i];
	qsort(sorted, db.groups.n, sizeof(*sorted), cmp_gid);
	printf("%-20s %-6s %s\n", "GROUP", "GID", "MEMBERS");
	for (i = 0; i < db.groups.n; i++) {
		printf("%-20s %-6u ", sorted[i]->name, (unsigned)sorted[i]->gid);
		print_members(sorted[i]);
		fputc('\n', stdout);
	}
	free(sorted);
	db_close(&db);
	return (0);
}

static int
cmd_group_show(const char *name)
{
	struct db db;
	const struct ds_group *g;

	if (name == NULL) {
		usage();
		return (2);
	}
	if (db_open(&db, 0) == -1)
		return (1);
	g = ds_group_find(&db.groups, name);
	if (g == NULL) {
		warnx_("group not found: %s", name);
		db_close(&db);
		return (1);
	}
	printf("Group:      %s\n", g->name);
	printf("GID:        %u\n", (unsigned)g->gid);
	printf("Members:    ");
	print_members(g);
	printf("%s\n", g->nmembers == 0 ? "(none)" : "");
	db_close(&db);
	return (0);
}

static int
cmd_group_add(int argc, char **argv)
{
	struct db db;
	const char *name = ARG(0);
	unsigned long id;
	gid_t gid = 0;
	int i, rv = 1;

	if (name == NULL) {
		usage();
		return (2);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--gid") == 0 && i + 1 < argc) {
			if (parse_id(argv[++i], &id) == -1)
				return (2);
			gid = (gid_t)id;
		} else {
			usage();
			return (2);
		}
	}
	if (!ds_valid_name(name)) {
		warnx_("bad group name '%s'", name);
		return (2);
	}
	if (need_root() == -1 || need_writable() == -1)
		return (1);
	if (db_open(&db, 1) == -1)
		return (1);
	if (ds_group_find(&db.groups, name) != NULL) {
		warnx_("group already exists: %s", name);
		goto out;
	}
	if (gid == 0) {
		gid = (gid_t)ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
		if (gid == 0) {
			warnx_("no free gid%s", "");
			goto out;
		}
	} else if (ds_group_find_gid(&db.groups, gid) != NULL) {
		warnx_("gid already in use%s", "");
		goto out;
	}
	if (ds_group_add(&db.groups, name, gid) == NULL) {
		warn_("group add");
		goto out;
	}
	if (db_save(&db, 0, 1) == -1)
		goto out;
	printf("Group created: %s (gid %u)\n", name, (unsigned)gid);
	rv = 0;
out:
	db_close(&db);
	return (rv);
}

static int
cmd_group_delete(const char *name)
{
	struct db db;
	const struct ds_group *g;
	size_t i;
	int rv = 1;

	if (name == NULL) {
		usage();
		return (2);
	}
	if (need_root() == -1 || need_writable() == -1)
		return (1);
	if (db_open(&db, 1) == -1)
		return (1);
	g = ds_group_find(&db.groups, name);
	if (g == NULL) {
		warnx_("group not found: %s", name);
		goto out;
	}
	if (strcmp(name, DS_ADMIN_GROUP) == 0) {
		warnx_("the admin group cannot be deleted%s", "");
		goto out;
	}
	for (i = 0; i < db.users.n; i++) {
		if (db.users.v[i].gid == g->gid) {
			warnx_("group is the primary group of %s", db.users.v[i].name);
			goto out;
		}
	}
	(void)ds_group_remove(&db.groups, name);
	if (db_save(&db, 0, 1) == -1)
		goto out;
	say("Group deleted: %s%s", name, "");
	rv = 0;
out:
	db_close(&db);
	return (rv);
}

static int
cmd_group_member(const char *gname, const char *uname, int add)
{
	struct db db;
	struct ds_group *g;
	int rv = 1;

	if (gname == NULL || uname == NULL) {
		usage();
		return (2);
	}
	if (need_root() == -1 || need_writable() == -1)
		return (1);
	if (db_open(&db, 1) == -1)
		return (1);
	g = ds_group_find(&db.groups, gname);
	if (g == NULL) {
		warnx_("group not found: %s", gname);
		goto out;
	}
	if (add && ds_user_find(&db.users, uname) == NULL) {
		warnx_("user not found: %s", uname);
		goto out;
	}
	if (add) {
		if (ds_group_add_member(g, uname) == -1) {
			warn_("add member");
			goto out;
		}
	} else if (ds_group_remove_member(g, uname) == -1) {
		warnx_("%s is not a member", uname);
		goto out;
	}
	if (db_save(&db, 0, 1) == -1)
		goto out;
	say(add ? "Added %s to %s" : "Removed %s from %s", uname, gname);
	rv = 0;
out:
	db_close(&db);
	return (rv);
}

/* ---- init, list, status -------------------------------------------------- */

/*
 * init [--admin-user NAME] [--real-name STR] [--password STR | --no-password]
 * Creates the directories and both plists with the admin group and a
 * first admin. Prompts for what is missing when on a terminal.
 */
static int
cmd_init(int argc, char **argv)
{
	struct db db;
	struct ds_user *u;
	struct ds_group *g;
	const char *name = NULL, *real = NULL, *password = NULL;
	char *nline = NULL, *rline = NULL, *pw = NULL;
	char home[PATH_MAX];
	struct stat st;
	uid_t uid;
	int i, nopass = 0, rv = 1;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--admin-user") == 0 && i + 1 < argc)
			name = argv[++i];
		else if ((strcmp(argv[i], "--real-name") == 0 ||
		    strcmp(argv[i], "--realname") == 0) && i + 1 < argc)
			real = argv[++i];
		else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc)
			password = argv[++i];
		else if (strcmp(argv[i], "--no-password") == 0)
			nopass = 1;
		else {
			usage();
			return (2);
		}
	}
	if (need_root() == -1)
		return (1);
	if (ds_from_network()) {
		warnx_("this machine is joined to a directory server; leave first%s", "");
		return (1);
	}
	if (ds_mkdirs(ds_local_dir(), 0755) == -1) {
		warn_(ds_local_dir());
		return (1);
	}
	if (ds_mkdirs(DS_LOCAL_USERS, 0755) == -1) {
		warn_(DS_LOCAL_USERS);
		return (1);
	}
	if (db_open(&db, 1) == -1)
		return (1);
	if (stat(db.upath, &st) == 0 && db.users.n > 0) {
		printf("%s already has %zu user(s); nothing to do\n",
		    db.upath, db.users.n);
		rv = 0;
		goto out;
	}
	if (name == NULL) {
		if (!isatty(STDIN_FILENO)) {
			warnx_("--admin-user is required when not on a terminal%s", "");
			goto out;
		}
		printf("Administrator user name: ");
		fflush(stdout);
		nline = read_line(stdin);
		name = nline;
	}
	if (!ds_valid_name(name)) {
		warnx_("bad user name '%s'", name != NULL ? name : "");
		goto out;
	}
	if (real == NULL) {
		if (isatty(STDIN_FILENO)) {
			printf("Real name [Administrator]: ");
			fflush(stdout);
			rline = read_line(stdin);
			real = (rline != NULL && rline[0] != '\0') ? rline : "Administrator";
		} else
			real = "Administrator";
	}
	if (nopass)
		pw = strdup("");
	else if (password != NULL)
		pw = strdup(password);
	else if (isatty(STDIN_FILENO)) {
		printf("An empty password lets %s log in without one.\n", name);
		pw = read_password("Password", 1);
	} else {
		warnx_("--password or --no-password is required when not on a terminal%s", "");
		goto out;
	}
	if (pw == NULL)
		goto out;

	uid = ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
	if (uid == 0) {
		warnx_("no free uid%s", "");
		goto out;
	}
	u = ds_user_add(&db.users, name);
	if (u == NULL) {
		warn_("user add");
		goto out;
	}
	u->uid = uid;
	u->gid = uid;
	if (ds_set_string(&u->real, real) == -1 || set_password(u, pw) == -1)
		goto out;
	g = ds_group_find(&db.groups, name);
	if (g == NULL)
		g = ds_group_add(&db.groups, name, uid);
	if (g == NULL || ds_group_add_member(g, name) == -1)
		goto out;
	g = ds_group_find(&db.groups, DS_ADMIN_GROUP);
	if (g == NULL)
		g = ds_group_add(&db.groups, DS_ADMIN_GROUP, DS_ADMIN_GID);
	if (g == NULL || ds_group_add_member(g, name) == -1)
		goto out;
	if (db_save(&db, 1, 1) == -1)
		goto out;
	snprintf(home, sizeof(home), "%s/%s", DS_LOCAL_USERS, name);
	if (ds_make_home(home, uid, uid) == -1)
		warn_(home);
	printf("Directory Services initialized in %s\n", ds_local_dir());
	printf("Administrator: %s (uid %u, member of admin)\n", name, (unsigned)uid);
	rv = 0;
out:
	wipe(pw);
	free(nline);
	free(rline);
	db_close(&db);
	return (rv);
}

static int
cmd_status(void)
{
	char server[256];
	enum ds_role role = ds_role(server, sizeof(server));
	struct db db;

	switch (role) {
	case DS_JOINED:
		printf("Role:       client, joined to %s\n", server);
		break;
	case DS_SERVER:
		printf("Role:       directory server\n");
		break;
	default:
		printf("Role:       standalone\n");
	}
	printf("Directory:  %s%s\n", ds_dir(),
	    ds_from_network() ? " (mounted from the server)" : "");
	if (db_open(&db, 0) == 0) {
		printf("Users:      %zu\n", db.users.n);
		printf("Groups:     %zu\n", db.groups.n);
		db_close(&db);
	}
	return (0);
}

static int
cmd_list(void)
{
	int rv;

	rv = cmd_status();
	fputc('\n', stdout);
	rv |= cmd_user_list();
	fputc('\n', stdout);
	rv |= cmd_group_list();
	return (rv);
}

/* ---- promote, demote ----------------------------------------------------- */

static void
step(const char *fmt, const char *a)
{
	printf(dry_run ? "would %s\n" : "%s\n", a);
	(void)fmt;
}

static int
copy_plist(const char *from_dir, const char *to_dir, const char *file)
{
	char from[PATH_MAX], to[PATH_MAX], *data;
	size_t len;
	struct stat st;
	int rv;

	if (ds_path(from, sizeof(from), from_dir, file) == -1 ||
	    ds_path(to, sizeof(to), to_dir, file) == -1)
		return (-1);
	if (stat(to, &st) == 0)
		return (0);		/* an existing network copy is kept */
	if (stat(from, &st) == -1) {
		/* No local copy: an empty database. */
		static const char empty[] =
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		    "<plist version=\"1.0\">\n<dict/>\n</plist>\n";
		return (ds_write_atomic(to, empty, sizeof(empty) - 1, 0644));
	}
	data = ds_read_file(from, &len);
	if (data == NULL)
		return (-1);
	rv = ds_write_atomic(to, data, len, 0644);
	free(data);
	return (rv);
}

static int
cmd_promote(int argc, char **argv)
{
	static const char *const jobs[] = { DS_JOB_RPCBIND, DS_JOB_MOUNTD, DS_JOB_NFSD };
	char hostname[MAXHOSTNAMELEN], msg[PATH_MAX + 128];
	const char *display = NULL;
	size_t i;
	int j;

	for (j = 0; j < argc; j++) {
		if (strcmp(argv[j], "--name") == 0 && j + 1 < argc)
			display = argv[++j];
		else {
			usage();
			return (2);
		}
	}
	if (need_root() == -1)
		return (1);
	if (ds_from_network()) {
		warnx_("this machine is joined to a directory server; leave first%s", "");
		return (1);
	}
	if (display == NULL) {
		if (gethostname(hostname, sizeof(hostname)) == -1)
			strlcpy(hostname, "NextBSD", sizeof(hostname));
		display = hostname;
	}
	/* Preflight, so a missing piece leaves nothing half done. */
	for (i = 0; i < nitems(jobs); i++) {
		if (!ds_job_installed(jobs[i])) {
			snprintf(msg, sizeof(msg), "%s/%s.plist is missing: the NFS "
			    "server jobs (nextbsd-userland#252) are not installed",
			    DS_LAUNCHDAEMONS, jobs[i]);
			warnx_("%s", msg);
			return (1);
		}
	}

	snprintf(msg, sizeof(msg), "create %s, seeded from %s", DS_NETWORK_DIR, ds_local_dir());
	step("%s", msg);
	if (!dry_run) {
		if (ds_mkdirs(DS_NETWORK_DIR, 0755) == -1 ||
		    copy_plist(ds_local_dir(), DS_NETWORK_DIR, DS_USERS_PLIST) == -1 ||
		    copy_plist(ds_local_dir(), DS_NETWORK_DIR, DS_GROUPS_PLIST) == -1) {
			warn_(DS_NETWORK_DIR);
			return (1);
		}
	}
	snprintf(msg, sizeof(msg), "write %s", DS_EXPORTS);
	step("%s", msg);
	if (!dry_run && ds_exports_write() == -1) {
		warn_(DS_EXPORTS);
		return (1);
	}
	for (i = 0; i < nitems(jobs); i++) {
		snprintf(msg, sizeof(msg), "launchctl load -w %s", jobs[i]);
		step("%s", msg);
		if (!dry_run && ds_launchctl("load", true, jobs[i]) != 0) {
			warnx_("launchctl load -w %s failed", jobs[i]);
			return (1);
		}
	}
	snprintf(msg, sizeof(msg), "announce \"%s\" as %s over Bonjour (%s/%s)",
	    display, DS_SERVICE_TYPE, DS_SERVICE_DIR, DS_SERVICE_FILE);
	step("%s", msg);
	if (!dry_run && ds_service_file_write(display) == -1) {
		warn_(DS_SERVICE_DIR);
		return (1);
	}
	if (!dry_run)
		printf("This machine is now a directory server. Clients: dscli join\n");
	return (0);
}

static int
cmd_demote(void)
{
	static const char *const jobs[] = { DS_JOB_NFSD, DS_JOB_MOUNTD, DS_JOB_RPCBIND };
	char msg[PATH_MAX + 64];
	size_t i;
	int rv = 0;

	if (need_root() == -1)
		return (1);
	snprintf(msg, sizeof(msg), "withdraw the Bonjour announcement (%s/%s)",
	    DS_SERVICE_DIR, DS_SERVICE_FILE);
	step("%s", msg);
	if (!dry_run && ds_service_file_remove() == -1) {
		warn_(DS_SERVICE_DIR);
		rv = 1;
	}
	for (i = 0; i < nitems(jobs); i++) {
		snprintf(msg, sizeof(msg), "launchctl unload -w %s", jobs[i]);
		step("%s", msg);
		if (!dry_run && ds_job_installed(jobs[i]) &&
		    ds_launchctl("unload", true, jobs[i]) != 0) {
			warnx_("launchctl unload -w %s failed", jobs[i]);
			rv = 1;
		}
	}
	snprintf(msg, sizeof(msg), "remove %s", DS_EXPORTS);
	step("%s", msg);
	if (!dry_run && ds_exports_ours() && ds_exports_remove() == -1) {
		warn_(DS_EXPORTS);
		rv = 1;
	}
	if (!dry_run)
		printf("No longer a directory server. %s is kept; remove it "
		    "yourself if it is no longer wanted.\n", DS_NETWORK_DIR);
	return (rv);
}

/* ---- join, leave --------------------------------------------------------- */

struct found {
	char	name[256];
	char	host[256];
	int	port;
};

struct browse_ctx {
	struct found	list[16];
	int		n;
};

static void DNSSD_API
resolve_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifindex,
    DNSServiceErrorType err, const char *fullname, const char *host,
    uint16_t port, uint16_t txtlen, const unsigned char *txt, void *ctx)
{
	struct found *f = ctx;
	const void *val;
	uint8_t vlen;
	size_t n;

	(void)ref; (void)flags; (void)ifindex; (void)fullname;
	if (err != kDNSServiceErr_NoError)
		return;
	strlcpy(f->host, host, sizeof(f->host));
	n = strlen(f->host);
	if (n > 0 && f->host[n - 1] == '.')
		f->host[n - 1] = '\0';
	f->port = ntohs(port);
	val = TXTRecordGetValuePtr(txtlen, txt, "name", &vlen);
	if (val != NULL && vlen > 0) {
		memcpy(f->name, val, vlen);
		f->name[vlen] = '\0';
	}
}

/* Run one DNSServiceRef for up to `seconds`, processing its results. */
static void
pump(DNSServiceRef ref, int seconds)
{
	struct timeval deadline, now, tv;
	fd_set fds;
	int fd = DNSServiceRefSockFD(ref);

	gettimeofday(&deadline, NULL);
	deadline.tv_sec += seconds;
	for (;;) {
		gettimeofday(&now, NULL);
		if (timercmp(&now, &deadline, >=))
			break;
		timersub(&deadline, &now, &tv);
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0)
			break;
		if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError)
			break;
	}
}

static void DNSSD_API
browse_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifindex,
    DNSServiceErrorType err, const char *name, const char *regtype,
    const char *domain, void *ctx)
{
	struct browse_ctx *bc = ctx;
	struct found *f;
	DNSServiceRef rref;
	int i;

	(void)ref;
	if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd))
		return;
	for (i = 0; i < bc->n; i++)
		if (strcmp(bc->list[i].name, name) == 0)
			return;
	if (bc->n >= (int)nitems(bc->list))
		return;
	f = &bc->list[bc->n];
	memset(f, 0, sizeof(*f));
	strlcpy(f->name, name, sizeof(f->name));
	if (DNSServiceResolve(&rref, 0, ifindex, name, regtype, domain,
	    resolve_cb, f) != kDNSServiceErr_NoError)
		return;
	pump(rref, 2);
	DNSServiceRefDeallocate(rref);
	if (f->host[0] != '\0')
		bc->n++;
}

/* Browse for servers; returns the chosen host in `host` or -1. */
static int
browse(char *host, size_t len)
{
	struct browse_ctx bc;
	DNSServiceRef ref;
	char *line;
	long pick = 1;
	int i;

	memset(&bc, 0, sizeof(bc));
	printf("Looking for directory servers (%s)...\n", DS_SERVICE_TYPE);
	if (DNSServiceBrowse(&ref, 0, 0, DS_SERVICE_TYPE, NULL, browse_cb, &bc)
	    != kDNSServiceErr_NoError) {
		warnx_("Bonjour browse failed; is mDNSResponder running?%s", "");
		return (-1);
	}
	pump(ref, 3);
	DNSServiceRefDeallocate(ref);
	if (bc.n == 0) {
		warnx_("no directory server found; run 'dscli promote' on the server, or give its host name%s", "");
		return (-1);
	}
	for (i = 0; i < bc.n; i++)
		printf("  %d. %s (%s:%d)\n", i + 1, bc.list[i].name,
		    bc.list[i].host, bc.list[i].port);
	if (bc.n > 1 || !(assume_yes || !isatty(STDIN_FILENO))) {
		if (!isatty(STDIN_FILENO)) {
			warnx_("several servers found; give a host name or use --yes with one%s", "");
			return (-1);
		}
		printf("Join which server? [1] ");
		fflush(stdout);
		line = read_line(stdin);
		if (line != NULL && line[0] != '\0')
			pick = strtol(line, NULL, 10);
		free(line);
		if (pick < 1 || pick > bc.n) {
			warnx_("no such entry%s", "");
			return (-1);
		}
	}
	strlcpy(host, bc.list[pick - 1].host, len);
	return (0);
}

static int
ntp_apply(const char *host)
{
	char msg[512];

	snprintf(msg, sizeof(msg), host != NULL ?
	    "point %s at %s" : "restore the default time sources in %s%s",
	    DS_NTP_CONF, host != NULL ? host : "");
	step("%s", msg);
	if (dry_run)
		return (0);
	if (ds_ntp_set_server(host) == -1) {
		warn_(DS_NTP_CONF);
		return (-1);
	}
	if (ds_job_installed(DS_JOB_NTPD) && ds_job_loaded(DS_JOB_NTPD)) {
		(void)ds_launchctl("unload", false, DS_JOB_NTPD);
		(void)ds_launchctl("load", false, DS_JOB_NTPD);
	}
	return (0);
}

static int
cmd_join(int argc, char **argv)
{
	char host[256], msg[PATH_MAX + 128], *mount_argv[2];
	struct stat st;
	int i;

	host[0] = '\0';
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--yes") == 0)
			assume_yes = 1;
		else if (argv[i][0] == '-') {
			usage();
			return (2);
		} else
			strlcpy(host, argv[i], sizeof(host));
	}
	if (need_root() == -1)
		return (1);
	if (ds_exports_ours()) {
		warnx_("this machine is a directory server; demote first%s", "");
		return (1);
	}
	if (!ds_job_installed(DS_JOB_NETWORK_MOUNT) ||
	    stat(DS_NETWORK_MOUNT_CMD, &st) == -1) {
		snprintf(msg, sizeof(msg), "%s.plist or %s is missing: the NFS client "
		    "job (nextbsd-userland#252) is not installed",
		    DS_JOB_NETWORK_MOUNT, DS_NETWORK_MOUNT_CMD);
		warnx_("%s", msg);
		return (1);
	}
	if (host[0] == '\0' && browse(host, sizeof(host)) == -1)
		return (1);

	snprintf(msg, sizeof(msg), "bind to %s (%s/%s)", host, ds_local_dir(), DS_BINDING_PLIST);
	step("%s", msg);
	if (!dry_run && ds_binding_write(host) == -1) {
		warn_(DS_BINDING_PLIST);
		return (1);
	}
	snprintf(msg, sizeof(msg), "launchctl load -w %s", DS_JOB_NETWORK_MOUNT);
	step("%s", msg);
	if (!dry_run && ds_launchctl("load", true, DS_JOB_NETWORK_MOUNT) != 0) {
		warnx_("launchctl load -w %s failed", DS_JOB_NETWORK_MOUNT);
		return (1);
	}
	snprintf(msg, sizeof(msg), "mount %s:%s and %s:%s now (%s)", host,
	    DS_NETWORK_DIR, host, DS_LOCAL_USERS, DS_NETWORK_MOUNT_CMD);
	step("%s", msg);
	if (!dry_run) {
		mount_argv[0] = ARGV(DS_NETWORK_MOUNT_CMD);
		mount_argv[1] = NULL;
		if (ds_run(mount_argv) != 0)
			warnx_("%s failed; the mount will be retried at boot", DS_NETWORK_MOUNT_CMD);
	}
	if (ntp_apply(host) == -1)
		return (1);
	if (!dry_run)
		printf("Joined %s. Accounts now come from %s.\n", host, DS_NETWORK_DIR);
	return (0);
}

static int
cmd_leave(void)
{
	static const char *const mounts[] = { DS_NETWORK_USERS, DS_NETWORK_DIR };
	char server[256], msg[PATH_MAX + 64], *umount_argv[3];
	size_t i;
	int rv = 0;

	if (need_root() == -1)
		return (1);
	if (ds_binding_read(server, sizeof(server)) == -1)
		strlcpy(server, "(unknown)", sizeof(server));
	snprintf(msg, sizeof(msg), "launchctl unload -w %s", DS_JOB_NETWORK_MOUNT);
	step("%s", msg);
	if (!dry_run && ds_job_installed(DS_JOB_NETWORK_MOUNT) &&
	    ds_launchctl("unload", true, DS_JOB_NETWORK_MOUNT) != 0)
		rv = 1;
	for (i = 0; i < nitems(mounts); i++) {
		snprintf(msg, sizeof(msg), "umount %s", mounts[i]);
		step("%s", msg);
		if (dry_run)
			continue;
		umount_argv[0] = ARGV("umount");
		umount_argv[1] = ARGV(mounts[i]);
		umount_argv[2] = NULL;
		(void)ds_run(umount_argv);	/* not mounted is fine */
	}
	snprintf(msg, sizeof(msg), "remove %s/%s", ds_local_dir(), DS_BINDING_PLIST);
	step("%s", msg);
	if (!dry_run && ds_binding_remove() == -1) {
		warn_(DS_BINDING_PLIST);
		rv = 1;
	}
	if (ntp_apply(NULL) == -1)
		rv = 1;
	if (!dry_run)
		printf("Left %s. Accounts now come from %s.\n", server, ds_local_dir());
	return (rv);
}

/* ---- main ---------------------------------------------------------------- */

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [--dry-run] command ...\n"
	    "\n"
	    "  init [--admin-user NAME] [--real-name STR] [--password STR | --no-password]\n"
	    "  status | list\n"
	    "\n"
	    "  user list\n"
	    "  user show NAME\n"
	    "  user add NAME [--admin] [--uid N] [--gid N] [--shell PATH]\n"
	    "                [--real-name STR] [--password STR | --no-password]\n"
	    "  user delete NAME [--remove-home]\n"
	    "  user passwd NAME [--no-password]\n"
	    "  user verify NAME\n"
	    "  user edit NAME [--real-name STR] [--shell PATH] [--uid N] [--gid N]\n"
	    "\n"
	    "  group list\n"
	    "  group show NAME\n"
	    "  group add NAME [--gid N]\n"
	    "  group delete NAME\n"
	    "  group addmember GROUP USER\n"
	    "  group removemember GROUP USER\n"
	    "\n"
	    "  promote [--name DISPLAYNAME]\n"
	    "  demote\n"
	    "  join [HOST] [--yes]\n"
	    "  leave\n",
	    progname);
}

int
main(int argc, char **argv)
{
	const char *cmd, *sub;
	int i = 1;

	while (i < argc && argv[i][0] == '-') {
		if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else if (strcmp(argv[i], "--yes") == 0)
			assume_yes = 1;
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage();
			return (0);
		} else {
			usage();
			return (2);
		}
		i++;
	}
	if (i >= argc) {
		usage();
		return (2);
	}
	cmd = argv[i++];
	argc -= i;
	argv += i;

	if (strcmp(cmd, "init") == 0)
		return (cmd_init(argc, argv));
	if (strcmp(cmd, "status") == 0)
		return (cmd_status());
	if (strcmp(cmd, "list") == 0)
		return (cmd_list());
	if (strcmp(cmd, "passwd") == 0)
		return (cmd_user_passwd(argc, argv));
	if (strcmp(cmd, "verify") == 0)
		return (cmd_user_verify(ARG(0)));
	if (strcmp(cmd, "promote") == 0)
		return (cmd_promote(argc, argv));
	if (strcmp(cmd, "demote") == 0)
		return (cmd_demote());
	if (strcmp(cmd, "join") == 0)
		return (cmd_join(argc, argv));
	if (strcmp(cmd, "leave") == 0)
		return (cmd_leave());
	if (strcmp(cmd, "user") == 0 || strcmp(cmd, "group") == 0) {
		sub = ARG(0);
		if (sub == NULL) {
			usage();
			return (2);
		}
		argc--;
		argv++;
		if (cmd[0] == 'u') {
			if (strcmp(sub, "list") == 0)
				return (cmd_user_list());
			if (strcmp(sub, "show") == 0)
				return (cmd_user_show(ARG(0)));
			if (strcmp(sub, "add") == 0)
				return (cmd_user_add(argc, argv));
			if (strcmp(sub, "delete") == 0)
				return (cmd_user_delete(argc, argv));
			if (strcmp(sub, "passwd") == 0)
				return (cmd_user_passwd(argc, argv));
			if (strcmp(sub, "verify") == 0)
				return (cmd_user_verify(ARG(0)));
			if (strcmp(sub, "edit") == 0)
				return (cmd_user_edit(argc, argv));
		} else {
			if (strcmp(sub, "list") == 0)
				return (cmd_group_list());
			if (strcmp(sub, "show") == 0)
				return (cmd_group_show(ARG(0)));
			if (strcmp(sub, "add") == 0)
				return (cmd_group_add(argc, argv));
			if (strcmp(sub, "delete") == 0)
				return (cmd_group_delete(ARG(0)));
			if (strcmp(sub, "addmember") == 0)
				return (cmd_group_member(ARG(0), ARG(1), 1));
			if (strcmp(sub, "removemember") == 0)
				return (cmd_group_member(ARG(0), ARG(1), 0));
		}
	}
	usage();
	return (2);
}
