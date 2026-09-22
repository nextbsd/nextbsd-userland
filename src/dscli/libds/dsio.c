/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
 * Ported from Gershwin's dscli by Joe Maloney
 * (gershwin-desktop/gershwin-components, DirectoryServices/dscli, BSD-2-Clause).
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
 * Files, locking, homes and child processes.
 */

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"

/* execvp takes char *const argv[]; these literals are never written. */
#define ARGV(s)	((char *)(uintptr_t)(s))

int
ds_lock(const char *dir)
{
	char path[PATH_MAX];
	int fd;

	if (ds_path(path, sizeof(path), dir, DS_LOCK_FILE) == -1)
		return (-1);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd == -1)
		return (-1);
	if (flock(fd, LOCK_EX) == -1) {
		int saved = errno;
		(void)close(fd);
		errno = saved;
		return (-1);
	}
	return (fd);
}

void
ds_unlock(int fd)
{
	if (fd >= 0) {
		(void)flock(fd, LOCK_UN);
		(void)close(fd);
	}
}

/*
 * The temp file lives next to the target so rename(2) is atomic; the
 * NSS module therefore never sees a half-written plist, and a reader that
 * already opened the old inode keeps reading the old file.
 */
int
ds_write_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
	char tmp[PATH_MAX];
	const char *slash;
	size_t have = 0;
	ssize_t wrote;
	int fd, rv, saved;

	slash = strrchr(path, '/');
	rv = snprintf(tmp, sizeof(tmp), "%.*s.%s.XXXXXX",
	    slash != NULL ? (int)(slash - path + 1) : 0, path,
	    slash != NULL ? slash + 1 : path);
	if (rv < 0 || (size_t)rv >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	fd = mkstemp(tmp);
	if (fd == -1)
		return (-1);
	while (have < len) {
		wrote = write(fd, data + have, len - have);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			goto fail;
		have += (size_t)wrote;
	}
	if (fchmod(fd, mode) == -1 || fsync(fd) == -1)
		goto fail;
	if (close(fd) == -1) {
		fd = -1;
		goto fail;
	}
	if (rename(tmp, path) == -1) {
		fd = -1;
		goto fail;
	}
	return (0);
fail:
	saved = errno;
	if (fd != -1)
		(void)close(fd);
	(void)unlink(tmp);
	errno = saved;
	return (-1);
}

char *
ds_read_file(const char *path, size_t *len)
{
	struct stat st;
	char *buf;
	size_t have = 0;
	ssize_t got;
	int fd, saved;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return (NULL);
	if (fstat(fd, &st) == -1) {
		saved = errno;
		(void)close(fd);
		errno = saved;
		return (NULL);
	}
	if (!S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
		(void)close(fd);
		errno = EINVAL;
		return (NULL);
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		(void)close(fd);
		return (NULL);
	}
	while (have < (size_t)st.st_size) {
		got = read(fd, buf + have, (size_t)st.st_size - have);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		have += (size_t)got;
	}
	(void)close(fd);
	buf[have] = '\0';
	if (len != NULL)
		*len = have;
	return (buf);
}

int
ds_mkdirs(const char *path, mode_t mode)
{
	char work[PATH_MAX];
	char *p;
	struct stat st;

	if (strlen(path) >= sizeof(work)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	strcpy(work, path);
	for (p = work + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(work, mode) == -1 && errno != EEXIST)
			return (-1);
		*p = '/';
	}
	if (mkdir(work, mode) == -1) {
		if (errno != EEXIST)
			return (-1);
		if (stat(work, &st) == -1)
			return (-1);
		if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return (-1);
		}
	}
	return (0);
}

static int
copy_file(const char *from, const char *to, uid_t uid, gid_t gid)
{
	char *data;
	size_t len;
	int rv;

	data = ds_read_file(from, &len);
	if (data == NULL)
		return (-1);
	rv = ds_write_atomic(to, data, len, 0644);
	free(data);
	if (rv == -1)
		return (-1);
	return (chown(to, uid, gid));
}

int
ds_make_home(const char *home, uid_t uid, gid_t gid)
{
	static const char *const skel[][2] = {
		{ "dot.zshrc", ".zshrc" },
		{ "dot.zprofile", ".zprofile" },
		{ "dot.profile", ".profile" },
		{ "dot.shrc", ".shrc" },
	};
	char from[PATH_MAX], to[PATH_MAX], sysskel[PATH_MAX];
	struct stat st;
	size_t i;

	if (mkdir(home, 0755) == -1) {
		if (errno != EEXIST)
			return (-1);
		if (stat(home, &st) == -1)
			return (-1);
		if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return (-1);
		}
	}
	ds_syspath(sysskel, sizeof(sysskel), DS_SKEL_DIR);
	for (i = 0; i < sizeof(skel) / sizeof(skel[0]); i++) {
		if (ds_path(from, sizeof(from), sysskel, skel[i][0]) == -1 ||
		    ds_path(to, sizeof(to), home, skel[i][1]) == -1)
			return (-1);
		if (stat(from, &st) == -1 || stat(to, &st) == 0)
			continue;	/* no skeleton file, or the user has one */
		if (copy_file(from, to, uid, gid) == -1)
			return (-1);
	}
	return (chown(home, uid, gid));
}

static int
rm_tree(const char *path)
{
	DIR *d;
	struct dirent *e;
	struct stat st;
	char child[PATH_MAX];

	if (lstat(path, &st) == -1)
		return (errno == ENOENT ? 0 : -1);
	if (!S_ISDIR(st.st_mode))
		return (unlink(path));
	d = opendir(path);
	if (d == NULL)
		return (-1);
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (ds_path(child, sizeof(child), path, e->d_name) == -1 ||
		    rm_tree(child) == -1) {
			(void)closedir(d);
			return (-1);
		}
	}
	(void)closedir(d);
	return (rmdir(path));
}

int
ds_remove_home(const char *home)
{
	char base[PATH_MAX];
	size_t n;

	ds_syspath(base, sizeof(base), DS_LOCAL_USERS);
	n = strlen(base);
	if (strncmp(home, base, n) != 0 || home[n] != '/' ||
	    home[n + 1] == '\0' || strstr(home, "/../") != NULL) {
		errno = EPERM;
		return (-1);
	}
	return (rm_tree(home));
}

int
ds_run(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid == -1)
		return (-1);
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) == -1) {
		if (errno != EINTR)
			return (-1);
	}
	if (!WIFEXITED(status)) {
		errno = EINTR;
		return (-1);
	}
	if (WEXITSTATUS(status) == 127) {
		errno = ENOENT;
		return (-1);
	}
	return (WEXITSTATUS(status));
}

static int
job_plist(char *buf, size_t len, const char *label)
{
	char dir[PATH_MAX];
	int rv;

	ds_syspath(dir, sizeof(dir), DS_LAUNCHDAEMONS);
	rv = snprintf(buf, len, "%s/%s.plist", dir, label);
	if (rv < 0 || (size_t)rv >= len) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	return (0);
}

bool
ds_job_installed(const char *label)
{
	char path[PATH_MAX];
	struct stat st;

	return (job_plist(path, sizeof(path), label) == 0 &&
	    stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

int
ds_launchctl(const char *verb, bool persist, const char *label)
{
	char path[PATH_MAX];
	char *argv[5];
	int i = 0;

	if (job_plist(path, sizeof(path), label) == -1)
		return (-1);
	argv[i++] = ARGV("launchctl");
	argv[i++] = ARGV(verb);
	if (persist)
		argv[i++] = ARGV("-w");
	argv[i++] = path;
	argv[i] = NULL;
	return (ds_run(argv));
}

bool
ds_job_loaded(const char *label)
{
	char *argv[4];
	pid_t pid;
	int status, devnull;

	argv[0] = ARGV("launchctl");
	argv[1] = ARGV("list");
	argv[2] = ARGV(label);
	argv[3] = NULL;
	pid = fork();
	if (pid == -1)
		return (false);
	if (pid == 0) {
		devnull = open("/dev/null", O_RDWR);
		if (devnull != -1) {
			(void)dup2(devnull, STDOUT_FILENO);
			(void)dup2(devnull, STDERR_FILENO);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR)
			return (false);
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
