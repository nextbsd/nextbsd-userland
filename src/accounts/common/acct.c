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
#include <sys/wait.h>

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
 * The binding file is a plist, but reading one key out of it does not
 * justify linking CoreFoundation into every account tool. The value we
 * need is the string after <key>server</key>, so scan for it. A binding
 * we cannot parse still counts as bound: refusing is the safe answer,
 * because the alternative is creating a local account that the /Network
 * plists will shadow.
 */
bool
acct_bound_server(char *server, size_t len)
{
	FILE *f;
	char line[1024], *p, *q;
	bool in_key = false, bound = false;

	if (server != NULL && len > 0)
		server[0] = '\0';
	if ((f = fopen(ACCT_BINDING_PLIST, "r")) == NULL)
		return (false);		/* no binding file: not bound */
	bound = true;
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
