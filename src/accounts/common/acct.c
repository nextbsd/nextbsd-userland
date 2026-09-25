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

/* See acct.h. Plain libc plus libutil for login.conf. */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
/*
 * struct kinfo_proc: <sys/user.h> on FreeBSD, where <sys/sysctl.h> only
 * forward-declares it. Darwin defines it in <sys/sysctl.h> itself. Leaving
 * this out compiled on the host and broke every tool on the target, because
 * this file is shared by all four of them.
 */
#ifdef __FreeBSD__
#include <sys/user.h>
#endif
#include <sys/wait.h>

#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <utmpx.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * login.conf lives in libutil, which is FreeBSD's. On a host build for the
 * unit tests there is no login_cap.h, so the policy lookup compiles out and
 * the default applies. The target always has it.
 */
#ifdef __FreeBSD__
#include <login_cap.h>
#define ACCT_HAVE_LOGIN_CAP 1
#endif

#include "acct.h"

/*
 * kinfo_proc names its pid field differently on the two systems this builds
 * for: ki_pid on FreeBSD, kp_proc.p_pid on Darwin. The host build only has
 * to compile and run the tests; the target is FreeBSD.
 */
#ifdef __FreeBSD__
#define ACCT_KP_PID	ki_pid
#else
#define ACCT_KP_PID	kp_proc.p_pid
#endif

/*
 * A salt for SHA-512 crypt: 16 characters from crypt(3)'s alphabet, which
 * is what Gershwin's dscli uses and long enough that the setting string
 * carries the full 96 bits crypt allows.
 */
static void
make_salt(char *out, size_t len)
{
	static const char alphabet[] = "./0123456789"
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	size_t i;

	for (i = 0; i + 1 < len; i++)
		out[i] = alphabet[arc4random_uniform(sizeof(alphabet) - 1)];
	out[i] = '\0';
}

/*
 * The crypt(3) setting prefix for a passwd_format name. Only formats the
 * base crypt(3) actually implements are accepted; anything else falls back
 * to SHA-512 rather than silently producing a weaker hash than asked for.
 */
static const char *
setting_prefix(const char *format)
{
	if (format == NULL)
		return ("$6$");
	if (strcmp(format, "sha512") == 0)
		return ("$6$");
	if (strcmp(format, "sha256") == 0)
		return ("$5$");
	if (strcmp(format, "blf") == 0 || strcmp(format, "blowfish") == 0)
		return ("$2b$");
	return ("$6$");
}

bool
acct_hash_password(const char *password, char *out, size_t len)
{
	char stored[512];
	char salt[17], setting[64];
	const char *format = NULL, *prefix;
	char *hash;

	if (password == NULL || out == NULL || len == 0)
		return (false);
	out[0] = '\0';

	/*
	 * login.conf's passwd_format is the one place the policy is stated,
	 * so read it rather than hard-coding it here. The default class is
	 * enough: the plists carry no login class, by design.
	 */
#ifdef ACCT_HAVE_LOGIN_CAP
	{
		login_cap_t *lc;

		if ((lc = login_getclass(NULL)) != NULL) {
			format = login_getcapstr(lc, "passwd_format", NULL,
			    NULL);
			login_close(lc);
		}
	}
#endif
	prefix = setting_prefix(format);
	(void)snprintf(setting, sizeof(setting), "%s", prefix);

	make_salt(salt, sizeof(salt));
	(void)strlcat(setting, salt, sizeof(setting));
	(void)strlcat(setting, "$", sizeof(setting));

	if ((hash = crypt(password, setting)) == NULL)
		return (false);
	if (strlcpy(stored, hash, sizeof(stored)) >= sizeof(stored))
		return (false);
	/*
	 * Refuse anything that is not the format we asked for.
	 *
	 * A crypt(3) that does not implement the requested algorithm does not
	 * fail: it falls back, and takes the leading bytes of the setting as a
	 * DES salt. Asking for "$6$..." on such a system yields "$6XXXXXXXXXXX",
	 * thirteen characters of DES that still begin with a dollar sign. So
	 * checking for a dollar sign is not enough; the prefix has to match.
	 *
	 * Getting this wrong would store a DES hash while every other part of
	 * the system believed it was SHA-512, which is the kind of downgrade
	 * nothing downstream would notice.
	 */
	{
		size_t plen = strlen(prefix);

		if (strncmp(stored, prefix, plen) != 0 ||
		    strlen(stored) <= plen) {
			warnx("crypt(3) does not implement %s on this system; "
			    "refusing to store a weaker hash", prefix);
			acct_zero(stored, sizeof(stored));
			return (false);
		}
	}
	if (strlcpy(out, stored, len) >= len) {
		acct_zero(stored, sizeof(stored));
		acct_zero(out, len);
		return (false);
	}
	acct_zero(stored, sizeof(stored));
	return (true);
}

const char *
acct_name_problem(const char *n)
{
	size_t i, len;

	if (n == NULL || n[0] == '\0')
		return ("empty");
	len = strlen(n);
	if (len >= 32)
		return ("longer than 31 characters");
	if (isdigit((unsigned char)n[0]))
		return ("starts with a digit");
	if (n[0] == '-')
		return ("starts with a dash");
	if (n[0] == '.')
		return ("starts with a dot");
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)n[i];

		if (isalnum(c) || c == '.' || c == '_' || c == '-')
			continue;
		return ("has a character outside A-Z a-z 0-9 . _ -");
	}
	return (NULL);
}

bool
acct_hash_locked(const char *hash)
{
	return (hash != NULL &&
	    strncmp(hash, ACCT_LOCK_PREFIX, sizeof(ACCT_LOCK_PREFIX) - 1) == 0);
}

bool
acct_verify_password(const char *plain, const char *stored)
{
	char *computed;
	size_t i, n;
	unsigned char diff = 0;

	if (plain == NULL || stored == NULL || stored[0] == '\0')
		return (false);
	if (acct_hash_locked(stored))
		return (false);
	if ((computed = crypt(plain, stored)) == NULL)
		return (false);
	/*
	 * Constant-time in the length of the stored hash. Comparing lengths
	 * first would leak whether the formats match, so mismatched lengths
	 * still walk the whole buffer and simply cannot come out equal.
	 */
	n = strlen(stored);
	if (strlen(computed) != n)
		diff = 1;
	for (i = 0; i < n; i++)
		diff |= (unsigned char)computed[i] ^ (unsigned char)stored[i];
	return (diff == 0);
}

bool
acct_is_system_name(const char *name)
{
	return (name != NULL && name[0] == '_');
}

bool
acct_is_system_id(uid_t id)
{
	return (id <= ACCT_SYSTEM_MAX);
}

bool
acct_shell_listed(const char *shell)
{
	FILE *f;
	char line[1024];
	char *p;
	bool found = false;

	if (shell == NULL || shell[0] == '\0')
		return (false);
	if ((f = fopen(ACCT_SHELLS, "r")) == NULL)
		return (true);		/* cannot tell: do not block */
	while (fgets(line, sizeof(line), f) != NULL) {
		if ((p = strchr(line, '#')) != NULL)
			*p = '\0';
		line[strcspn(line, "\r\n")] = '\0';
		for (p = line; *p == ' ' || *p == '\t'; p++)
			;
		if (*p == '\0')
			continue;
		if (strcmp(p, shell) == 0) {
			found = true;
			break;
		}
	}
	(void)fclose(f);
	return (found);
}

/*
 * Are we a domain-joined system right now?
 *
 * The answer is whether /Network/Library/DirectoryServices/Users.plist is
 * there, and nothing else. Not the bare /Network directory -- that exists as an
 * empty mount point whether or not anything is mounted on it. The full path is
 * the same one nss_directory_services stats (it prefers the /Network plists
 * when they are there and falls back to /Local otherwise) and the same one
 * autologin-user checks, so all three agree on one question.
 *
 * If it is absent -- never joined, or the mount is not up yet at boot -- the
 * machine simply behaves as a non-joined system. That is the designed
 * fallback, not a failure.
 *
 * This used to test whether Binding.plist opened, i.e. "am I configured as
 * joined" rather than "am I joined". That was both the wrong question and
 * unreachable in practice: passwd, chpass and pw route to master.passwd as
 * soon as a name misses the local plists, which is exactly what happens on a
 * real client, so the check sat behind the routing and never ran. Keying off
 * the mount makes it true on a client regardless of where the account lives,
 * so it can be asked first.
 *
 * Binding.plist is still read, but only to name the server in the message --
 * never to decide. The value we need is the string after <key>server</key>,
 * and scanning for it does not justify linking CoreFoundation into every
 * account tool.
 */
bool
acct_bound_server(char *server, size_t len)
{
	FILE *f;
	char line[1024], *p, *q;
	bool in_key = false, bound = false;

	if (server != NULL && len > 0)
		server[0] = '\0';
	if (access(ACCT_NETWORK_USERS, F_OK) != 0)
		return (false);		/* no network plists: not joined */
	bound = true;
	if ((f = fopen(ACCT_BINDING_PLIST, "r")) == NULL)
		return (true);		/* joined, but we cannot name it */
	while (fgets(line, sizeof(line), f) != NULL) {
		if (!in_key) {
			if ((p = strstr(line, "<key>server</key>")) == NULL)
				continue;
			in_key = true;
			/*
			 * The value may follow on the same line, as a plist
			 * written by hand or by plutil -convert often does,
			 * or on the next one. Look here first, then fall
			 * through to the following lines.
			 */
			p += sizeof("<key>server</key>") - 1;
		} else
			p = line;
		if ((p = strstr(p, "<string>")) == NULL)
			continue;
		p += sizeof("<string>") - 1;
		if ((q = strstr(p, "</string>")) == NULL)
			break;
		*q = '\0';
		if (server != NULL && len > 0)
			(void)strlcpy(server, p, len);
		break;
	}
	(void)fclose(f);
	return (bound);
}

void
acct_zero(void *buf, size_t len)
{
	if (buf == NULL || len == 0)
		return;
#if defined(__FreeBSD__) || defined(__OpenBSD__)
	explicit_bzero(buf, len);
#else
	{
		volatile unsigned char *p = buf;

		while (len-- > 0)
			*p++ = 0;
	}
#endif
}

int
acct_make_home(const char *user, bool quiet)
{
	pid_t pid;
	int status;

	if (user == NULL)
		return (-1);
	if (access(ACCT_CREATEHOMEDIR, X_OK) == -1) {
		warnx("%s is not available; the home was not created",
		    ACCT_CREATEHOMEDIR);
		return (-1);
	}
	if ((pid = fork()) == -1) {
		warnx("fork: %s", strerror(errno));
		return (-1);
	}
	if (pid == 0) {
		if (quiet) {
			int fd = open(_PATH_DEVNULL, O_WRONLY);

			if (fd != -1) {
				(void)dup2(fd, STDOUT_FILENO);
				if (fd > STDERR_FILENO)
					(void)close(fd);
			}
		}
		execl(ACCT_CREATEHOMEDIR, "createhomedir", "-u", user,
		    (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR)
			return (-1);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		warnx("createhomedir failed for %s; run it again by hand",
		    user);
		return (-1);
	}
	return (0);
}

/* ---- what rmuser needs beyond the record itself --------------------- */

/*
 * A name safe to paste into a path. Stricter than acct_name_problem: no
 * dots at all, so neither "." nor ".." nor anything containing a slash can
 * reach a path we are about to delete. A real account name never needs one
 * here, and the cost of being wrong is a recursive delete of the wrong
 * directory.
 */
static bool
name_ok_for_path(const char *n)
{
	size_t i;

	if (n == NULL || n[0] == '\0' || strlen(n) >= 33)
		return (false);
	for (i = 0; n[i] != '\0'; i++) {
		unsigned char c = (unsigned char)n[i];

		if (isalnum(c) || c == '_' || c == '-')
			continue;
		return (false);
	}
	return (true);
}

/* Remove a directory tree. No fork, no rm(1), nothing to quote wrongly. */
static int
remove_tree(const char *path)
{
	DIR *d;
	struct dirent *de;
	char child[PATH_MAX];
	struct stat st;
	int rc = 0;

	if ((d = opendir(path)) == NULL)
		return (-1);
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path,
		    de->d_name) >= (int)sizeof(child)) {
			rc = -1;
			continue;
		}
		if (lstat(child, &st) == -1) {
			rc = -1;
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (remove_tree(child) == -1)
				rc = -1;
		} else if (unlink(child) == -1) {
			rc = -1;
		}
	}
	(void)closedir(d);
	if (rmdir(path) == -1)
		rc = -1;
	return (rc);
}

int
acct_sessions(const char *name)
{
	struct utmpx *u;
	int n = 0;

	if (name == NULL)
		return (0);
	setutxent();
	while ((u = getutxent()) != NULL)
		if (u->ut_type == USER_PROCESS &&
		    strncmp(u->ut_user, name, sizeof(u->ut_user)) == 0)
			n++;
	endutxent();
	return (n);
}

int
acct_kill_uid(uid_t uid)
{
	struct kinfo_proc *procs = NULL;
	size_t len = 0;
	int mib[4], n = 0;
	unsigned int i, count;
	pid_t self = getpid(), parent = getppid();

	/*
	 * sysctl rather than shelling out to pkill: one fewer thing that has
	 * to exist on the image, and no pattern matching to get wrong. The
	 * KERN_PROC_UID form is the same on both systems this builds for.
	 */
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_UID;
	mib[3] = (int)uid;
	if (sysctl(mib, 4, NULL, &len, NULL, 0) == -1)
		return (-1);
	if (len == 0)
		return (0);
	/* Room for the table to grow between the sizing call and the read. */
	len += len / 8 + sizeof(*procs);
	if ((procs = malloc(len)) == NULL)
		return (-1);
	if (sysctl(mib, 4, procs, &len, NULL, 0) == -1) {
		free(procs);
		return (-1);
	}
	count = (unsigned int)(len / sizeof(*procs));
	for (i = 0; i < count; i++) {
		pid_t pid = procs[i].ACCT_KP_PID;

		if (pid <= 1 || pid == self || pid == parent)
			continue;
		if (kill(pid, SIGKILL) == 0)
			n++;
	}
	free(procs);
	return (n);
}

int
acct_remove_home(const char *name)
{
	char path[PATH_MAX];
	struct stat st;
	const char *roots[] = { ACCT_LOCAL_USERS, "/Network/Users", NULL };
	size_t i;

	if (!name_ok_for_path(name))
		return (-1);
	for (i = 0; roots[i] != NULL; i++) {
		if (snprintf(path, sizeof(path), "%s/%s", roots[i], name) >=
		    (int)sizeof(path))
			return (-1);
		if (lstat(path, &st) == -1)
			continue;
		/*
		 * A symlink where a home should be is not something to follow
		 * and delete the target of.
		 */
		if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return (-1);
		}
		return (remove_tree(path));
	}
	return (1);		/* nothing there */
}

int
acct_remove_cron(const char *name)
{
	char path[PATH_MAX];

	if (!name_ok_for_path(name))
		return (-1);
	if (snprintf(path, sizeof(path), "%s/%s", ACCT_CRON_TABS, name) >=
	    (int)sizeof(path))
		return (-1);
	if (unlink(path) == -1 && errno != ENOENT)
		return (-1);
	return (0);
}

int
acct_remove_at(const char *name)
{
	char path[PATH_MAX];
	DIR *d;
	struct dirent *de;
	struct passwd *pw;
	struct stat st;
	int rc = 0;

	if (!name_ok_for_path(name) || (pw = getpwnam(name)) == NULL)
		return (0);
	if ((d = opendir(ACCT_AT_JOBS)) == NULL)
		return (errno == ENOENT ? 0 : -1);
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", ACCT_AT_JOBS,
		    de->d_name) >= (int)sizeof(path))
			continue;
		if (lstat(path, &st) == -1 || !S_ISREG(st.st_mode))
			continue;
		if (st.st_uid != pw->pw_uid)
			continue;
		if (unlink(path) == -1 && errno != ENOENT)
			rc = -1;
	}
	(void)closedir(d);
	return (rc);
}
