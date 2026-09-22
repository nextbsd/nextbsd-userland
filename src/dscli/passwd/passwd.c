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
 * passwd(1) for NextBSD (E18 U9). A directory user's password lives in
 * Users.plist as passwordHash (SHA-512 crypt, Gershwin-compatible); root
 * and the system users keep theirs in master.passwd. This program decides
 * which one the account is in and edits that: the plist through libds,
 * master.passwd through libutil's pw_* functions followed by pwd_mkdb, the
 * same sequence FreeBSD's pam_unix uses.
 *
 * Installed setuid root, as FreeBSD's is. Users change their own password
 * after giving the old one; root sets anyone's. `passwd -d USER` (root
 * only) marks a directory user noPassword, so they log in with none.
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

#include <libutil.h>

#include "acct.h"
#include "ds.h"

static const char *progname = "passwd";

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-l] [-d] [user]\n", progname);
	exit(EX_USAGE);
}

/* Constant-time comparison of two hashes of possibly different length. */
static bool
hash_equal(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b), i;
	unsigned char diff = (unsigned char)(la != lb);

	for (i = 0; i < la; i++)
		diff |= (unsigned char)(a[i] ^ b[i % (lb == 0 ? 1 : lb)]);
	return (diff == 0);
}

/* The old password of a master.passwd account, verified with crypt(3). */
static bool
system_verify(const struct passwd *pw, const char *given)
{
	const char *h;

	if (pw->pw_passwd[0] == '\0')
		return (given[0] == '\0');
	h = crypt(given, pw->pw_passwd);
	return (h != NULL && hash_equal(h, pw->pw_passwd));
}

static int
change_plist(const char *name, uid_t ruid, bool delete)
{
	struct ds_users us;
	struct ds_user *u;
	char path[PATH_MAX];
	char *old = NULL, *new = NULL;
	int lockfd = -1, rv = 1;

	if (acct_refuse_if_joined(progname))
		return (1);
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
		warnx("you may only change your own password");
		goto out;
	}
	if (delete && ruid != 0) {
		warnx("only root may remove a password");
		goto out;
	}
	printf("Changing %s password for %s.\n",
	    ds_from_network() ? "network" : "local", name);
	if (ruid != 0) {
		old = acct_read_password(progname, "Old password", false);
		if (old == NULL)
			goto out;
		if (!ds_verify_password(u, old)) {
			sleep(1);
			warnx("sorry");
			goto out;
		}
	}
	if (!delete) {
		new = acct_read_password(progname, "New password", true);
		if (new == NULL)
			goto out;
		if (new[0] == '\0') {
			warnx("empty password; use `passwd -d %s` to allow "
			    "logging in without one", name);
			goto out;
		}
	}

	/* Re-read under the lock so a concurrent edit is not lost. */
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
	free(u->hash);
	u->hash = NULL;
	if (delete) {
		u->nopass = true;
	} else {
		u->hash = ds_hash_password(new);
		if (u->hash == NULL) {
			warn("hashing the password");
			goto out;
		}
		u->nopass = false;
	}
	if (ds_users_save(&us, path) == -1) {
		warn("write %s", path);
		goto out;
	}
	printf("%s: %s\n", progname, delete ? "password removed" : "password updated");
	rv = 0;
out:
	if (lockfd != -1)
		ds_unlock(lockfd);
	ds_users_free(&us);
	acct_wipe(old);
	acct_wipe(new);
	return (rv);
}

static int
change_system(const char *name, uid_t ruid, bool delete)
{
	struct passwd *pw, *npw = NULL;
	char *old = NULL, *new = NULL, *hash = NULL;
	int pfd = -1, tfd = -1, rv = 1;

	if (delete) {
		warnx("-d applies to directory users only; edit %s with vipw "
		    "for a system account", DS_MASTER_PASSWD);
		return (1);
	}
	/* nsswitch answers from files for a system account; euid 0 sees the hash. */
	pw = getpwnam(name);
	if (pw == NULL) {
		warnx("%s: no such user", name);
		return (1);
	}
	if (ruid != 0 && ruid != pw->pw_uid) {
		warnx("you may only change your own password");
		return (1);
	}
	pw = pw_dup(pw);
	if (pw == NULL) {
		warn("pw_dup");
		return (1);
	}
	printf("Changing local password for %s.\n", name);
	if (ruid != 0) {
		old = acct_read_password(progname, "Old password", false);
		if (old == NULL || !system_verify(pw, old)) {
			sleep(1);
			warnx("sorry");
			goto out;
		}
	}
	new = acct_read_password(progname, "New password", true);
	if (new == NULL)
		goto out;
	if (new[0] == '\0') {
		warnx("empty password not accepted");
		goto out;
	}
	hash = ds_hash_password(new);
	if (hash == NULL) {
		warn("hashing the password");
		goto out;
	}
	npw = pw_dup(pw);
	if (npw == NULL) {
		warn("pw_dup");
		goto out;
	}
	npw->pw_passwd = hash;
	npw->pw_change = 0;

	if (pw_init(NULL, NULL) == -1) {
		warn("pw_init");
		goto out;
	}
	pfd = pw_lock();
	if (pfd == -1) {
		warn("lock %s", DS_MASTER_PASSWD);
		goto out;
	}
	tfd = pw_tmp(pfd);
	if (tfd == -1) {
		warn("temporary passwd file");
		goto out;
	}
	if (pw_copy(pfd, tfd, npw, pw) == -1) {
		warn("update %s", DS_MASTER_PASSWD);
		goto out;
	}
	if (pw_mkdb(name) == -1) {
		warn("pwd_mkdb");
		goto out;
	}
	printf("%s: password updated\n", progname);
	rv = 0;
out:
	pw_fini();
	if (npw != NULL) {
		npw->pw_passwd = NULL;	/* hash is freed below */
		free(npw);
	}
	free(pw);
	acct_wipe(old);
	acct_wipe(new);
	free(hash);
	return (rv);
}

int
main(int argc, char *argv[])
{
	struct passwd *pw;
	char namebuf[64];
	const char *name = NULL;
	uid_t ruid;
	bool delete = false;
	int ch;

	progname = basename(argv[0]);
	if (strcmp(progname, "yppasswd") == 0)
		errx(EX_UNAVAILABLE, "NIS is not supported on NextBSD; use passwd(1)");

	while ((ch = getopt(argc, argv, "dl")) != -1) {
		switch (ch) {
		case 'd':
			delete = true;
			break;
		case 'l':	/* local: the only kind there is */
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
		/* The caller's own account, wherever it lives. */
		if (ds_route_uid(ruid, namebuf, sizeof(namebuf)) != DS_WHERE_NONE)
			name = namebuf;
		else if ((pw = getpwuid(ruid)) != NULL)
			name = pw->pw_name;
		else
			errx(EX_NOUSER, "uid %lu has no account", (unsigned long)ruid);
	}

	switch (ds_route_user(name)) {
	case DS_WHERE_PLIST:
		return (change_plist(name, ruid, delete));
	case DS_WHERE_SYSTEM:
		return (change_system(name, ruid, delete));
	default:
		errx(EX_NOUSER, "%s: no such user", name);
	}
}
