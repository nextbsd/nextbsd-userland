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
 * pw(8) for NextBSD (E18 U9). The command line is FreeBSD's; what changes
 * is where the account goes:
 *
 *   - root, the system users and every group in /etc/group stay in
 *     master.passwd and group, edited by FreeBSD's pw kept at
 *     /usr/libexec/bsd/pw, which is run with the exact arguments. This is
 *     what pkg install scripts rely on (`pw useradd -n _x -u 123 -s
 *     /usr/sbin/nologin -d /nonexistent`).
 *   - proper users and their groups live in the DirectoryServices plists
 *     and are edited here through libds.
 *
 * A new user is a system account when its uid is 499 or below, or its
 * shell is nologin with no real home; a new group when its gid is 499 or
 * below, or it has no gid and a name starting with '_'. Existing accounts
 * are found where they are. Only the common flags are translated for a
 * directory account; an unsupported one is refused with a message, never
 * guessed.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "acct.h"
#include "ds.h"

enum which { W_NONE = -1, W_USER, W_GROUP };
enum mode { M_NONE = -1, M_ADD, M_DEL, M_MOD, M_SHOW, M_NEXT, M_LOCK, M_UNLOCK };

static const char *const modes[] = { "add", "del", "mod", "show", "next",
    "lock", "unlock" };
static const char *const combos[][2] = {
	{ "useradd", "adduser" }, { "userdel", "deluser" },
	{ "usermod", "moduser" }, { "usershow", "showuser" },
	{ "usernext", "nextuser" }, { "lock", "lock" }, { "unlock", "unlock" },
	{ "groupadd", "addgroup" }, { "groupdel", "delgroup" },
	{ "groupmod", "modgroup" }, { "groupshow", "showgroup" },
	{ "groupnext", "nextgroup" },
};

static const char *progname = "pw";
static char **orig_argv;
static bool quiet;

struct db {
	struct ds_users		users;
	struct ds_groups	groups;
	char			upath[PATH_MAX];
	char			gpath[PATH_MAX];
	int			lockfd;
};

#define LOCKED_PREFIX	"*LOCKED*"

static void
exec_bsd(void)
{
	char path[PATH_MAX];

	if (ds_bsd_tool("pw", "/usr/sbin/pw", path, sizeof(path)) == -1)
		errx(EX_UNAVAILABLE, "FreeBSD's pw is not available at %s/pw; "
		    "edit %s with vipw(8)", DS_BSD_DIR, DS_MASTER_PASSWD);
	execv(path, orig_argv);
	err(EX_OSERR, "exec %s", path);
}

static void
unsupported(int ch, const char *what)
{
	errx(EX_USAGE, "-%c is not supported for a directory %s; see dscli(8)",
	    ch, what);
}

static unsigned long
parse_id(const char *s, const char *what)
{
	char *ep;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &ep, 10);
	if (s[0] == '\0' || *ep != '\0' || errno != 0 || v > UINT_MAX)
		errx(EX_USAGE, "invalid %s '%s'", what, s);
	return (v);
}

static bool
all_digits(const char *s)
{
	return (s[0] != '\0' && s[strspn(s, "0123456789")] == '\0');
}

static void
need_root(void)
{
	if (geteuid() != 0)
		errx(EX_NOPERM, "you must be root to change accounts");
}

/* ---- the database ---------------------------------------------------- */

static void
db_open(struct db *db, bool write)
{
	memset(db, 0, sizeof(*db));
	db->lockfd = -1;
	if (write) {
		need_root();
		if (acct_refuse_if_joined(progname))
			exit(EX_NOPERM);
		db->lockfd = ds_lock(ds_dir());
		if (db->lockfd == -1)
			err(EX_OSERR, "lock %s", ds_dir());
	}
	if (ds_path(db->upath, sizeof(db->upath), ds_dir(), DS_USERS_PLIST) == -1 ||
	    ds_path(db->gpath, sizeof(db->gpath), ds_dir(), DS_GROUPS_PLIST) == -1)
		err(EX_OSERR, "path");
	if (ds_users_load(&db->users, db->upath) == -1)
		err(EX_DATAERR, "%s", db->upath);
	if (ds_groups_load(&db->groups, db->gpath) == -1)
		err(EX_DATAERR, "%s", db->gpath);
}

static void
db_save(struct db *db, bool users, bool groups)
{
	if (users && ds_users_save(&db->users, db->upath) == -1)
		err(EX_IOERR, "write %s", db->upath);
	if (groups && ds_groups_save(&db->groups, db->gpath) == -1)
		err(EX_IOERR, "write %s", db->gpath);
}

static void
db_close(struct db *db)
{
	if (db->lockfd != -1)
		ds_unlock(db->lockfd);
	ds_users_free(&db->users);
	ds_groups_free(&db->groups);
}

/* A gid from a group name or number, in the plists or /etc/group. */
static gid_t
resolve_gid(const struct db *db, const char *s)
{
	struct ds_group *g;
	gid_t gid;

	if (all_digits(s))
		return ((gid_t)parse_id(s, "gid"));
	g = ds_group_find(&db->groups, s);
	if (g != NULL)
		return (g->gid);
	if (ds_system_group(s, 0, NULL, 0, &gid) == 0)
		return (gid);
	errx(EX_NOUSER, "group '%s' does not exist", s);
}

static bool
user_exists_anywhere(const struct db *db, const char *name)
{
	return (ds_user_find(&db->users, name) != NULL ||
	    ds_system_user(name, 0, NULL, 0, NULL) == 0);
}

/* Read one line from a file descriptor (pw -h / -H). */
static char *
read_fd_line(int fd)
{
	char buf[1024];
	ssize_t got;
	size_t len = 0;

	for (;;) {
		got = read(fd, buf + len, 1);
		if (got <= 0 || buf[len] == '\n')
			break;
		if (++len >= sizeof(buf) - 1)
			break;
	}
	buf[len] = '\0';
	return (strdup(buf));
}

static int
parse_fd(const char *s)
{
	if (strcmp(s, "-") == 0)
		return (-2);
	return ((int)parse_id(s, "file descriptor"));
}

/*
 * The password for a new or modified directory user, from pw's -h, -H and
 * -w. Sets *hash (malloc'd, or NULL for "no usable password") and
 * *nopass. Returns -1 on failure with a message printed.
 */
static int
password_from_flags(const char *name, int hfd, int Hfd, const char *method,
    char **hash, bool *nopass)
{
	char *line;

	*hash = NULL;
	*nopass = false;
	if (Hfd != -1) {
		line = read_fd_line(Hfd);
		if (line == NULL || line[0] == '\0') {
			warnx("no pre-encrypted password read from fd %d", Hfd);
			free(line);
			return (-1);
		}
		*hash = line;
		return (0);
	}
	if (hfd == -2)		/* -h -: locked */
		return (0);
	if (hfd != -1) {
		line = read_fd_line(hfd);
		if (line == NULL) {
			warnx("no password read from fd %d", hfd);
			return (-1);
		}
		if (line[0] == '\0') {
			*nopass = true;
			acct_wipe(line);
			return (0);
		}
		*hash = ds_hash_password(line);
		acct_wipe(line);
		if (*hash == NULL) {
			warn("hashing the password");
			return (-1);
		}
		return (0);
	}
	if (method == NULL || strcmp(method, "no") == 0)
		return (0);	/* locked, as FreeBSD's default */
	if (strcmp(method, "none") == 0) {
		*nopass = true;
		return (0);
	}
	if (strcmp(method, "yes") == 0) {
		*hash = ds_hash_password(name);
		if (*hash == NULL) {
			warn("hashing the password");
			return (-1);
		}
		return (0);
	}
	warnx("-w %s is not supported for a directory user (use no, none or yes)",
	    method);
	return (-1);
}

/* Apply -G: the user's secondary groups become exactly this list. */
static void
set_secondary_groups(struct db *db, const char *name, const char *list)
{
	struct ds_group *g;
	char *copy, *p, *tok;
	size_t i;

	if (list == NULL)
		return;
	/* Drop the user from every plist group except its private one. */
	for (i = 0; i < db->groups.n; i++) {
		g = &db->groups.v[i];
		if (strcmp(g->name, name) != 0)
			(void)ds_group_remove_member(g, name);
	}
	copy = strdup(list);
	if (copy == NULL)
		err(EX_OSERR, "strdup");
	p = copy;
	while ((tok = strsep(&p, ",")) != NULL) {
		if (tok[0] == '\0')
			continue;
		g = ds_group_find(&db->groups, tok);
		if (g == NULL) {
			if (ds_system_group(tok, 0, NULL, 0, NULL) == 0)
				errx(EX_USAGE, "group '%s' is a system group in "
				    "%s; a directory user cannot join it", tok,
				    DS_GROUP);
			errx(EX_NOUSER, "group '%s' does not exist", tok);
		}
		if (ds_group_add_member(g, name) == -1)
			err(EX_OSERR, "add %s to %s", name, tok);
	}
	free(copy);
}

/* ---- user commands ----------------------------------------------------- */

static const char *
home_of(const char *name)
{
	static char home[PATH_MAX];

	snprintf(home, sizeof(home), "%s/%s",
	    ds_from_network() ? DS_NETWORK_USERS : DS_LOCAL_USERS, name);
	return (home);
}

static void
check_home_flag(const char *name, const char *homedir)
{
	if (homedir != NULL && strcmp(homedir, home_of(name)) != 0)
		errx(EX_USAGE, "a directory user's home is always %s; "
		    "-d %s cannot be honoured", home_of(name), homedir);
}

static int
user_add(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_user *u;
	struct ds_group *g;
	const char *name = NULL, *gecos = NULL, *homedir = NULL, *shell = NULL;
	const char *grname = NULL, *grlist = NULL, *method = NULL;
	char *hash;
	uid_t uid = 0;
	gid_t gid;
	bool have_uid = false, nopass, createhome = false, dup_ok = false;
	int ch, hfd = -1, Hfd = -1;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			uid = (uid_t)parse_id(arg1, "uid");
			have_uid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:u:c:d:e:p:g:G:mM:k:s:oL:i:w:h:H:Db:NPy:Y")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'u': uid = (uid_t)parse_id(optarg, "uid"); have_uid = true; break;
		case 'c': gecos = optarg; break;
		case 'd': homedir = optarg; break;
		case 'g': grname = optarg; break;
		case 'G': grlist = optarg; break;
		case 'm': createhome = true; break;
		case 's': shell = optarg; break;
		case 'o': dup_ok = true; break;
		case 'w': method = optarg; break;
		case 'h': hfd = parse_fd(optarg); break;
		case 'H': Hfd = parse_fd(optarg); break;
		case 'q': quiet = true; break;
		case 'C': case 'D': case 'N': case 'y': case 'Y':
			exec_bsd();	/* config, defaults, dry run, NIS: FreeBSD's */
			/* NOTREACHED */
		case 'M': case 'k': case 'P':
			break;		/* home mode, skeleton dir, pretty: ignored */
		case 'e': case 'p': case 'L': case 'i': case 'b':
			unsupported(ch, "user");
			/* NOTREACHED */
		default:
			exec_bsd();	/* let FreeBSD's print its usage */
		}
	}
	if (name == NULL)
		exec_bsd();

	if (ds_route_new_user(have_uid, uid, shell, homedir) == DS_WHERE_SYSTEM)
		exec_bsd();
	if (!ds_valid_name(name))
		errx(EX_USAGE, "invalid user name '%s'", name);
	check_home_flag(name, homedir);

	db_open(&db, true);
	if (user_exists_anywhere(&db, name))
		errx(EX_DATAERR, "user '%s' already exists", name);
	if (have_uid) {
		if (!dup_ok && (ds_user_find_uid(&db.users, uid) != NULL ||
		    ds_system_user(NULL, uid, NULL, 0, NULL) == 0))
			errx(EX_DATAERR, "uid %lu is already in use (-o allows it)",
			    (unsigned long)uid);
	} else
		uid = ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
	if (grname != NULL)
		gid = resolve_gid(&db, grname);
	else if (ds_group_find_gid(&db.groups, (gid_t)uid) == NULL &&
	    ds_system_group(NULL, (gid_t)uid, NULL, 0, NULL) == -1)
		gid = (gid_t)uid;
	else
		gid = (gid_t)ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
	if (password_from_flags(name, hfd, Hfd, method, &hash, &nopass) == -1)
		exit(EX_DATAERR);

	u = ds_user_add(&db.users, name);
	if (u == NULL)
		err(EX_OSERR, "add %s", name);
	u->uid = uid;
	u->gid = gid;
	u->hash = hash;
	u->nopass = nopass;
	if (ds_set_string(&u->shell, shell != NULL ? shell : DS_DEFAULT_SHELL) == -1 ||
	    ds_set_string(&u->real, gecos != NULL ? gecos : "") == -1)
		err(EX_OSERR, "set fields");
	if (grname == NULL) {
		g = ds_group_add(&db.groups, name, gid);
		if (g == NULL || ds_group_add_member(g, name) == -1)
			err(EX_OSERR, "private group %s", name);
	}
	set_secondary_groups(&db, name, grlist);
	db_save(&db, true, true);
	db_close(&db);

	if (createhome && ds_make_home(home_of(name), uid, gid) == -1)
		warn("create %s", home_of(name));
	return (0);
}

static void
rename_user(struct db *db, struct ds_user *u, const char *newname)
{
	struct ds_group *g;
	size_t i;
	char *old;

	if (!ds_valid_name(newname))
		errx(EX_USAGE, "invalid user name '%s'", newname);
	if (user_exists_anywhere(db, newname))
		errx(EX_DATAERR, "user '%s' already exists", newname);
	old = strdup(u->name);
	if (old == NULL || ds_set_string(&u->name, newname) == -1)
		err(EX_OSERR, "rename");
	for (i = 0; i < db->groups.n; i++) {
		g = &db->groups.v[i];
		if (ds_group_has_member(g, old)) {
			(void)ds_group_remove_member(g, old);
			(void)ds_group_add_member(g, newname);
		}
		if (strcmp(g->name, old) == 0 &&
		    ds_group_find(&db->groups, newname) == NULL)
			(void)ds_set_string(&g->name, newname);
	}
	warnx("renamed %s to %s; the home stays at %s", old, newname, home_of(old));
	free(old);
}

static int
user_mod(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_user *u;
	const char *name = NULL, *newname = NULL, *gecos = NULL, *homedir = NULL;
	const char *shell = NULL, *grname = NULL, *grlist = NULL, *method = NULL;
	char namebuf[64], *hash;
	uid_t uid = 0, newuid = 0;
	bool have_uid = false, have_newuid = false, nopass, createhome = false;
	int ch, hfd = -1, Hfd = -1;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			uid = (uid_t)parse_id(arg1, "uid");
			have_uid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:u:c:d:e:p:g:G:mM:l:k:s:w:L:h:H:NPYy:")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'u':
			if (name == NULL && !have_uid) {
				uid = (uid_t)parse_id(optarg, "uid");
				have_uid = true;
			} else {
				newuid = (uid_t)parse_id(optarg, "uid");
				have_newuid = true;
			}
			break;
		case 'c': gecos = optarg; break;
		case 'd': homedir = optarg; break;
		case 'g': grname = optarg; break;
		case 'G': grlist = optarg; break;
		case 'l': newname = optarg; break;
		case 'm': createhome = true; break;
		case 's': shell = optarg; break;
		case 'w': method = optarg; break;
		case 'h': hfd = parse_fd(optarg); break;
		case 'H': Hfd = parse_fd(optarg); break;
		case 'q': quiet = true; break;
		case 'C': case 'N': case 'y': case 'Y':
			exec_bsd();
			/* NOTREACHED */
		case 'M': case 'k': case 'P':
			break;
		case 'e': case 'p': case 'L':
			unsupported(ch, "user");
			/* NOTREACHED */
		default:
			exec_bsd();
		}
	}
	if (name == NULL) {
		if (!have_uid)
			exec_bsd();
		if (ds_route_uid(uid, namebuf, sizeof(namebuf)) != DS_WHERE_PLIST)
			exec_bsd();
		name = namebuf;
	} else if (ds_route_user(name) != DS_WHERE_PLIST)
		exec_bsd();
	check_home_flag(newname != NULL ? newname : name, homedir);

	db_open(&db, true);
	u = ds_user_find(&db.users, name);
	if (u == NULL)
		errx(EX_NOUSER, "user '%s' does not exist", name);
	if (have_newuid) {
		if (ds_user_find_uid(&db.users, newuid) != NULL ||
		    ds_system_user(NULL, newuid, NULL, 0, NULL) == 0)
			errx(EX_DATAERR, "uid %lu is already in use", (unsigned long)newuid);
		u->uid = newuid;
		warnx("uid changed; files in %s keep their old owner", home_of(name));
	}
	if (gecos != NULL && ds_set_string(&u->real, gecos) == -1)
		err(EX_OSERR, "set full name");
	if (shell != NULL && ds_set_string(&u->shell, shell) == -1)
		err(EX_OSERR, "set shell");
	if (grname != NULL)
		u->gid = resolve_gid(&db, grname);
	if (hfd != -1 || Hfd != -1 || method != NULL) {
		if (password_from_flags(name, hfd, Hfd, method, &hash, &nopass) == -1)
			exit(EX_DATAERR);
		free(u->hash);
		u->hash = hash;
		u->nopass = nopass;
	}
	set_secondary_groups(&db, name, grlist);
	if (newname != NULL) {
		rename_user(&db, u, newname);
		name = newname;
	}
	db_save(&db, true, true);
	if (createhome && ds_make_home(home_of(name), u->uid, u->gid) == -1)
		warn("create %s", home_of(name));
	db_close(&db);
	return (0);
}

static int
user_del(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_user *u;
	struct ds_group *g;
	const char *name = NULL;
	char namebuf[64], home[PATH_MAX];
	uid_t uid = 0;
	bool have_uid = false, remove_home = false;
	int ch;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			uid = (uid_t)parse_id(arg1, "uid");
			have_uid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:u:rYy:")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'u': uid = (uid_t)parse_id(optarg, "uid"); have_uid = true; break;
		case 'r': remove_home = true; break;
		case 'q': quiet = true; break;
		default:
			exec_bsd();
		}
	}
	if (name == NULL) {
		if (!have_uid)
			exec_bsd();
		if (ds_route_uid(uid, namebuf, sizeof(namebuf)) != DS_WHERE_PLIST)
			exec_bsd();
		name = namebuf;
	} else if (ds_route_user(name) != DS_WHERE_PLIST)
		exec_bsd();

	db_open(&db, true);
	u = ds_user_find(&db.users, name);
	if (u == NULL)
		errx(EX_NOUSER, "user '%s' does not exist", name);
	strlcpy(home, home_of(name), sizeof(home));
	if (ds_user_remove(&db.users, name) == -1)
		err(EX_OSERR, "remove %s", name);
	(void)ds_groups_drop_member(&db.groups, name);
	g = ds_group_find(&db.groups, name);
	if (g != NULL && g->nmembers == 0)
		(void)ds_group_remove(&db.groups, name);
	db_save(&db, true, true);
	db_close(&db);
	if (remove_home && ds_remove_home(home) == -1)
		warn("remove %s", home);
	return (0);
}

static void
print_user(const struct ds_user *u, bool seven, bool pretty)
{
	const char *pass;

	pass = geteuid() == 0 ? (u->nopass ? "" : (u->hash != NULL ? u->hash : "*"))
	    : "*";
	if (pretty) {
		printf("Login Name: %-15s   #%-12lu Group: %-15s #%lu\n",
		    u->name, (unsigned long)u->uid, u->name, (unsigned long)u->gid);
		printf(" Full Name: %s\n", u->real);
		printf("      Home: %-26s      Class: \n", home_of(u->name));
		printf("     Shell: %-26s     Office: \n", u->shell);
		printf("  Database: %s\n", ds_from_network() ? DS_NETWORK_DIR : DS_LOCAL_DIR);
		return;
	}
	if (seven)
		printf("%s:%s:%lu:%lu:%s:%s:%s\n", u->name, pass,
		    (unsigned long)u->uid, (unsigned long)u->gid, u->real,
		    home_of(u->name), u->shell);
	else
		printf("%s:%s:%lu:%lu::0:0:%s:%s:%s\n", u->name, pass,
		    (unsigned long)u->uid, (unsigned long)u->gid, u->real,
		    home_of(u->name), u->shell);
}

static int
user_show(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_user *u;
	const char *name = NULL;
	char namebuf[64];
	uid_t uid = 0;
	bool have_uid = false, all = false, seven = false, pretty = false;
	size_t i;
	int ch;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			uid = (uid_t)parse_id(arg1, "uid");
			have_uid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:u:FPa7")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'u': uid = (uid_t)parse_id(optarg, "uid"); have_uid = true; break;
		case 'a': all = true; break;
		case '7': seven = true; break;
		case 'P': pretty = true; break;
		case 'F': case 'q': break;
		default:
			exec_bsd();
		}
	}
	if (all) {
		/* The directory users first, then FreeBSD's pw for the rest. */
		db_open(&db, false);
		for (i = 0; i < db.users.n; i++)
			print_user(&db.users.v[i], seven, pretty);
		db_close(&db);
		fflush(stdout);
		exec_bsd();
	}
	if (name == NULL) {
		if (!have_uid)
			exec_bsd();
		if (ds_route_uid(uid, namebuf, sizeof(namebuf)) != DS_WHERE_PLIST)
			exec_bsd();
		name = namebuf;
	} else if (ds_route_user(name) != DS_WHERE_PLIST)
		exec_bsd();
	db_open(&db, false);
	u = ds_user_find(&db.users, name);
	if (u == NULL)
		errx(EX_NOUSER, "user '%s' does not exist", name);
	print_user(u, seven, pretty);
	db_close(&db);
	return (0);
}

static int
user_next(void)
{
	struct db db;
	uid_t id;

	db_open(&db, false);
	id = ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
	db_close(&db);
	printf("%lu:%lu\n", (unsigned long)id, (unsigned long)id);
	return (0);
}

static int
user_lock(int argc, char *argv[], const char *arg1, bool lock)
{
	struct db db;
	struct ds_user *u;
	const char *name = NULL;
	char namebuf[64], *nh;
	uid_t uid = 0;
	bool have_uid = false;
	int ch;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			uid = (uid_t)parse_id(arg1, "uid");
			have_uid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "Cq")) != -1) {
		if (ch == 'q')
			quiet = true;
		else
			exec_bsd();
	}
	if (name == NULL) {
		if (!have_uid)
			exec_bsd();
		if (ds_route_uid(uid, namebuf, sizeof(namebuf)) != DS_WHERE_PLIST)
			exec_bsd();
		name = namebuf;
	} else if (ds_route_user(name) != DS_WHERE_PLIST)
		exec_bsd();

	db_open(&db, true);
	u = ds_user_find(&db.users, name);
	if (u == NULL)
		errx(EX_NOUSER, "user '%s' does not exist", name);
	if (lock) {
		if (u->hash != NULL && strncmp(u->hash, LOCKED_PREFIX,
		    strlen(LOCKED_PREFIX)) == 0)
			errx(EX_DATAERR, "user '%s' is already locked", name);
		if (asprintf(&nh, "%s%s", LOCKED_PREFIX,
		    u->hash != NULL ? u->hash : "") == -1)
			err(EX_OSERR, "asprintf");
		free(u->hash);
		u->hash = nh;
		u->nopass = false;
	} else {
		if (u->hash == NULL || strncmp(u->hash, LOCKED_PREFIX,
		    strlen(LOCKED_PREFIX)) != 0)
			errx(EX_DATAERR, "user '%s' is not locked", name);
		nh = strdup(u->hash + strlen(LOCKED_PREFIX));
		if (nh == NULL)
			err(EX_OSERR, "strdup");
		free(u->hash);
		u->hash = nh[0] != '\0' ? nh : NULL;
		if (nh[0] == '\0')
			free(nh);
	}
	db_save(&db, true, false);
	db_close(&db);
	return (0);
}

/* ---- group commands ---------------------------------------------------- */

static void
add_members(struct db *db, struct ds_group *g, const char *list)
{
	char *copy, *p, *tok;

	copy = strdup(list);
	if (copy == NULL)
		err(EX_OSERR, "strdup");
	p = copy;
	while ((tok = strsep(&p, ",")) != NULL) {
		if (tok[0] == '\0')
			continue;
		if (!user_exists_anywhere(db, tok))
			errx(EX_NOUSER, "user '%s' does not exist", tok);
		if (ds_group_add_member(g, tok) == -1)
			err(EX_OSERR, "add %s", tok);
	}
	free(copy);
}

static void
del_members(struct ds_group *g, const char *list)
{
	char *copy, *p, *tok;

	copy = strdup(list);
	if (copy == NULL)
		err(EX_OSERR, "strdup");
	p = copy;
	while ((tok = strsep(&p, ",")) != NULL)
		if (tok[0] != '\0')
			(void)ds_group_remove_member(g, tok);
	free(copy);
}

static void
clear_members(struct ds_group *g)
{
	while (g->nmembers > 0)
		(void)ds_group_remove_member(g, g->members[0]);
}

static int
group_add(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_group *g;
	const char *name = NULL, *members = NULL;
	gid_t gid = 0;
	bool have_gid = false, dup_ok = false;
	int ch;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			gid = (gid_t)parse_id(arg1, "gid");
			have_gid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:g:h:H:M:oNPY")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'g': gid = (gid_t)parse_id(optarg, "gid"); have_gid = true; break;
		case 'M': members = optarg; break;
		case 'o': dup_ok = true; break;
		case 'q': quiet = true; break;
		case 'C': case 'N': case 'Y':
			exec_bsd();
			/* NOTREACHED */
		case 'P': break;
		case 'h': case 'H':
			unsupported(ch, "group");
			/* NOTREACHED */
		default:
			exec_bsd();
		}
	}
	if (name == NULL)
		exec_bsd();
	if (ds_route_new_group(have_gid, gid, name) == DS_WHERE_SYSTEM)
		exec_bsd();
	if (!ds_valid_name(name))
		errx(EX_USAGE, "invalid group name '%s'", name);

	db_open(&db, true);
	if (ds_group_find(&db.groups, name) != NULL ||
	    ds_system_group(name, 0, NULL, 0, NULL) == 0)
		errx(EX_DATAERR, "group '%s' already exists", name);
	if (have_gid) {
		if (!dup_ok && (ds_group_find_gid(&db.groups, gid) != NULL ||
		    ds_system_group(NULL, gid, NULL, 0, NULL) == 0))
			errx(EX_DATAERR, "gid %lu is already in use (-o allows it)",
			    (unsigned long)gid);
	} else
		gid = (gid_t)ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
	g = ds_group_add(&db.groups, name, gid);
	if (g == NULL)
		err(EX_OSERR, "add %s", name);
	if (members != NULL)
		add_members(&db, g, members);
	db_save(&db, false, true);
	db_close(&db);
	return (0);
}

static struct ds_group *
find_group_arg(struct db *db, const char *name, bool have_gid, gid_t gid)
{
	struct ds_group *g;

	g = name != NULL ? ds_group_find(&db->groups, name) :
	    (have_gid ? ds_group_find_gid(&db->groups, gid) : NULL);
	if (g == NULL)
		errx(EX_NOUSER, "group does not exist");
	return (g);
}

static void
route_group_arg(const char *name, bool have_gid, gid_t gid)
{
	char namebuf[64];

	if (name != NULL) {
		if (ds_route_group(name) != DS_WHERE_PLIST)
			exec_bsd();
	} else if (!have_gid ||
	    ds_route_gid(gid, namebuf, sizeof(namebuf)) != DS_WHERE_PLIST)
		exec_bsd();
}

static int
group_mod(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_group *g;
	const char *name = NULL, *newname = NULL, *setm = NULL, *addm = NULL;
	const char *delm = NULL;
	gid_t gid = 0, newgid = 0;
	bool have_gid = false, have_newgid = false;
	int ch;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			gid = (gid_t)parse_id(arg1, "gid");
			have_gid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:d:g:l:h:H:M:m:NPY")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'g':
			if (name == NULL && !have_gid) {
				gid = (gid_t)parse_id(optarg, "gid");
				have_gid = true;
			} else {
				newgid = (gid_t)parse_id(optarg, "gid");
				have_newgid = true;
			}
			break;
		case 'l': newname = optarg; break;
		case 'M': setm = optarg; break;
		case 'm': addm = optarg; break;
		case 'd': delm = optarg; break;
		case 'q': quiet = true; break;
		case 'C': case 'N': case 'Y':
			exec_bsd();
			/* NOTREACHED */
		case 'P': break;
		case 'h': case 'H':
			unsupported(ch, "group");
			/* NOTREACHED */
		default:
			exec_bsd();
		}
	}
	route_group_arg(name, have_gid, gid);
	db_open(&db, true);
	g = find_group_arg(&db, name, have_gid, gid);
	if (have_newgid) {
		if (ds_group_find_gid(&db.groups, newgid) != NULL ||
		    ds_system_group(NULL, newgid, NULL, 0, NULL) == 0)
			errx(EX_DATAERR, "gid %lu is already in use", (unsigned long)newgid);
		g->gid = newgid;
	}
	if (newname != NULL) {
		if (!ds_valid_name(newname))
			errx(EX_USAGE, "invalid group name '%s'", newname);
		if (ds_group_find(&db.groups, newname) != NULL ||
		    ds_system_group(newname, 0, NULL, 0, NULL) == 0)
			errx(EX_DATAERR, "group '%s' already exists", newname);
		if (ds_set_string(&g->name, newname) == -1)
			err(EX_OSERR, "rename");
	}
	if (setm != NULL) {
		clear_members(g);
		add_members(&db, g, setm);
	}
	if (addm != NULL)
		add_members(&db, g, addm);
	if (delm != NULL)
		del_members(g, delm);
	db_save(&db, false, true);
	db_close(&db);
	return (0);
}

static int
group_del(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_group *g;
	const char *name = NULL;
	gid_t gid = 0;
	bool have_gid = false;
	size_t i;
	int ch;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			gid = (gid_t)parse_id(arg1, "gid");
			have_gid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:g:Y")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'g': gid = (gid_t)parse_id(optarg, "gid"); have_gid = true; break;
		case 'q': quiet = true; break;
		default:
			exec_bsd();
		}
	}
	route_group_arg(name, have_gid, gid);
	db_open(&db, true);
	g = find_group_arg(&db, name, have_gid, gid);
	for (i = 0; i < db.users.n; i++)
		if (db.users.v[i].gid == g->gid)
			errx(EX_DATAERR, "group '%s' is the primary group of %s",
			    g->name, db.users.v[i].name);
	if (ds_group_remove(&db.groups, g->name) == -1)
		err(EX_OSERR, "remove group");
	db_save(&db, false, true);
	db_close(&db);
	return (0);
}

static void
print_group(const struct ds_group *g, bool pretty)
{
	size_t i;

	if (pretty) {
		printf("Group Name: %-15s   #%lu\n   Members: ", g->name,
		    (unsigned long)g->gid);
	} else
		printf("%s:x:%lu:", g->name, (unsigned long)g->gid);
	for (i = 0; i < g->nmembers; i++)
		printf("%s%s", i > 0 ? "," : "", g->members[i]);
	printf("\n");
}

static int
group_show(int argc, char *argv[], const char *arg1)
{
	struct db db;
	struct ds_group *g;
	const char *name = NULL;
	gid_t gid = 0;
	bool have_gid = false, all = false, pretty = false;
	size_t i;
	int ch;

	if (arg1 != NULL) {
		if (all_digits(arg1)) {
			gid = (gid_t)parse_id(arg1, "gid");
			have_gid = true;
		} else
			name = arg1;
	}
	optind = 1;
	while ((ch = getopt(argc, argv, "C:qn:g:FPa")) != -1) {
		switch (ch) {
		case 'n': name = optarg; break;
		case 'g': gid = (gid_t)parse_id(optarg, "gid"); have_gid = true; break;
		case 'a': all = true; break;
		case 'P': pretty = true; break;
		case 'F': case 'q': break;
		default:
			exec_bsd();
		}
	}
	if (all) {
		db_open(&db, false);
		for (i = 0; i < db.groups.n; i++)
			print_group(&db.groups.v[i], pretty);
		db_close(&db);
		fflush(stdout);
		exec_bsd();
	}
	route_group_arg(name, have_gid, gid);
	db_open(&db, false);
	g = find_group_arg(&db, name, have_gid, gid);
	print_group(g, pretty);
	db_close(&db);
	return (0);
}

static int
group_next(void)
{
	struct db db;
	gid_t id;

	db_open(&db, false);
	id = (gid_t)ds_next_id(&db.users, &db.groups, DS_FIRST_ID);
	db_close(&db);
	printf("%lu\n", (unsigned long)id);
	return (0);
}

/* ---- main -------------------------------------------------------------- */

static int
index_of(const char *const *list, size_t n, const char *word)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (strcmp(list[i], word) == 0)
			return ((int)i);
	return (-1);
}

int
main(int argc, char *argv[])
{
	enum which which = W_NONE;
	enum mode mode = M_NONE;
	const char *arg1 = NULL;
	size_t i;
	int idx;

	progname = "pw";
	orig_argv = argv;

	/*
	 * The leading words: global options, then user|group and add|del|...,
	 * or a combined word, then an optional name or id, exactly as
	 * FreeBSD's pw takes them. Anything that names an alternate root,
	 * config or metalog, a dry run or NIS is FreeBSD's business.
	 */
	while (argc > 1) {
		if (argv[1][0] == '-') {
			switch (argv[1][1]) {
			case 'V': case 'R': case 'M': case 'C': case 'N': case 'Y':
				exec_bsd();
				/* NOTREACHED */
			case 'q':
				quiet = true;
				break;
			default:
				exec_bsd();
			}
		} else if (mode == M_NONE && (idx = index_of(modes, 7, argv[1])) != -1)
			mode = (enum mode)idx;
		else if (which == W_NONE && strcmp(argv[1], "user") == 0)
			which = W_USER;
		else if (which == W_NONE && strcmp(argv[1], "group") == 0)
			which = W_GROUP;
		else if (mode == M_NONE && which == W_NONE) {
			for (i = 0; i < 12; i++)
				if (strcmp(combos[i][0], argv[1]) == 0 ||
				    strcmp(combos[i][1], argv[1]) == 0)
					break;
			if (i == 12)
				exec_bsd();	/* "help", or an unknown word */
			which = i < 7 ? W_USER : W_GROUP;
			mode = (enum mode)(i < 7 ? i : i - 7);
		} else if (which != W_NONE && mode != M_NONE) {
			arg1 = argv[1];
		} else
			exec_bsd();
		++argv;
		--argc;
	}
	if (mode == M_NONE || which == W_NONE)
		exec_bsd();
	(void)quiet;

	if (which == W_USER) {
		switch (mode) {
		case M_ADD:	return (user_add(argc, argv, arg1));
		case M_DEL:	return (user_del(argc, argv, arg1));
		case M_MOD:	return (user_mod(argc, argv, arg1));
		case M_SHOW:	return (user_show(argc, argv, arg1));
		case M_NEXT:	return (user_next());
		case M_LOCK:	return (user_lock(argc, argv, arg1, true));
		case M_UNLOCK:	return (user_lock(argc, argv, arg1, false));
		default:	break;
		}
	} else {
		switch (mode) {
		case M_ADD:	return (group_add(argc, argv, arg1));
		case M_DEL:	return (group_del(argc, argv, arg1));
		case M_MOD:	return (group_mod(argc, argv, arg1));
		case M_SHOW:	return (group_show(argc, argv, arg1));
		case M_NEXT:	return (group_next());
		default:	break;
		}
	}
	exec_bsd();
	/* NOTREACHED */
	return (EX_USAGE);
}
