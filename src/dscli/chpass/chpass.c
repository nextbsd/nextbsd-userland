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
 * chpass(1), chfn(1) and chsh(1) for NextBSD (E18 U9). For a directory
 * user, -f edits realName and -s edits shell in Users.plist; a user may
 * change their own after giving their password, root anyone's. For root
 * and the system users the FreeBSD chpass, kept at /usr/libexec/bsd/chpass,
 * is run with the same arguments. Setuid root, as FreeBSD's is.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "acct.h"
#include "ds.h"

static const char *progname = "chpass";

static void
usage(void)
{
	if (strcmp(progname, "chfn") == 0 || strcmp(progname, "ypchfn") == 0)
		fprintf(stderr, "usage: %s [-f full-name] [user]\n", progname);
	else if (strcmp(progname, "chsh") == 0 || strcmp(progname, "ypchsh") == 0)
		fprintf(stderr, "usage: %s [-s shell] [user]\n", progname);
	else
		fprintf(stderr, "usage: %s [-f full-name] [-s shell] [user]\n",
		    progname);
	exit(EX_USAGE);
}

static void
exec_bsd(char *argv[])
{
	char path[PATH_MAX];

	if (ds_bsd_tool("chpass", "/usr/bin/chpass", path, sizeof(path)) == -1)
		errx(EX_UNAVAILABLE, "FreeBSD's chpass is not available at %s/chpass; "
		    "edit %s with vipw(8)", DS_BSD_DIR, DS_MASTER_PASSWD);
	execv(path, argv);
	err(EX_OSERR, "exec %s", path);
}

static int
edit_plist(const char *name, uid_t ruid, const char *real, const char *shell)
{
	struct ds_users us;
	struct ds_user *u;
	char path[PATH_MAX];
	char *pass = NULL;
	int lockfd = -1, rv = 1;

	if (ds_path(path, sizeof(path), ds_dir(), DS_USERS_PLIST) == -1 ||
	    ds_users_load(&us, path) == -1) {
		warn("%s", path);
		return (1);
	}
	u = ds_user_find(&us, name);
	if (u == NULL) {
		warnx("%s: no such user in %s", name, path);
		goto out;
	}
	if (ruid != 0 && ruid != u->uid) {
		warnx("you may only change your own information");
		goto out;
	}
	if (real == NULL && shell == NULL) {
		printf("Login:     %s\n", u->name);
		printf("Uid:       %lu\n", (unsigned long)u->uid);
		printf("Gid:       %lu\n", (unsigned long)u->gid);
		printf("Full name: %s\n", u->real);
		printf("Shell:     %s\n", u->shell);
		printf("Home:      %s/Users/%s\n",
		    ds_from_network() ? "/Network" : "/Local", u->name);
		printf("%s: use -f to change the full name and -s the shell; "
		    "the other fields belong to dscli(8)\n", progname);
		rv = 0;
		goto out;
	}
	if (acct_refuse_if_joined(progname))
		goto out;
	if (shell != NULL) {
		if (shell[0] != '/') {
			warnx("%s: shell must be an absolute path", shell);
			goto out;
		}
		if (!ds_shell_listed(shell)) {
			if (ruid != 0) {
				warnx("%s: not a listed shell (see %s)", shell,
				    DS_SHELLS);
				goto out;
			}
			warnx("warning: %s is not listed in %s", shell, DS_SHELLS);
		}
		if (access(shell, X_OK) == -1)
			warnx("warning: %s is not executable", shell);
	}
	if (real != NULL && strchr(real, ':') != NULL) {
		warnx("full name may not contain ':'");
		goto out;
	}
	if (ruid != 0) {
		pass = acct_read_password(progname, "Password", false);
		if (pass == NULL || !ds_verify_password(u, pass)) {
			sleep(1);
			warnx("sorry");
			goto out;
		}
	}

	lockfd = ds_lock(ds_dir());
	if (lockfd == -1) {
		warn("lock %s", ds_dir());
		goto out;
	}
	ds_users_free(&us);
	if (ds_users_load(&us, path) == -1 || (u = ds_user_find(&us, name)) == NULL) {
		warnx("%s: vanished from %s", name, path);
		memset(&us, 0, sizeof(us));
		goto out;
	}
	if ((real != NULL && ds_set_string(&u->real, real) == -1) ||
	    (shell != NULL && ds_set_string(&u->shell, shell) == -1)) {
		warn("update record");
		goto out;
	}
	if (ds_users_save(&us, path) == -1) {
		warn("write %s", path);
		goto out;
	}
	if (real != NULL)
		printf("%s: full name of %s is now \"%s\"\n", progname, name, real);
	if (shell != NULL)
		printf("%s: shell of %s is now %s\n", progname, name, shell);
	rv = 0;
out:
	if (lockfd != -1)
		ds_unlock(lockfd);
	ds_users_free(&us);
	acct_wipe(pass);
	return (rv);
}

int
main(int argc, char *argv[])
{
	struct passwd *pw;
	char namebuf[64];
	const char *name = NULL, *real = NULL, *shell = NULL;
	char **orig_argv = argv;
	uid_t ruid;
	bool only_fn, only_sh, unsupported = false;
	int ch;

	progname = basename(argv[0]);
	only_fn = strcmp(progname, "chfn") == 0 || strcmp(progname, "ypchfn") == 0;
	only_sh = strcmp(progname, "chsh") == 0 || strcmp(progname, "ypchsh") == 0;

	/* FreeBSD's full option set is accepted so the exec below sees it. */
	while ((ch = getopt(argc, argv, "a:p:s:e:d:f:h:loy")) != -1) {
		switch (ch) {
		case 'f':
			if (only_sh)
				usage();
			real = optarg;
			break;
		case 's':
			if (only_fn)
				usage();
			shell = optarg;
			break;
		case 'a':
		case 'p':
		case 'e':
		case 'd':
		case 'h':
		case 'l':
		case 'o':
		case 'y':
			unsupported = true;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();

	ruid = getuid();
	if (argc == 1) {
		name = argv[0];
	} else {
		if (ds_route_uid(ruid, namebuf, sizeof(namebuf)) != DS_WHERE_NONE)
			name = namebuf;
		else if ((pw = getpwuid(ruid)) != NULL)
			name = pw->pw_name;
		else
			errx(EX_NOUSER, "uid %lu has no account", (unsigned long)ruid);
	}

	switch (ds_route_user(name)) {
	case DS_WHERE_PLIST:
		if (unsupported)
			errx(EX_USAGE, "only -f and -s apply to a directory user; "
			    "see dscli(8) for the rest");
		return (edit_plist(name, ruid, real, shell));
	case DS_WHERE_SYSTEM:
		exec_bsd(orig_argv);
		/* NOTREACHED */
	default:
		errx(EX_NOUSER, "%s: no such user", name);
	}
}
