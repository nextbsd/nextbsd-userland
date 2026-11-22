/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_file.c — what the operations share: reading a small file whole,
 * replacing one, and running a program.
 *
 * A file is replaced by writing a temporary next to it and renaming it
 * over, so a reader never sees half a file. Programs are run with fork
 * and exec, never a shell; their arguments are the helper's own.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	MAX_FILE	(1024 * 1024)	/* a zone file is a few KB */

bool
sh_run(const char *const argv[])
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid == -1)
		return (false);
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd != -1) {
			(void)dup2(fd, STDIN_FILENO);
			(void)dup2(fd, STDOUT_FILENO);
			if (fd > STDERR_FILENO)
				(void)close(fd);
		}
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) == -1)
		if (errno != EINTR)
			return (false);
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

char *
sh_read_file(const char *path, size_t *len)
{
	struct stat st;
	char *buf;
	ssize_t n;
	size_t got = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return (NULL);
	if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
	    st.st_size > MAX_FILE) {
		(void)close(fd);
		return (NULL);
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		(void)close(fd);
		return (NULL);
	}
	while (got < (size_t)st.st_size) {
		n = read(fd, buf + got, (size_t)st.st_size - got);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	(void)close(fd);
	buf[got] = '\0';
	*len = got;
	return (buf);
}

bool
sh_write_file(const char *path, const char *data, size_t len)
{
	char tmp[PATH_MAX];
	ssize_t n;
	size_t done = 0;
	int fd;

	(void)snprintf(tmp, sizeof(tmp), "%s.SettingsHelper", path);
	(void)unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd == -1) {
		sh_log("%s: %s", tmp, strerror(errno));
		return (false);
	}
	while (done < len) {
		n = write(fd, data + done, len - done);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += (size_t)n;
	}
	if (done != len || fchmod(fd, 0644) == -1 || fsync(fd) == -1) {
		sh_log("%s: %s", tmp, strerror(errno));
		(void)close(fd);
		(void)unlink(tmp);
		return (false);
	}
	(void)close(fd);
	if (rename(tmp, path) == -1) {
		sh_log("%s: %s", path, strerror(errno));
		(void)unlink(tmp);
		return (false);
	}
	return (true);
}
