/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Running NextBSD's own tools with fixed argument vectors: posix_spawn,
 * never a shell, stdin from /dev/null, with a clean environment.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

static char *const clean_env[] = {
	"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
	"LANG=C",
	"LC_ALL=C",
	NULL
};

static void
grow(char **b, size_t *n, size_t *cap, const char *d, size_t dn)
{
	if (*n + dn + 1 > *cap) {
		*cap = (*n + dn + 1) * 2 + 4096;
		if ((*b = realloc(*b, *cap)) == NULL)
			sa_fail("failed", "out of memory");
	}
	memcpy(*b + *n, d, dn);
	*n += dn;
	(*b)[*n] = '\0';
}

bool
sa_run(const char *const argv[], struct sa_out *o)
{
	posix_spawn_file_actions_t fa;
	int op[2], ep[2], st;
	pid_t pid;
	size_t ocap = 0, ecap = 0, elen = 0;
	struct pollfd p[2];
	char t[8192];
	ssize_t r;

	memset(o, 0, sizeof(*o));
	o->status = -1;
	grow(&o->out, &o->outlen, &ocap, "", 0);
	grow(&o->err, &elen, &ecap, "", 0);
	if (pipe(op) == -1 || pipe(ep) == -1)
		return (false);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, op[1], 1);
	posix_spawn_file_actions_adddup2(&fa, ep[1], 2);
	posix_spawn_file_actions_addclose(&fa, op[0]);
	posix_spawn_file_actions_addclose(&fa, ep[0]);
	if (posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv,
	    clean_env) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
		snprintf(t, sizeof(t), "%s: %s", argv[0], strerror(errno));
		grow(&o->err, &elen, &ecap, t, strlen(t));
		return (false);
	}
	posix_spawn_file_actions_destroy(&fa);
	close(op[1]);
	close(ep[1]);
	p[0].fd = op[0]; p[0].events = POLLIN;
	p[1].fd = ep[0]; p[1].events = POLLIN;
	while (p[0].fd != -1 || p[1].fd != -1) {
		if (poll(p, 2, -1) == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (int i = 0; i < 2; i++) {
			if (p[i].fd == -1 || p[i].revents == 0)
				continue;
			r = read(p[i].fd, t, sizeof(t));
			if (r <= 0) {
				close(p[i].fd);
				p[i].fd = -1;
				continue;
			}
			if (i == 0)
				grow(&o->out, &o->outlen, &ocap, t, (size_t)r);
			else if (elen < 65536)
				grow(&o->err, &elen, &ecap, t, (size_t)r);
		}
	}
	while (waitpid(pid, &st, 0) == -1 && errno == EINTR)
		;
	o->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
	return (o->status == 0);
}

bool
sa_run_stdio(const char *const argv[], char **err)
{
	posix_spawn_file_actions_t fa;
	int ep[2], st;
	pid_t pid;
	size_t ecap = 0, elen = 0;
	char t[4096];
	ssize_t r;

	*err = NULL;
	grow(err, &elen, &ecap, "", 0);
	if (pipe(ep) == -1)
		return (false);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, ep[1], 2);
	posix_spawn_file_actions_addclose(&fa, ep[0]);
	if (posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv,
	    clean_env) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		close(ep[0]); close(ep[1]);
		return (false);
	}
	posix_spawn_file_actions_destroy(&fa);
	close(ep[1]);
	while ((r = read(ep[0], t, sizeof(t))) > 0)
		if (elen < 65536)
			grow(err, &elen, &ecap, t, (size_t)r);
	close(ep[0]);
	while (waitpid(pid, &st, 0) == -1 && errno == EINTR)
		;
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

void
sa_out_free(struct sa_out *o)
{
	free(o->out);
	free(o->err);
	memset(o, 0, sizeof(*o));
}

void
sa_must(const char *what, const char *const argv[])
{
	struct sa_out o;
	char *e;
	size_t n;

	if (sa_run(argv, &o)) {
		sa_out_free(&o);
		return;
	}
	e = o.err;
	n = strlen(e);
	while (n > 0 && (e[n - 1] == '\n' || e[n - 1] == ' '))
		e[--n] = '\0';
	sa_fail("failed", "%s: %s", what, n > 0 ? e : "failed");
}

char **
sa_lines(char *b, int *n)
{
	char **v = NULL, *l;
	int c = 0, cap = 0;

	while ((l = strsep(&b, "\n")) != NULL) {
		if (*l == '\0')
			continue;
		if (c + 1 >= cap) {
			cap = cap * 2 + 16;
			if ((v = realloc(v, sizeof(*v) * (size_t)cap)) == NULL)
				sa_fail("failed", "out of memory");
		}
		v[c++] = l;
	}
	if (v == NULL && (v = calloc(1, sizeof(*v))) == NULL)
		sa_fail("failed", "out of memory");
	v[c] = NULL;
	*n = c;
	return (v);
}

char **
sa_fields(char *l, const char *sep, int *n)
{
	char **v = NULL, *f;
	int c = 0, cap = 0;

	while ((f = strsep(&l, sep)) != NULL) {
		if (c + 1 >= cap) {
			cap = cap * 2 + 8;
			if ((v = realloc(v, sizeof(*v) * (size_t)cap)) == NULL)
				sa_fail("failed", "out of memory");
		}
		v[c++] = f;
	}
	if (v == NULL && (v = calloc(1, sizeof(*v))) == NULL)
		sa_fail("failed", "out of memory");
	v[c] = NULL;
	*n = c;
	return (v);
}
