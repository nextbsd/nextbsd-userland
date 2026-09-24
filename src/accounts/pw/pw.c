/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * pw — account and group maintenance, over the DirectoryServices plists or
 * over master.passwd (nextbsd/nextbsd-userland#255, E18 U9).
 *
 * This one has the least design freedom of the set, because its caller is
 * not a person. Ports install scripts generated from the framework's
 * do-users-groups.sh emit exactly five forms, and every one of them has to
 * keep working byte for byte:
 *
 *     pw usershow  LOGIN
 *     pw useradd   LOGIN -u UID -g GID [-L CLASS] -c "GECOS" -d HOME -s SH
 *     pw groupshow GROUP
 *     pw groupadd  GROUP -g GID
 *     pw groupmod  GROUP -m LOGIN
 *
 * Three things about those lines drive the whole design. The name is
 * positional, never -n, so anything that assumes -n breaks every port that
 * creates a user. -L appears in real invocations, so a system account has to
 * tolerate a login class even though our records have no such field. And the
 * show verbs are used as existence tests, so their exit status and output
 * format are load bearing rather than cosmetic.
 *
 * The way all three are satisfied is by not reimplementing pw. Everything
 * that belongs in master.passwd is handed to the copy the base ships,
 * the vendored copy under bsd/, with the original argv untouched. Since
 * every account a port creates is a system account, the packaging contract
 * is met by delegation rather than by imitation, which is the only way to be
 * sure it stays met.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "acct.h"
#include "libds.h"

/*
 * A system account is handled by the pw FreeBSD ships, vendored under bsd/ and
 * entered as a function rather than exec'd as a second binary. Every account a
 * port creates is a system account, so this path is the packaging contract:
 * it behaves exactly as it always has because it is the same code.
 *
 * Not built on Darwin -- it needs libutil's pw_* and login_cap, neither of
 * which exists there. The master.passwd half is unreachable on Darwin anyway,
 * so the stub only keeps the file compiling for a host syntax check.
 */
#ifdef __FreeBSD__
int pw_bsd_main(int, char **);
#else
static int
pw_bsd_main(int argc __unused, char **argv __unused)
{
	warnx("system accounts are not supported on this platform");
	return (EX_UNAVAILABLE);
}
#endif

enum verb {
	V_UNKNOWN,
	V_USERADD, V_USERDEL, V_USERMOD, V_USERSHOW, V_USERNEXT,
	V_GROUPADD, V_GROUPDEL, V_GROUPMOD, V_GROUPSHOW, V_GROUPNEXT,
	V_LOCK, V_UNLOCK
};

static const struct {
	const char	*name;
	enum verb	 verb;
} verbs[] = {
	{ "useradd", V_USERADD },   { "userdel", V_USERDEL },
	{ "usermod", V_USERMOD },   { "usershow", V_USERSHOW },
	{ "usernext", V_USERNEXT },
	{ "groupadd", V_GROUPADD }, { "groupdel", V_GROUPDEL },
	{ "groupmod", V_GROUPMOD }, { "groupshow", V_GROUPSHOW },
	{ "groupnext", V_GROUPNEXT },
	{ "lock", V_LOCK },         { "unlock", V_UNLOCK },
	{ NULL, V_UNKNOWN }
};

static bool	 verb_is_group(enum verb v);

/* Set only by the test hook below; always false in a shipped build. */
static bool	 pw_test_root;

static enum verb
verb_of(const char *s)
{
	size_t i;

	for (i = 0; verbs[i].name != NULL; i++)
		if (strcmp(s, verbs[i].name) == 0)
			return (verbs[i].verb);
	return (V_UNKNOWN);
}

static bool
verb_is_group(enum verb v)
{
	switch (v) {
	case V_GROUPADD: case V_GROUPDEL: case V_GROUPMOD:
	case V_GROUPSHOW: case V_GROUPNEXT:
		return (true);
	default:
		return (false);
	}
}

/* Hand the whole invocation to the copy the base ships. */
static int
delegate(int argc, char *argv[])
{
#ifdef PW_TEST
	/*
	 * The vendored half needs libutil and does not build on the host, so
	 * what the suite checks here is the routing decision: record the
	 * command line that would have been handed over, and say no more.
	 * Whether pw itself does the right thing with it is FreeBSD's own
	 * test suite's business, and the on-image lifecycle's.
	 */
	{
		const char *rec = getenv("PW_DELEGATED_TO");
		FILE *f;
		int i;

		if (rec != NULL && rec[0] != '\0' &&
		    (f = fopen(rec, "w")) != NULL) {
			for (i = 1; i < argc; i++)
				(void)fprintf(f, "%s%s", i > 1 ? " " : "",
				    argv[i]);
			(void)fputc('\n', f);
			(void)fclose(f);
		}
		return (0);
	}
#else
	/*
	 * The vendored parser scans the command line itself, and this process
	 * has already scanned it once: optreset as well as optind, or getopt
	 * resumes mid-line.
	 */
	optreset = 1;
	optind = 1;
	return (pw_bsd_main(argc, argv));
#endif
}

/*
 * The flags we care about for routing and for the plist path. Everything
 * else is passed through untouched when we delegate, which is why this does
 * not try to be a complete parser.
 */
struct pwargs {
	const char	*name;		/* positional, or -n */
	const char	*gecos;		/* -c */
	const char	*shell;		/* -s */
	const char	*home;		/* -d */
	const char	*class;		/* -L */
	const char	*members;	/* -m / -d for groupmod */
	const char	*delmember;
	const char	*newname;	/* -l */
	long long	 uid;		/* -u */
	long long	 gid;		/* -g */
	bool		 have_uid, have_gid;
	bool		 remove_home;	/* -r */
	bool		 quiet;		/* -q */
	bool		 pretty;	/* -P */
};

static void
parse(int argc, char *argv[], struct pwargs *a)
{
	int i;

	memset(a, 0, sizeof(*a));
	a->uid = a->gid = -1;
	for (i = 0; i < argc; i++) {
		const char *s = argv[i];
		const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

		if (s[0] != '-') {
			if (a->name == NULL)
				a->name = s;	/* positional, as ports emit */
			continue;
		}
		if (strcmp(s, "-n") == 0 && val != NULL) { a->name = val; i++; }
		else if (strcmp(s, "-c") == 0 && val != NULL) { a->gecos = val; i++; }
		else if (strcmp(s, "-s") == 0 && val != NULL) { a->shell = val; i++; }
		else if (strcmp(s, "-d") == 0 && val != NULL) { a->home = val; a->delmember = val; i++; }
		else if (strcmp(s, "-L") == 0 && val != NULL) { a->class = val; i++; }
		else if (strcmp(s, "-m") == 0 && val != NULL) { a->members = val; i++; }
		else if (strcmp(s, "-l") == 0 && val != NULL) { a->newname = val; i++; }
		else if (strcmp(s, "-u") == 0 && val != NULL) { a->uid = strtoll(val, NULL, 10); a->have_uid = true; i++; }
		else if (strcmp(s, "-g") == 0 && val != NULL) { a->gid = strtoll(val, NULL, 10); a->have_gid = true; i++; }
		else if (strcmp(s, "-r") == 0) a->remove_home = true;
		else if (strcmp(s, "-q") == 0) a->quiet = true;
		else if (strcmp(s, "-P") == 0) a->pretty = true;
	}
}

/*
 * Where this operation belongs, in the order the design settles it: an
 * existing account goes wherever it already lives, and only a new one is
 * decided by its shape.
 */
static bool
route_to_files(enum verb v, const struct pwargs *a, struct ds_handle *h)
{
	struct ds_userrec u;
	struct ds_grouprec g;

	if (a->name == NULL)
		return (false);

	if (verb_is_group(v)) {
		if (ds_group_get(h, a->name, &g) == DS_OK)
			return (false);		/* exists in the plists */
		if (getgrnam(a->name) != NULL)
			return (true);		/* exists in /etc/group */
	} else {
		if (ds_user_get(h, a->name, &u) == DS_OK)
			return (false);
		if (getpwnam(a->name) != NULL)
			return (true);
	}

	/* New. An id in the system range is a system account. */
	if (a->have_uid && a->uid >= 0 && a->uid <= ACCT_SYSTEM_MAX)
		return (true);
	if (verb_is_group(v) && a->have_gid && a->gid >= 0 &&
	    a->gid <= ACCT_SYSTEM_MAX)
		return (true);
	if (acct_is_system_name(a->name))
		return (true);
	/*
	 * A service account with no login and nowhere to live, which is what
	 * most ports create even when they pick a high id.
	 */
	if (a->shell != NULL && strstr(a->shell, "nologin") != NULL &&
	    (a->home == NULL || a->home[0] == '\0' ||
	     strcmp(a->home, "/nonexistent") == 0))
		return (true);
	return (false);
}

/* passwd(5)-shaped line, which is what the show verbs are grepped for. */
static void
show_user(const struct ds_userrec *u, bool pretty)
{
	if (pretty) {
		(void)printf("Login Name: %-10s   #%-5u Group: %-10u\n",
		    u->username, (unsigned)u->uid, (unsigned)u->gid);
		(void)printf("  Full Name: %s\n", u->realName);
		(void)printf("       Home: %s/%s\n", ACCT_LOCAL_USERS,
		    u->username);
		(void)printf("      Shell: %s\n", u->shell);
		return;
	}
	(void)printf("%s:*:%u:%u::0:0:%s:%s/%s:%s\n", u->username,
	    (unsigned)u->uid, (unsigned)u->gid, u->realName,
	    ACCT_LOCAL_USERS, u->username, u->shell);
}

static void
show_group(struct ds_handle *h, const struct ds_grouprec *g, bool pretty)
{
	size_t n, i;
	char m[DS_NAME_MAX];
	char list[4096];

	list[0] = '\0';
	n = ds_group_member_count(h, g->groupname);
	for (i = 0; i < n; i++) {
		if (ds_group_member_at(h, g->groupname, i, m, sizeof(m)) != DS_OK)
			continue;
		if (list[0] != '\0')
			(void)strlcat(list, ",", sizeof(list));
		(void)strlcat(list, m, sizeof(list));
	}
	if (pretty) {
		(void)printf("Group Name: %-10s   #%-5u\n", g->groupname,
		    (unsigned)g->gid);
		(void)printf("   Members: %s\n", list);
		return;
	}
	/* group(5): name:passwd:gid:members — grepped for a member name. */
	(void)printf("%s:*:%u:%s\n", g->groupname, (unsigned)g->gid, list);
}

int
main(int argc, char *argv[])
{
	struct ds_handle *h = NULL;
	struct ds_userrec u;
	struct ds_grouprec g;
	struct pwargs a;
	char server[256];
	enum verb v;
	enum ds_error err;
	int rest_argc, rc = EX_OK;
	char **rest_argv;

	if (argc < 2) {
		(void)fprintf(stderr,
		    "usage: pw [user|group][add|del|mod|show|next] ...\n");
		return (EX_USAGE);
	}

	/* "pw useradd" and "pw user add" are both accepted. */
	v = verb_of(argv[1]);
	rest_argc = argc - 2;
	rest_argv = argv + 2;
	if (v == V_UNKNOWN && argc >= 3 &&
	    (strcmp(argv[1], "user") == 0 || strcmp(argv[1], "group") == 0)) {
		char joined[32];

		(void)snprintf(joined, sizeof(joined), "%s%s", argv[1], argv[2]);
		v = verb_of(joined);
		rest_argc = argc - 3;
		rest_argv = argv + 3;
	}
	if (v == V_UNKNOWN)
		return (delegate(argc, argv));

	parse(rest_argc, rest_argv, &a);

	/*
	 * Test hook, with its own macro for the reason given in adduser: a
	 * flag that points this at a fixture and lets it believe it is root
	 * must not be reachable through a macro another component might
	 * define. Nothing in the shipped build defines PW_TEST.
	 */
#ifdef PW_TEST
	{
		const char *fixture = getenv("NEXTBSD_DS_DIR");

		if (fixture != NULL && fixture[0] != '\0') {
			ds_set_dirs(fixture, NULL);
			pw_test_root = true;
		}
	}
#endif

	/*
	 * usernext and groupnext report the next free id and belong to the
	 * base copy: its answer covers master.passwd, which is where a caller
	 * asking pw for an id is going to put the account.
	 */
	if (v == V_USERNEXT || v == V_GROUPNEXT)
		return (delegate(argc, argv));

	if ((err = ds_open(DS_LOCAL, DS_RDWR, &h)) != DS_OK) {
		if (err == DS_ELOCK) {
			warnx("the directory is locked; another account tool "
			    "may be running");
			return (EX_TEMPFAIL);
		}
		warnx("%s", ds_strerror(err));
		return (EX_DATAERR);
	}

	if (route_to_files(v, &a, h)) {
		ds_close(h);
		return (delegate(argc, argv));
	}

	/* From here the operation is ours, so refuse what we cannot store. */
	if (a.class != NULL) {
		warnx("-L is a login class; NextBSD directory accounts have no "
		    "class field");
		ds_close(h);
		return (EX_USAGE);
	}
	if (a.home != NULL && v != V_GROUPMOD) {
		warnx("-d is a home directory; a directory account's home is "
		    "always %s/<name>", ACCT_LOCAL_USERS);
		ds_close(h);
		return (EX_USAGE);
	}
	if (v != V_USERSHOW && v != V_GROUPSHOW) {
		if (geteuid() != 0 && !pw_test_root) {
			warnx("only root may change accounts");
			ds_close(h);
			return (EX_NOPERM);
		}
		if (acct_bound_server(server, sizeof(server))) {
			if (server[0] != '\0')
				warnx("this machine is joined to %s; change "
				    "the account there", server);
			else
				warnx("this machine is joined to a directory "
				    "server; change the account there");
			ds_close(h);
			return (EX_NOPERM);
		}
	}

	switch (v) {
	case V_USERSHOW:
		if (a.name == NULL || ds_user_get(h, a.name, &u) != DS_OK) {
			warnx("no such user `%s'", a.name ? a.name : "");
			rc = EX_NOUSER;
			break;
		}
		show_user(&u, a.pretty);
		break;

	case V_GROUPSHOW:
		if (a.name == NULL || ds_group_get(h, a.name, &g) != DS_OK) {
			warnx("no such group `%s'", a.name ? a.name : "");
			rc = EX_NOUSER;
			break;
		}
		show_group(h, &g, a.pretty);
		break;

	case V_USERADD:
		if (ds_user_get(h, a.name, &u) == DS_OK) {
			warnx("login name `%s' already exists", a.name);
			rc = EX_DATAERR;
			break;
		}
		memset(&u, 0, sizeof(u));
		(void)strlcpy(u.username, a.name, sizeof(u.username));
		if (a.gecos != NULL)
			(void)strlcpy(u.realName, a.gecos, sizeof(u.realName));
		(void)strlcpy(u.shell, a.shell != NULL ? a.shell :
		    ACCT_DEFAULT_SHELL, sizeof(u.shell));
		if (a.have_uid)
			u.uid = (uid_t)a.uid;
		else if (ds_user_next_uid(h, ACCT_FIRST_ID, ACCT_LAST_ID, true,
		    &u.uid) != DS_OK) {
			warnx("no free uid");
			rc = EX_UNAVAILABLE;
			break;
		}
		u.gid = a.have_gid ? (gid_t)a.gid : (gid_t)u.uid;
		u.noPassword = false;
		(void)strlcpy(u.passwordHash, "*", sizeof(u.passwordHash));
		u.hasHash = true;
		if ((err = ds_user_add(h, &u)) != DS_OK) {
			warnx("%s", ds_strerror(err));
			rc = EX_IOERR;
		}
		break;

	case V_USERDEL:
		if (ds_user_del(h, a.name) != DS_OK) {
			warnx("no such user `%s'", a.name);
			rc = EX_NOUSER;
			break;
		}
		if (a.remove_home)
			(void)acct_remove_home(a.name);
		break;

	case V_USERMOD:
		if (ds_user_get(h, a.name, &u) != DS_OK) {
			warnx("no such user `%s'", a.name);
			rc = EX_NOUSER;
			break;
		}
		if (a.newname != NULL) {
			warnx("-l renames an account; that is not supported "
			    "for a directory account");
			rc = EX_USAGE;
			break;
		}
		if (a.gecos != NULL)
			(void)strlcpy(u.realName, a.gecos, sizeof(u.realName));
		if (a.shell != NULL)
			(void)strlcpy(u.shell, a.shell, sizeof(u.shell));
		if (a.have_uid) u.uid = (uid_t)a.uid;
		if (a.have_gid) u.gid = (gid_t)a.gid;
		if ((err = ds_user_set(h, &u)) != DS_OK) {
			warnx("%s", ds_strerror(err));
			rc = EX_IOERR;
		}
		break;

	case V_LOCK:
	case V_UNLOCK:
		if (ds_user_get(h, a.name, &u) != DS_OK) {
			warnx("no such user `%s'", a.name);
			rc = EX_NOUSER;
			break;
		}
		/*
		 * The same sentinel passwd(1) uses, so the two agree and a
		 * lock set by either is understood by the other.
		 */
		if (v == V_LOCK) {
			char tmp[DS_HASH_MAX];

			if (acct_hash_locked(u.passwordHash))
				break;		/* already locked */
			(void)snprintf(tmp, sizeof(tmp), "%s%s",
			    ACCT_LOCK_PREFIX, u.hasHash ? u.passwordHash : "");
			(void)strlcpy(u.passwordHash, tmp, sizeof(u.passwordHash));
			u.hasHash = true;
			u.noPassword = false;
		} else {
			if (!acct_hash_locked(u.passwordHash))
				break;		/* not locked */
			(void)memmove(u.passwordHash,
			    u.passwordHash + sizeof(ACCT_LOCK_PREFIX) - 1,
			    strlen(u.passwordHash) -
			    (sizeof(ACCT_LOCK_PREFIX) - 1) + 1);
			if (u.passwordHash[0] == '\0') {
				u.hasHash = false;
				u.noPassword = true;
			}
		}
		if ((err = ds_user_set(h, &u)) != DS_OK) {
			warnx("%s", ds_strerror(err));
			rc = EX_IOERR;
		}
		break;

	case V_GROUPADD:
		if (ds_group_get(h, a.name, &g) == DS_OK) {
			warnx("group `%s' already exists", a.name);
			rc = EX_DATAERR;
			break;
		}
		memset(&g, 0, sizeof(g));
		(void)strlcpy(g.groupname, a.name, sizeof(g.groupname));
		g.gid = a.have_gid ? (gid_t)a.gid : (gid_t)ACCT_FIRST_ID;
		if ((err = ds_group_add(h, &g)) != DS_OK) {
			warnx("%s", ds_strerror(err));
			rc = EX_IOERR;
		}
		break;

	case V_GROUPDEL:
		if (ds_group_del(h, a.name) != DS_OK) {
			warnx("no such group `%s'", a.name);
			rc = EX_NOUSER;
		}
		break;

	case V_GROUPMOD:
		if (ds_group_get(h, a.name, &g) != DS_OK) {
			warnx("no such group `%s'", a.name);
			rc = EX_NOUSER;
			break;
		}
		if (a.members != NULL) {
			char list[1024], *tok, *brk;

			(void)strlcpy(list, a.members, sizeof(list));
			for (tok = strtok_r(list, ",", &brk); tok != NULL;
			    tok = strtok_r(NULL, ",", &brk))
				if ((err = ds_group_add_member(h, a.name,
				    tok)) != DS_OK) {
					warnx("%s", ds_strerror(err));
					rc = EX_IOERR;
				}
		}
		if (a.delmember != NULL) {
			char list[1024], *tok, *brk;

			(void)strlcpy(list, a.delmember, sizeof(list));
			for (tok = strtok_r(list, ",", &brk); tok != NULL;
			    tok = strtok_r(NULL, ",", &brk))
				(void)ds_group_del_member(h, a.name, tok);
		}
		if (a.have_gid) {
			warnx("changing a group's gid is not supported for a "
			    "directory group");
			rc = EX_USAGE;
		}
		break;

	default:
		ds_close(h);
		return (delegate(argc, argv));
	}

	if (rc == EX_OK && v != V_USERSHOW && v != V_GROUPSHOW &&
	    (err = ds_commit(h)) != DS_OK) {
		warnx("could not write the directory: %s", ds_strerror(err));
		rc = EX_IOERR;
	}
	ds_close(h);
	return (rc);
}
