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
 * passwd — change a password, in the DirectoryServices plists or in
 * master.passwd (nextbsd/nextbsd-userland#255, E18 U9).
 *
 * NextBSD's own, replacing the one from base, because that one cannot be
 * wrapped: it switches on the source an account was resolved from and
 * accepts only _PWF_FILES or _PWF_NIS (usr.bin/passwd/passwd.c), so every
 * directory account gets "Sorry, `passwd' can only change passwords for
 * local or NIS users." Wrapping cannot fix a refusal that happens before
 * our code would run.
 *
 * An existing account is edited wherever it lives: the plists when it is
 * there, master.passwd otherwise. Nothing is routed by uid here, unlike a
 * new account in adduser(8) or pw(8); an account that exists has already
 * decided which store it is in.
 *
 * Setuid root, because the plists are mode 0644 owned by root: a user can
 * read their own hash but cannot rewrite the file. The real uid is what
 * every authorisation decision uses, and the effective uid matters only
 * for the write.
 *
 * The prompt wording follows Darwin's so the two systems read alike.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <pwd.h>
#include <readpassphrase.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __FreeBSD__
#include <libutil.h>
#define PASSWD_HAVE_LIBUTIL 1
#endif

#include "acct.h"
#include "libds.h"

/* Which store the account lives in. */
enum store {
	STORE_NONE,
	STORE_DIRECTORY,	/* Users.plist */
	STORE_FILES		/* master.passwd */
};

/* What we were asked to do. */
enum action {
	ACT_CHANGE,		/* the default: set a new password */
	ACT_CLEAR,		/* -d: noPassword */
	ACT_LOCK,		/* -l */
	ACT_UNLOCK		/* -u */
};

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: passwd [-dlu] [-i files|directory] [user]\n");
	exit(1);
}

/*
 * Read a password with no echo. Returns false on EOF or error, which the
 * caller treats as "give up", not as an empty password.
 */
static bool
ask_secret(const char *prompt, char *buf, size_t len)
{
	if (readpassphrase(prompt, buf, len, RPP_REQUIRE_TTY) == NULL) {
		acct_zero(buf, len);
		return (false);
	}
	return (true);
}

/*
 * The new password, asked twice. Returns 1 on success, 0 when the caller
 * gave an empty password (leave it alone), -1 to abort.
 */
static int
ask_new(char *out, size_t len)
{
	char again[256];
	int rc = -1;

	for (;;) {
		if (!ask_secret("New password: ", out, len))
			return (-1);
		if (out[0] == '\0') {
			(void)printf("Password unchanged.\n");
			return (0);
		}
		if (!ask_secret("Retype new password: ", again, sizeof(again)))
			break;
		if (strcmp(out, again) == 0) {
			rc = 1;
			break;
		}
		(void)fprintf(stderr, "Mismatch; try again, EOF to quit.\n");
		acct_zero(again, sizeof(again));
	}
	acct_zero(again, sizeof(again));
	if (rc != 1)
		acct_zero(out, len);
	return (rc);
}

/* ---- the directory store -------------------------------------------- */

static int
do_directory(const char *name, enum action act, uid_t ruid)
{
	struct ds_handle *h;
	struct ds_userrec u;
	char newpw[256];
	enum ds_error err;
	int rc;

	if ((err = ds_open(DS_LOCAL, DS_RDWR, &h)) != DS_OK) {
		warnx("%s", err == DS_ELOCK ?
		    "the directory is locked; another account tool may be "
		    "running" : ds_strerror(err));
		return (1);
	}
	if (ds_user_get(h, name, &u) != DS_OK) {
		warnx("%s: no such user", name);
		ds_close(h);
		return (1);
	}
	/* Authorisation is on the real uid, never the effective one. */
	if (ruid != 0 && ruid != u.uid) {
		warnx("permission denied");
		ds_close(h);
		return (1);
	}

	switch (act) {
	case ACT_CLEAR:
		u.noPassword = true;
		u.hasHash = false;
		u.passwordHash[0] = '\0';
		break;
	case ACT_LOCK:
		if (acct_hash_locked(u.passwordHash)) {
			warnx("%s: already locked", name);
			ds_close(h);
			return (1);
		}
		{
			char tmp[DS_HASH_MAX];

			(void)snprintf(tmp, sizeof(tmp), "%s%s",
			    ACCT_LOCK_PREFIX,
			    u.hasHash ? u.passwordHash : "");
			(void)strlcpy(u.passwordHash, tmp,
			    sizeof(u.passwordHash));
		}
		u.hasHash = true;
		u.noPassword = false;
		break;
	case ACT_UNLOCK:
		if (!acct_hash_locked(u.passwordHash)) {
			warnx("%s: not locked", name);
			ds_close(h);
			return (1);
		}
		(void)memmove(u.passwordHash,
		    u.passwordHash + sizeof(ACCT_LOCK_PREFIX) - 1,
		    strlen(u.passwordHash) - (sizeof(ACCT_LOCK_PREFIX) - 1) + 1);
		if (u.passwordHash[0] == '\0') {
			/* Locked while it had no password: restore that. */
			u.hasHash = false;
			u.noPassword = true;
		}
		break;
	case ACT_CHANGE:
		(void)printf("Changing local password for %s.\n", name);
		/*
		 * Prove the old password, unless the caller is root or the
		 * account has none to prove.
		 */
		if (ruid != 0 && !u.noPassword && u.hasHash) {
			char old[256];

			if (!ask_secret("Old password: ", old, sizeof(old))) {
				ds_close(h);
				return (1);
			}
			if (!acct_verify_password(old, u.passwordHash)) {
				acct_zero(old, sizeof(old));
				(void)sleep(1);
				warnx("sorry");
				ds_close(h);
				return (1);
			}
			acct_zero(old, sizeof(old));
		}
		rc = ask_new(newpw, sizeof(newpw));
		if (rc <= 0) {
			ds_close(h);
			return (rc == 0 ? 0 : 1);
		}
		if (!acct_hash_password(newpw, u.passwordHash,
		    sizeof(u.passwordHash))) {
			acct_zero(newpw, sizeof(newpw));
			warnx("could not hash the password");
			ds_close(h);
			return (1);
		}
		acct_zero(newpw, sizeof(newpw));
		u.hasHash = true;
		u.noPassword = false;
		break;
	}

	if ((err = ds_user_set(h, &u)) != DS_OK ||
	    (err = ds_commit(h)) != DS_OK) {
		warnx("could not write the directory: %s", ds_strerror(err));
		ds_close(h);
		return (1);
	}
	ds_close(h);
	if (act == ACT_CHANGE)
		warnx("password updated");
	return (0);
}

/* ---- master.passwd -------------------------------------------------- */

#ifdef PASSWD_HAVE_LIBUTIL
/*
 * The libutil pw_* family, which is the only correct way to edit
 * master.passwd on this system: it takes the lock, writes through a
 * temporary file, and runs pwd_mkdb so /etc/pwd.db and /etc/spwd.db are
 * regenerated. Editing the text file alone would leave libc answering from
 * a stale database, so a password change would appear to succeed and do
 * nothing.
 */
static int
do_files(const char *name, enum action act, uid_t ruid)
{
	struct passwd *pw, newpw;
	char newpass[256], hashbuf[512];
	int pfd, tfd, rc;

	if ((pw = getpwnam(name)) == NULL) {
		warnx("%s: no such user", name);
		return (1);
	}
	if (ruid != 0 && ruid != pw->pw_uid) {
		warnx("permission denied");
		return (1);
	}
	/*
	 * -d, -l and -u are directory-only. master.passwd has its own
	 * conventions for these and vipw(8) is the tool for them; quietly
	 * doing something different here would be worse than refusing.
	 */
	if (act != ACT_CHANGE) {
		warnx("%s is in master.passwd; -d, -l and -u apply only to "
		    "directory accounts. Use vipw(8).", name);
		return (1);
	}

	(void)printf("Changing local password for %s.\n", name);
	if (ruid != 0 && pw->pw_passwd != NULL && pw->pw_passwd[0] != '\0' &&
	    strcmp(pw->pw_passwd, "*") != 0) {
		char old[256];

		if (!ask_secret("Old password: ", old, sizeof(old)))
			return (1);
		if (!acct_verify_password(old, pw->pw_passwd)) {
			acct_zero(old, sizeof(old));
			(void)sleep(1);
			warnx("sorry");
			return (1);
		}
		acct_zero(old, sizeof(old));
	}
	rc = ask_new(newpass, sizeof(newpass));
	if (rc <= 0)
		return (rc == 0 ? 0 : 1);
	if (!acct_hash_password(newpass, hashbuf, sizeof(hashbuf))) {
		acct_zero(newpass, sizeof(newpass));
		warnx("could not hash the password");
		return (1);
	}
	acct_zero(newpass, sizeof(newpass));

	newpw = *pw;
	newpw.pw_passwd = hashbuf;

	if (pw_init(NULL, NULL) == -1) {
		warn("pw_init");
		return (1);
	}
	if ((pfd = pw_lock()) == -1) {
		warn("cannot lock the password file");
		pw_fini();
		return (1);
	}
	if ((tfd = pw_tmp(pfd)) == -1) {
		warn("cannot create a temporary password file");
		pw_fini();
		return (1);
	}
	if (pw_copy(pfd, tfd, &newpw, pw) == -1) {
		warnx("cannot write the temporary password file");
		pw_fini();
		return (1);
	}
	if (pw_mkdb(newpw.pw_name) == -1) {
		warn("cannot rebuild the password database");
		pw_fini();
		return (1);
	}
	pw_fini();
	warnx("password updated");
	return (0);
}
#else
static int
do_files(const char *name, enum action act __unused, uid_t ruid __unused)
{
	/*
	 * Host builds for the unit tests have no libutil pw_* family. The
	 * directory path is what those tests exercise; master.passwd is
	 * covered by the on-image suite.
	 */
	warnx("%s: editing master.passwd needs libutil, which this build "
	    "does not have", name);
	return (1);
}
#endif

/* ---- which store, and then get on with it --------------------------- */

static enum store
find_store(const char *name, const char *forced)
{
	struct ds_handle *h;
	struct ds_userrec u;
	enum store s = STORE_NONE;

	if (forced != NULL) {
		if (strcmp(forced, "directory") == 0)
			return (STORE_DIRECTORY);
		if (strcmp(forced, "files") == 0)
			return (STORE_FILES);
		warnx("%s: -i takes files or directory", forced);
		exit(1);
	}
	/*
	 * Read-only first, so deciding where an account lives never takes the
	 * write lock. The chosen backend takes it properly afterwards.
	 */
	if (ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK) {
		if (ds_user_get(h, name, &u) == DS_OK)
			s = STORE_DIRECTORY;
		ds_close(h);
	}
	if (s == STORE_NONE && getpwnam(name) != NULL)
		s = STORE_FILES;
	return (s);
}

int
main(int argc, char *argv[])
{
	char server[256];
	const char *forced = NULL;
	const char *name;
	struct passwd *self;
	enum action act = ACT_CHANGE;
	enum store store;
	uid_t ruid;
	int ch, nact = 0;

	while ((ch = getopt(argc, argv, "dlui:")) != -1) {
		switch (ch) {
		case 'd': act = ACT_CLEAR;  nact++; break;
		case 'l': act = ACT_LOCK;   nact++; break;
		case 'u': act = ACT_UNLOCK; nact++; break;
		case 'i': forced = optarg; break;
		default: usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1 || nact > 1)
		usage();

	ruid = getuid();
	/*
	 * Test hook, with its own macro and not libds's, for the reason given
	 * in adduser: a flag that makes this tool believe the caller is root
	 * must not be reachable through a macro another component might
	 * switch on. Nothing in the shipped build defines PASSWD_TEST.
	 */
#ifdef PASSWD_TEST
	{
		const char *fixture = getenv("NEXTBSD_DS_DIR");
		const char *as = getenv("NEXTBSD_TEST_UID");

		if (fixture != NULL && fixture[0] != '\0') {
			ds_set_dirs(fixture, NULL);
			/*
			 * The pretended uid is settable, not fixed at 0. An
			 * earlier version always pretended to be root, which
			 * made the root-only paths testable and quietly made
			 * the authorisation check untestable: disabling it
			 * broke no test at all. Whatever a hook makes
			 * easy to reach, it must not make anything
			 * unreachable.
			 */
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

	/* -d, -l and -u are root's. */
	if (act != ACT_CHANGE && ruid != 0) {
		warnx("only root may use -d, -l or -u");
		return (1);
	}

	/*
	 * Asked before find_store(), because the store is the wrong thing to
	 * branch on first. A joined machine reads its accounts from the
	 * server's copy under /Network, which this tool has no business
	 * rewriting -- it belongs to the server, and an export with root
	 * squashed would refuse the write anyway.
	 *
	 * This check used to live inside do_directory(), which find_store()
	 * only selects when the name is in the LOCAL plists. On a real client
	 * it is not, so the routing went to do_files() and the refusal never
	 * ran. Worse than a missing diagnostic: on a client that still had a
	 * local master.passwd entry of the same name, passwd rewrote the local
	 * hash and reported success while login carried on authenticating
	 * against /Network -- the password had not changed.
	 */
	if (acct_bound_server(server, sizeof(server))) {
		if (server[0] != '\0')
			warnx("this machine is joined to %s; change the "
			    "password there", server);
		else
			warnx("this machine is joined to a directory server; "
			    "change the password there");
		return (ACCT_EX_REFUSED);
	}

	store = find_store(name, forced);
	switch (store) {
	case STORE_DIRECTORY:
		return (do_directory(name, act, ruid));
	case STORE_FILES:
		return (do_files(name, act, ruid));
	case STORE_NONE:
		warnx("%s: no such user", name);
		return (1);
	}
	return (1);
}
