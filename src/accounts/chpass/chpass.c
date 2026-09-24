/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * chpass, chfn, chsh — change a user's real name or login shell in the
 * DirectoryServices plists (nextbsd/nextbsd-userland#255, E18 U9).
 *
 * One binary behind several names, and here the names are not synonyms.
 * On the BSD this borrows its name from, chfn and chsh are both just chpass
 * and either will edit anything. Here chfn edits the real name and chsh the
 * shell, and neither will touch the other field. A command called chsh that
 * can rewrite your full name is a trap, and the restriction costs nothing.
 *
 * There is no editor session. The plists hold two editable fields for a
 * regular account, and dropping someone into $EDITOR over two lines invites
 * them to change a uid and wonder why nothing happened.
 *
 * The yp* names exist only to say that NIS is not supported, rather than
 * leaving the shell to say "command not found" about a name that exists on
 * every other BSD.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <readpassphrase.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acct.h"
#include "libds.h"

/* Where the base copy lives once compat relocates it, for system accounts. */
#ifndef CHPASS_BSD_BINARY
#define CHPASS_BSD_BINARY	"/usr/libexec/bsd/chpass"
#endif

/* Which name we were invoked as, and so which fields may be edited. */
enum mode {
	MODE_CHPASS,	/* both */
	MODE_CHFN,	/* real name only */
	MODE_CHSH,	/* shell only */
	MODE_YP		/* refuse: NIS is not supported */
};

static enum mode
mode_from_name(const char *argv0)
{
	char buf[PATH_MAX];
	const char *base;

	(void)strlcpy(buf, argv0 != NULL ? argv0 : "chpass", sizeof(buf));
	base = basename(buf);
	if (strncmp(base, "yp", 2) == 0)
		return (MODE_YP);
	if (strcmp(base, "chfn") == 0)
		return (MODE_CHFN);
	if (strcmp(base, "chsh") == 0)
		return (MODE_CHSH);
	return (MODE_CHPASS);
}

static void
usage(enum mode m)
{
	switch (m) {
	case MODE_CHFN:
		(void)fprintf(stderr, "usage: chfn [-f \"full name\"] [user]\n");
		break;
	case MODE_CHSH:
		(void)fprintf(stderr, "usage: chsh [-s shell] [user]\n");
		break;
	default:
		(void)fprintf(stderr,
		    "usage: chpass [-f \"full name\"] [-s shell] [user]\n");
		break;
	}
	exit(1);
}

/*
 * Prove the caller's own password before letting them edit their record.
 * The BSD this takes its name from does not ask, relying on file ownership.
 * Here the record is root-owned and the tool is setuid, so ownership proves
 * nothing, and a moment at an unlocked terminal would otherwise be enough to
 * point somebody's login shell at a program of your choosing.
 */
static bool
prove_self(const struct ds_userrec *u)
{
	char pw[256];
	bool ok;

	if (u->noPassword || !u->hasHash)
		return (true);	/* nothing to prove */
	if (readpassphrase("Password: ", pw, sizeof(pw), RPP_REQUIRE_TTY) == NULL)
		return (false);
	ok = acct_verify_password(pw, u->passwordHash);
	acct_zero(pw, sizeof(pw));
	if (!ok) {
		(void)sleep(1);
		warnx("sorry");
	}
	return (ok);
}

static void
show(const struct ds_userrec *u)
{
	(void)printf("Login:       %s\n", u->username);
	(void)printf("Uid [#]:     %u\n", (unsigned)u->uid);
	(void)printf("Gid [# or name]: %u\n", (unsigned)u->gid);
	(void)printf("Full Name:   %s\n", u->realName);
	(void)printf("Home:        %s/%s\n", ACCT_LOCAL_USERS, u->username);
	(void)printf("Shell:       %s\n", u->shell);
}

int
main(int argc, char *argv[])
{
	struct ds_handle *h;
	struct ds_userrec u;
	struct passwd *self;
	const char *newname = NULL, *newshell = NULL, *name;
	char server[256];
	enum mode m;
	enum ds_error err;
	uid_t ruid;
	int ch;

	m = mode_from_name(argv[0]);
	if (m == MODE_YP) {
		warnx("NIS is not supported on NextBSD");
		return (1);
	}

	while ((ch = getopt(argc, argv, "f:s:")) != -1) {
		switch (ch) {
		case 'f':
			if (m == MODE_CHSH) {
				warnx("chsh changes the shell; use chfn for the "
				    "full name");
				return (1);
			}
			newname = optarg;
			break;
		case 's':
			if (m == MODE_CHFN) {
				warnx("chfn changes the full name; use chsh for "
				    "the shell");
				return (1);
			}
			newshell = optarg;
			break;
		default:
			usage(m);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage(m);

	ruid = getuid();
#ifdef CHPASS_TEST
	{
		const char *fixture = getenv("NEXTBSD_DS_DIR");
		const char *as = getenv("NEXTBSD_TEST_UID");

		if (fixture != NULL && fixture[0] != '\0') {
			ds_set_dirs(fixture, NULL);
			ruid = (as != NULL && as[0] != '\0') ?
			    (uid_t)strtoul(as, NULL, 10) : 0;
		}
	}
#endif
	if (argc == 1)
		name = argv[0];
	else if ((self = getpwuid(ruid)) != NULL)
		name = self->pw_name;
	else {
		warnx("cannot work out who you are (uid %u)", (unsigned)ruid);
		return (1);
	}

	if ((err = ds_open(DS_LOCAL, DS_RDWR, &h)) != DS_OK) {
		warnx("%s", err == DS_ELOCK ?
		    "the directory is locked; another account tool may be "
		    "running" : ds_strerror(err));
		return (1);
	}
	if (ds_user_get(h, name, &u) != DS_OK) {
		ds_close(h);
		/*
		 * Not a directory account. The base copy handles
		 * master.passwd, once compat has relocated it; until then say
		 * what to use instead rather than failing vaguely.
		 */
		if (getpwnam(name) != NULL) {
			if (access(CHPASS_BSD_BINARY, X_OK) == 0) {
				(void)execv(CHPASS_BSD_BINARY, argv - optind);
				warn("%s", CHPASS_BSD_BINARY);
				return (1);
			}
			warnx("%s is in master.passwd; use vipw(8)", name);
			return (1);
		}
		warnx("%s: no such user", name);
		return (1);
	}

	/* Authorisation on the real uid, never the effective one. */
	if (ruid != 0 && ruid != u.uid) {
		warnx("you may only change your own information");
		ds_close(h);
		return (1);
	}
	if (acct_bound_server(server, sizeof(server))) {
		if (server[0] != '\0')
			warnx("this machine is joined to %s; change the record "
			    "there", server);
		else
			warnx("this machine is joined to a directory server; "
			    "change the record there");
		ds_close(h);
		return (1);
	}

	/* Nothing to change: show the record and say how to change it. */
	if (newname == NULL && newshell == NULL) {
		show(&u);
		switch (m) {
		case MODE_CHFN:
			(void)printf("\nUse chfn -f \"Full Name\" to change the "
			    "name.\n");
			break;
		case MODE_CHSH:
			(void)printf("\nUse chsh -s /path/to/shell to change "
			    "the shell.\n");
			break;
		default:
			(void)printf("\nUse chpass -f \"Full Name\" or "
			    "chpass -s /path/to/shell to change these.\n");
			break;
		}
		ds_close(h);
		return (0);
	}

	if (ruid != 0 && !prove_self(&u)) {
		ds_close(h);
		return (1);
	}

	if (newshell != NULL) {
		if (newshell[0] != '/') {
			warnx("%s: a shell must be an absolute path", newshell);
			ds_close(h);
			return (1);
		}
		if (!acct_shell_listed(newshell)) {
			/*
			 * Fatal for a user, a warning for root: an
			 * administrator installing a shell outside
			 * /etc/shells knows what they are doing, and a user
			 * pointing their login at an arbitrary binary does
			 * not get to decide that.
			 */
			if (ruid != 0) {
				warnx("%s: not a listed shell (see %s)",
				    newshell, ACCT_SHELLS);
				ds_close(h);
				return (1);
			}
			warnx("warning: %s is not listed in %s", newshell,
			    ACCT_SHELLS);
		}
		if (access(newshell, X_OK) == -1)
			warnx("warning: %s is not executable", newshell);
		(void)strlcpy(u.shell, newshell, sizeof(u.shell));
	}
	if (newname != NULL) {
		if (strchr(newname, ':') != NULL) {
			warnx("a full name may not contain a colon");
			ds_close(h);
			return (1);
		}
		(void)strlcpy(u.realName, newname, sizeof(u.realName));
	}

	if ((err = ds_user_set(h, &u)) != DS_OK ||
	    (err = ds_commit(h)) != DS_OK) {
		warnx("could not write the directory: %s", ds_strerror(err));
		ds_close(h);
		return (1);
	}
	ds_close(h);
	return (0);
}
