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
 * createhomedir -- create a user's home directory.
 *
 * Darwin creates a local account's home when the account is created, and uses
 * createhomedir(8) to build one for a directory account that has not logged in
 * yet. NextBSD creates homes at account creation too, in both the local and
 * the network case, because a network user is created on the server where the
 * home lives. So this command covers the rest: a home missing because a plist
 * was restored, hand-edited or copied between machines, and a home for an
 * account created by something other than adduser(8) -- Gershwin's dscli
 * writes Users.plist directly. login(1) degrades rather than failing when the
 * home is absent, printing "No home directory. Logging in with home = /", so
 * nobody is locked out and this can run afterwards.
 *
 * It creates the directory and nothing inside it.
 *
 * There was once a /System/Library/User Template whose tree was copied in,
 * carrying nine folders and a .zshrc. Both halves stopped being ours to own.
 * A global /etc/zprofile and zshrc mean a new account needs no dotfiles of its
 * own, and Gershwin's Workspace creates the standard folders at startup, so
 * shipping a second set only produced two of some and none of others.
 *
 * Users are enumerated through getpwent(3), so the DirectoryServices plists
 * are read by nss_directory_services(8) rather than by this program, and any
 * other name source works the same way. Only homes under /Local/Users or
 * /Network/Users are touched, and never a system account's: master.passwd
 * points those at /var/empty, /nonexistent and the like, which must stay
 * absent.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Homes we are willing to create. Anything else is left alone: system
 * accounts in master.passwd point at /var/empty, /nonexistent and the like,
 * which must stay absent. Darwin's -c, -s and -b select local, server or
 * both home paths; ours are the two roots the NSS module computes.
 */
/*
 * The system id ceiling, matching src/accounts/common/acct.h. Duplicated
 * rather than included because this program links none of the account
 * library, and one number is cheaper than the dependency.
 */
#define ACCT_SYSTEM_MAX	999

#define ROOT_LOCAL	"/Local/Users/"
#define ROOT_NETWORK	"/Network/Users/"

static bool want_local = false;		/* -c */
static bool want_network = false;	/* -s */
static bool dry_run;
static bool verbose;
static int exit_status;

static void
say(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	(void)vfprintf(stdout, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stdout);
}

static bool
under(const char *dir, const char *root)
{
	size_t n = strlen(root);

	/* Must be under the root, and name something inside it. */
	return (strncmp(dir, root, n) == 0 && dir[n] != '\0');
}

static bool
managed(const char *dir)
{
	if (dir == NULL || strstr(dir, "/..") != NULL)
		return (false);
	if (want_local && under(dir, ROOT_LOCAL))
		return (true);
	if (want_network && under(dir, ROOT_NETWORK))
		return (true);
	return (false);
}

/* mkdir -p for a path already known to sit under a managed root. */
static int
make_dirs(const char *path, mode_t mode, uid_t uid, gid_t gid)
{
	char buf[PATH_MAX];
	char *p;

	if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) == 0)
			(void)chown(buf, uid, gid);
		else if (errno != EEXIST)
			return (-1);
		*p = '/';
	}
	if (mkdir(buf, mode) == -1) {
		if (errno != EEXIST)
			return (-1);
	} else if (chown(buf, uid, gid) == -1)
		return (-1);
	return (0);
}




static void
build_home(const struct passwd *pw)
{
	struct stat st;

	/*
	 * A system account never gets a home. managed() already rejects
	 * /nonexistent and /var/empty, but an id below the system ceiling is
	 * the thing being tested rather than the spelling of its home field,
	 * and this now runs on every login session.
	 */
	if (pw->pw_uid <= ACCT_SYSTEM_MAX) {
		say("%s: uid %u is a system account; skipped", pw->pw_name,
		    (unsigned)pw->pw_uid);
		return;
	}
	if (!managed(pw->pw_dir)) {
		say("%s: home %s is not under a managed root; skipped",
		    pw->pw_name, pw->pw_dir != NULL ? pw->pw_dir : "(none)");
		return;
	}
	if (stat(pw->pw_dir, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			warnx("%s: %s exists and is not a directory",
			    pw->pw_name, pw->pw_dir);
			exit_status = 1;
		} else
			say("%s: %s exists", pw->pw_name, pw->pw_dir);
		return;
	}
	if (errno != ENOENT) {
		warn("%s", pw->pw_dir);
		exit_status = 1;
		return;
	}

	printf("%s%s: %s\n", dry_run ? "would create " : "creating ",
	    pw->pw_name, pw->pw_dir);
	if (dry_run)
		return;
	if (make_dirs(pw->pw_dir, 0755, pw->pw_uid, pw->pw_gid) == -1) {
		warn("%s", pw->pw_dir);
		exit_status = 1;
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: createhomedir [-bcs] [-nv] -a\n"
	    "       createhomedir [-bcs] [-nv] -u user [-u user ...]\n"
	    "       createhomedir [-bcs] [-nv] -i\n"
	    "       createhomedir [-bcs] [-nv] -P\n");
	exit(2);
}

/* Build the home for one named user, reporting a name we cannot resolve. */
static void
build_named(const char *name)
{
	struct passwd *pw = getpwnam(name);

	if (pw == NULL) {
		warnx("no such user: %s", name);
		exit_status = 1;
		return;
	}
	build_home(pw);
}

int
main(int argc, char **argv)
{
	char line[LINE_MAX], *nl;
	struct passwd *pw;
	char **names;
	size_t nnames = 0, i;
	bool all = false, from_stdin = false, both = false, from_pam = false;
	const char *pam_user;
	int ch;

	/* At most one name per argument, so this is allocated once. */
	names = calloc((size_t)argc + 1, sizeof(*names));
	if (names == NULL)
		err(1, NULL);

	while ((ch = getopt(argc, argv, "abcinPsu:v")) != -1) {
		switch (ch) {
		case 'a': all = true; break;
		case 'b': both = true; break;
		case 'c': want_local = true; break;
		case 'i': from_stdin = true; break;
		case 'n': dry_run = true; break;
		case 'P': from_pam = true; break;
		case 's': want_network = true; break;
		case 'u': names[nnames++] = optarg; break;
		case 'v': verbose = true; break;
		default: usage();
		}
	}
	if (argc != optind)
		usage();

	/* -b, or none of -b/-c/-s, means both roots. */
	if (both || (!want_local && !want_network))
		want_local = want_network = true;
	/*
	 * -P is for a PAM session, where the user is in the environment
	 * because pam_exec(8) cannot substitute it into an argument. It says
	 * nothing and succeeds when PAM_USER is unset: a session phase that
	 * runs without it is not this program's problem to report, and the
	 * pam.d line is `optional` precisely so nothing here can block a
	 * login.
	 */
	if (from_pam) {
		if (all || from_stdin || nnames > 0)
			usage();
		if ((pam_user = getenv("PAM_USER")) == NULL ||
		    pam_user[0] == '\0')
			return (0);
		names[nnames++] = (char *)pam_user;
	}
	/* With no selection at all, every user is the sensible default. */
	if (!all && !from_stdin && nnames == 0 && !from_pam)
		all = true;
	if (all && (from_stdin || nnames > 0))
		usage();
	if (!dry_run && geteuid() != 0)
		errx(1, "must be root to create home directories");

	if (all) {
		setpwent();
		while ((pw = getpwent()) != NULL)
			build_home(pw);
		endpwent();
	}
	for (i = 0; i < nnames; i++)
		build_named(names[i]);
	if (from_stdin) {
		while (fgets(line, sizeof(line), stdin) != NULL) {
			if ((nl = strchr(line, '\n')) != NULL)
				*nl = '\0';
			if (line[0] != '\0')
				build_named(line);
		}
	}
	free(names);
	return (exit_status);
}
