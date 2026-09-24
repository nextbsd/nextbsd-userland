/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * rmuser — remove a user account from the DirectoryServices plists
 * (nextbsd/nextbsd-userland#255, E18 U9).
 *
 * The counterpart to adduser(8), and the same division: this removes
 * regular accounts from the plists. System accounts stay in master.passwd
 * and are removed with pw(8).
 *
 * Home removal is prompted for, defaulting to yes, which is between the two
 * systems we take from: one prompts and defaults to no, the other removes
 * without asking. Defaulting to yes matches what someone removing an
 * account almost always means, while still letting them object. --keep-home
 * covers scripts that want the files left alone, and -y answers every
 * prompt for the ones that do not want to be asked.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "acct.h"
#include "libds.h"

#define EX_OK		0
#define EX_USAGE	1
#define EX_REFUSED	2
#define EX_DATABASE	3
#define EX_CANCELLED	4

struct opts {
	const char	*batch;		/* -f */
	bool		 yes;		/* -y */
	bool		 verbose;	/* -v */
	bool		 keep_home;	/* --keep-home */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: rmuser [-yvh] [--keep-home] [-f file] [name ...]\n");
}

static int
ask_yesno(const char *prompt, bool dflt)
{
	char buf[64];

	for (;;) {
		(void)printf("%s (yes/no) [%s]: ", prompt, dflt ? "yes" : "no");
		(void)fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) == NULL)
			return (-1);
		buf[strcspn(buf, "\r\n")] = '\0';
		if (buf[0] == '\0')
			return (dflt ? 1 : 0);
		if (strcasecmp(buf, "y") == 0 || strcasecmp(buf, "yes") == 0)
			return (1);
		if (strcasecmp(buf, "n") == 0 || strcasecmp(buf, "no") == 0)
			return (0);
		(void)printf("Please answer yes or no.\n");
	}
}

/*
 * Would removing this user leave the admin group empty? With root shipping
 * disabled, that locks the machine out of sudo entirely and leaves only
 * single-user mode, so it is refused unless the operator insists.
 */
static bool
last_admin(struct ds_handle *h, const char *name)
{
	size_t n, i;
	char member[DS_NAME_MAX];
	bool found_other = false;

	n = ds_group_member_count(h, ACCT_ADMIN_GROUP);
	for (i = 0; i < n; i++) {
		if (ds_group_member_at(h, ACCT_ADMIN_GROUP, i, member,
		    sizeof(member)) != DS_OK)
			continue;
		if (strcmp(member, name) != 0)
			found_other = true;
	}
	/* Only "last" if they are in it at all and nobody else is. */
	return (!found_other && n > 0);
}

/* Drop the user from every group in the plists. */
static enum ds_error
drop_plist_groups(struct ds_handle *h, const char *name, bool verbose)
{
	struct ds_grouprec g;
	size_t n, i;
	bool member;
	enum ds_error err;

	n = ds_group_count(h);
	for (i = 0; i < n; i++) {
		if (ds_group_at(h, i, &g) != DS_OK)
			continue;
		if (ds_group_has_member(h, g.groupname, name, &member) != DS_OK ||
		    !member)
			continue;
		if ((err = ds_group_del_member(h, g.groupname, name)) != DS_OK)
			return (err);
		if (verbose)
			(void)printf("  dropped from group %s\n", g.groupname);
	}
	return (DS_OK);
}

/*
 * Groups in /etc/group are pw(8)'s to edit, so ask pw rather than rewriting
 * that file here. A user only lands there by hand or by a port, but leaving
 * a dangling member behind would be untidy and confusing later.
 */
static void
drop_etc_groups(const char *name, bool verbose)
{
	struct group *gr;
	char **m;
	pid_t pid;
	int status;

	setgrent();
	while ((gr = getgrent()) != NULL) {
		bool found = false;

		for (m = gr->gr_mem; m != NULL && *m != NULL; m++)
			if (strcmp(*m, name) == 0) {
				found = true;
				break;
			}
		if (!found)
			continue;
		if ((pid = fork()) == 0) {
			(void)execl("/usr/sbin/pw", "pw", "groupmod",
			    gr->gr_name, "-d", name, (char *)NULL);
			_exit(127);
		}
		if (pid > 0) {
			while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
				;
			if (verbose)
				(void)printf("  dropped from /etc/group group "
				    "%s\n", gr->gr_name);
		}
	}
	endgrent();
}

static int
remove_one(const struct opts *o, const char *name, bool interactive)
{
	struct ds_handle *h;
	struct ds_userrec u;
	struct ds_grouprec pg;
	char server[256];
	enum ds_error err;
	int sessions, killed, rc;
	bool drop_home;

	if (strcmp(name, "root") == 0) {
		warnx("root: cannot remove root");
		return (EX_REFUSED);
	}
	if (acct_is_system_name(name)) {
		warnx("%s: system account; use pw userdel", name);
		return (EX_REFUSED);
	}
	if (acct_bound_server(server, sizeof(server))) {
		if (server[0] != '\0')
			warnx("this machine is joined to %s; remove the "
			    "account on the server", server);
		else
			warnx("this machine is joined to a directory server; "
			    "remove the account there");
		return (EX_REFUSED);
	}

	if ((err = ds_open(DS_LOCAL, DS_RDWR, &h)) != DS_OK) {
		warnx("%s", err == DS_ELOCK ?
		    "the directory is locked; another account tool may be "
		    "running" : ds_strerror(err));
		return (EX_DATABASE);
	}
	if (ds_user_get(h, name, &u) != DS_OK) {
		warnx("%s: no such user", name);
		ds_close(h);
		return (EX_REFUSED);
	}
	if (acct_is_system_id(u.uid)) {
		warnx("%s: uid %u is a system account; use pw userdel", name,
		    (unsigned)u.uid);
		ds_close(h);
		return (EX_REFUSED);
	}
	if (last_admin(h, name) && !o->yes) {
		warnx("%s: refusing to remove the last member of the %s group "
		    "(use -y to force)", name, ACCT_ADMIN_GROUP);
		ds_close(h);
		return (EX_REFUSED);
	}

	/* Show what is about to go, the way rmuser has always done. */
	if (interactive) {
		(void)printf("Matching password entry:\n\n");
		(void)printf("%s:*:%u:%u:%s:%s/%s:%s\n\n", u.username,
		    (unsigned)u.uid, (unsigned)u.gid, u.realName,
		    ACCT_LOCAL_USERS, u.username, u.shell);
		rc = ask_yesno("Is this the entry you wish to remove?", true);
		if (rc != 1) {
			ds_close(h);
			return (EX_CANCELLED);
		}
	}

	drop_home = !o->keep_home;
	if (interactive && drop_home) {
		char q[PATH_MAX + 64];

		(void)snprintf(q, sizeof(q),
		    "Remove user's home directory (%s/%s)?", ACCT_LOCAL_USERS,
		    u.username);
		rc = ask_yesno(q, true);
		if (rc == -1) {
			ds_close(h);
			return (EX_CANCELLED);
		}
		drop_home = (rc == 1);
	}

	/*
	 * Say something before killing a live session. A SIGKILL arriving in
	 * somebody's shell with no warning is the kind of surprise that gets
	 * blamed on the machine.
	 */
	if ((sessions = acct_sessions(name)) > 0)
		warnx("%s has %d login session%s; killing", name, sessions,
		    sessions == 1 ? "" : "s");
	if ((killed = acct_kill_uid(u.uid)) > 0 && o->verbose)
		(void)printf("  killed %d process%s\n", killed,
		    killed == 1 ? "" : "es");

	if (acct_remove_cron(name) == -1)
		warn("could not remove the crontab for %s", name);
	else if (o->verbose)
		(void)printf("  crontab removed\n");
	if (acct_remove_at(name) == -1)
		warn("could not remove at(1) jobs for %s", name);
	else if (o->verbose)
		(void)printf("  at jobs removed\n");

	if ((err = drop_plist_groups(h, name, o->verbose)) != DS_OK) {
		warnx("could not update the groups: %s", ds_strerror(err));
		ds_close(h);
		return (EX_DATABASE);
	}
	/*
	 * The private group adduser(8) creates alongside the account: same
	 * name, same gid, and nobody else in it once the user has been
	 * dropped. Leaving it behind means every removed account leaves an
	 * orphan group forever, and the next user allocated that gid would
	 * inherit a group named after somebody else.
	 *
	 * All three conditions are required. A group that merely shares the
	 * name, or that still has members, belongs to somebody and is left
	 * alone.
	 */
	if (ds_group_get(h, name, &pg) == DS_OK && pg.gid == u.gid &&
	    ds_group_member_count(h, name) == 0) {
		if ((err = ds_group_del(h, name)) != DS_OK) {
			warnx("could not remove the private group %s: %s",
			    name, ds_strerror(err));
			ds_close(h);
			return (EX_DATABASE);
		}
		if (o->verbose)
			(void)printf("  removed the private group %s\n", name);
	}

	if ((err = ds_user_del(h, name)) != DS_OK) {
		warnx("could not remove %s: %s", name, ds_strerror(err));
		ds_close(h);
		return (EX_DATABASE);
	}
	if ((err = ds_commit(h)) != DS_OK) {
		warnx("could not write the directory: %s", ds_strerror(err));
		ds_close(h);
		return (EX_DATABASE);
	}
	ds_close(h);

	/*
	 * The home goes after the record, not before. If the write fails the
	 * account still exists, and an account whose home has already been
	 * deleted is worse than one with a stale home.
	 */
	if (drop_home) {
		if (acct_remove_home(name) == -1)
			warn("could not remove the home directory for %s",
			    name);
		else if (o->verbose)
			(void)printf("  home removed\n");
	}
	drop_etc_groups(name, o->verbose);

	if (!o->verbose)
		(void)printf("Removing user (%s): %spasswd.\n", name,
		    drop_home ? "home " : "");
	return (EX_OK);
}

static int
run_batch(const struct opts *o)
{
	FILE *f;
	char line[1024];
	int status = EX_OK, n = 0, rc;

	if (strcmp(o->batch, "-") == 0)
		f = stdin;
	else if ((f = fopen(o->batch, "r")) == NULL) {
		warn("%s", o->batch);
		return (EX_USAGE);
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		n++;
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		rc = remove_one(o, line, false);
		if (rc != EX_OK) {
			warnx("%s:%d: %s was not removed", o->batch, n, line);
			status = EX_USAGE;
		}
	}
	if (f != stdin)
		(void)fclose(f);
	return (status);
}

int
main(int argc, char *argv[])
{
	static struct option longopts[] = {
		{ "keep-home", no_argument, NULL, 'K' },
		{ NULL, 0, NULL, 0 }
	};
	struct opts o;
	int ch, i, rc, status = EX_OK;

	memset(&o, 0, sizeof(o));
	while ((ch = getopt_long(argc, argv, "f:hvy", longopts, NULL)) != -1) {
		switch (ch) {
		case 'K': o.keep_home = true; break;
		case 'f': o.batch = optarg; break;
		case 'v': o.verbose = true; break;
		case 'y': o.yes = true; break;
		case 'h': usage(); return (EX_OK);
		default: usage(); return (EX_USAGE);
		}
	}
	argc -= optind;
	argv += optind;

#ifdef RMUSER_TEST
	{
		const char *fixture = getenv("NEXTBSD_DS_DIR");

		if (fixture != NULL && fixture[0] != '\0')
			ds_set_dirs(fixture, NULL);
		else if (geteuid() != 0) {
			warnx("must be run as root");
			return (EX_REFUSED);
		}
	}
#else
	if (geteuid() != 0) {
		warnx("must be run as root");
		return (EX_REFUSED);
	}
#endif

	if (o.batch != NULL) {
		if (argc > 0) {
			warnx("-f and names on the command line are mutually "
			    "exclusive");
			return (EX_USAGE);
		}
		return (run_batch(&o));
	}
	if (argc == 0) {
		usage();
		return (EX_USAGE);
	}
	/*
	 * -y means do not ask, so with it a name on the command line is not
	 * interactive either. Without it every named user is confirmed.
	 */
	for (i = 0; i < argc; i++) {
		rc = remove_one(&o, argv[i], !o.yes);
		if (rc != EX_OK)
			status = rc;
	}
	return (status);
}
