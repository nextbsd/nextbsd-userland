/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin pair: Server's key in root's authorized_keys, so a
 * password is asked for at most once per computer.
 *
 *   pair --add-key "ssh-ed25519 AAAA... comment"
 *   pair --list
 *   pair --remove SHA256:...
 */

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

#define	SSH_DIR		"/root/.ssh"
#define	AUTH_KEYS	SSH_DIR "/authorized_keys"
#define	SSH_KEYGEN	"/usr/bin/ssh-keygen"

/* "ssh-ed25519 <base64> [comment]", the comment plain text only. */
static bool
valid_key(const char *k)
{
	const char *p;

	if (strncmp(k, "ssh-ed25519 ", 12) != 0)
		return (false);
	p = k + 12;
	if (strncmp(p, "AAAAC3NzaC1lZDI1NTE5", 20) != 0)
		return (false);
	while (*p != '\0' && *p != ' ') {
		if (!(isalnum((unsigned char)*p) || *p == '+' || *p == '/' ||
		    *p == '='))
			return (false);
		p++;
	}
	if (p - (k + 12) < 60 || p - (k + 12) > 80)
		return (false);
	for (; *p != '\0'; p++)
		if (!(isalnum((unsigned char)*p) || strchr(" ._@-()", *p)))
			return (false);
	return (strlen(k) < 512);
}

/* The key part ("ssh-ed25519 AAAA..."), without the comment. */
static void
key_part(const char *line, char *out, size_t len)
{
	const char *sp = strchr(line, ' ');

	strlcpy(out, line, len);
	if (sp != NULL && (sp = strchr(sp + 1, ' ')) != NULL &&
	    (size_t)(sp - line) < len)
		out[sp - line] = '\0';
	out[strcspn(out, "\n")] = '\0';
}

static char *
read_all(const char *path)
{
	FILE *f;
	char *b = NULL;
	size_t n = 0;
	ssize_t r;
	char t[4096];

	if ((f = fopen(path, "r")) == NULL)
		return (strdup(""));
	while ((r = (ssize_t)fread(t, 1, sizeof(t), f)) > 0) {
		if ((b = realloc(b, n + (size_t)r + 1)) == NULL)
			sa_fail("failed", "out of memory");
		memcpy(b + n, t, (size_t)r);
		n += (size_t)r;
	}
	fclose(f);
	if (b == NULL)
		return (strdup(""));
	b[n] = '\0';
	return (b);
}

static void
write_all(const char *text)
{
	char tmp[] = AUTH_KEYS ".XXXXXX";
	int fd;
	size_t n = strlen(text);

	(void)mkdir(SSH_DIR, 0700);
	if ((fd = mkstemp(tmp)) == -1)
		sa_fail("failed", "%s: %s", SSH_DIR, strerror(errno));
	if (fchmod(fd, 0600) == -1 || write(fd, text, n) != (ssize_t)n ||
	    fsync(fd) == -1 || close(fd) == -1 || rename(tmp, AUTH_KEYS) == -1) {
		(void)unlink(tmp);
		sa_fail("failed", "%s: %s", AUTH_KEYS, strerror(errno));
	}
}

/* ssh-keygen -lf on one key, through a temporary file. */
static void
fingerprint(const char *line, char *fp, size_t len, char *comment, size_t clen)
{
	char tmp[] = "/tmp/serveradmin-key.XXXXXX";
	const char *argv[] = { SSH_KEYGEN, "-l", "-E", "sha256", "-f", tmp, NULL };
	struct sa_out o;
	int fd, nf;
	char **f;

	fp[0] = comment[0] = '\0';
	if ((fd = mkstemp(tmp)) == -1)
		return;
	(void)write(fd, line, strlen(line));
	(void)write(fd, "\n", 1);
	close(fd);
	if (sa_run(argv, &o)) {
		o.out[strcspn(o.out, "\n")] = '\0';
		f = sa_fields(o.out, " ", &nf);
		if (nf >= 2)
			strlcpy(fp, f[1], len);
		if (nf >= 3) {
			/* "256 SHA256:... comment words (ED25519)" */
			comment[0] = '\0';
			for (int i = 2; i < nf - 1; i++) {
				if (comment[0] != '\0')
					strlcat(comment, " ", clen);
				strlcat(comment, f[i], clen);
			}
		}
		free(f);
	}
	sa_out_free(&o);
	(void)unlink(tmp);
}

void
cmd_pair(int argc, char **argv)
{
	char *text, **l, mine[512], theirs[512], fp[128], cm[256];
	int n;

	if (argc == 2 && strcmp(argv[0], "--add-key") == 0) {
		if (!valid_key(argv[1]))
			sa_usage("not one ed25519 public key");
		key_part(argv[1], mine, sizeof(mine));
		text = read_all(AUTH_KEYS);
		{
			char *copy = strdup(text);
			l = sa_lines(copy, &n);
			for (int i = 0; i < n; i++) {
				key_part(l[i], theirs, sizeof(theirs));
				if (strcmp(mine, theirs) == 0) {
					j_obj();
					j_kbool("added", false);
					j_kbool("present", true);
					j_end();
					j_flush();
					return;
				}
			}
			free(l);
			free(copy);
		}
		{
			size_t tl = strlen(text);
			char *nt = malloc(tl + strlen(argv[1]) + 3);
			if (nt == NULL)
				sa_fail("failed", "out of memory");
			strcpy(nt, text);
			if (tl > 0 && text[tl - 1] != '\n')
				strcat(nt, "\n");
			strcat(nt, argv[1]);
			strcat(nt, "\n");
			write_all(nt);
		}
		j_obj();
		j_kbool("added", true);
		j_kbool("present", true);
		j_end();
		j_flush();
		return;
	}
	if (argc == 1 && strcmp(argv[0], "--list") == 0) {
		text = read_all(AUTH_KEYS);
		l = sa_lines(text, &n);
		j_obj();
		j_key("keys");
		j_arr();
		for (int i = 0; i < n; i++) {
			if (l[i][0] == '#')
				continue;
			fingerprint(l[i], fp, sizeof(fp), cm, sizeof(cm));
			j_obj();
			j_kstr("fingerprint", fp);
			j_kstr("comment", cm);
			j_end();
		}
		j_aend();
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "--remove") == 0) {
		char *out;
		size_t ol = 0;
		bool removed = false;

		if (strncmp(argv[1], "SHA256:", 7) != 0 || strlen(argv[1]) > 100)
			sa_usage("not a SHA256 fingerprint");
		text = read_all(AUTH_KEYS);
		if ((out = calloc(1, strlen(text) + 2)) == NULL)
			sa_fail("failed", "out of memory");
		l = sa_lines(text, &n);
		for (int i = 0; i < n; i++) {
			fingerprint(l[i], fp, sizeof(fp), cm, sizeof(cm));
			if (strcmp(fp, argv[1]) == 0) {
				removed = true;
				continue;
			}
			strcpy(out + ol, l[i]);
			ol += strlen(l[i]);
			out[ol++] = '\n';
		}
		if (removed)
			write_all(out);
		j_obj();
		j_kbool("removed", removed);
		j_end();
		j_flush();
		return;
	}
	sa_usage("usage: serveradmin pair --add-key KEY | --list | --remove FINGERPRINT");
}
