/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_install.c — InstallStart, InstallProgress and InstallRestart:
 * Installer.app's operations (nextbsd/nextbsd#670).
 *
 * The install itself is the text installer's engine,
 * /usr/libexec/nextbsd-installer: probe-disks.sh lists the disks and says
 * what the system is, do-install.sh erases a disk and installs on it
 * (with -c, copying the running system). Nothing here partitions or
 * copies anything; the helper checks the request against the probe, runs
 * the engine with fork and exec (never a shell command line), and follows
 * its PROGRESS and PHASE lines.
 *
 * The checks don't trust the app: the disk must be one probe-disks.sh
 * lists, not the one the running system started from (is_media), at
 * least NEED bytes, and the mode must be the system's (SYSTEM). Memory
 * disks are listed only in the probe's test mode, which only root can
 * turn on.
 *
 * An install takes minutes and the helper answers one message at a time,
 * so do-install.sh runs in the background: sh_install_poll() reads its
 * output and reaps it, from the main loop and from every InstallProgress.
 * The helper stays up while it runs. A finished install keeps it up until
 * the restart, however long the Installer's Restart waits to be clicked:
 * the result lives only here, so a helper that exited idle came back with
 * no job and refused the restart. A failed one is kept for RESULT_LIFE so
 * the app can still ask how it went.
 */
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	ENGINE_DIR	"/usr/libexec/nextbsd-installer"
#define	PROBE		ENGINE_DIR "/probe-disks.sh"
#define	INSTALL		ENGINE_DIR "/do-install.sh"
#define	SHUTDOWN	"/sbin/shutdown"

#define	PROBE_MAX	(64 * 1024)	/* a few lines a disk */
#define	RESULT_LIFE	600		/* seconds a failed install is kept */

/* The engine's environment: nothing of the helper's own. */
static char *const engine_env[] = {
	"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
	"HOME=/var/empty",
	NULL
};

static struct {
	int		state;		/* SH_INSTALL_* */
	int		percent;
	int		phase;		/* SH_INSTALL_PREPARE ... */
	pid_t		pid;		/* do-install.sh, while it runs */
	int		fd;		/* its output, or -1 */
	char		disk[SH_DISK_MAX + 1];
	bool		test_disk;	/* an md(4) disk: restart nothing */
	pid_t		leader;		/* the session that started it */
	long long	leader_start;
	time_t		finished;
	char		line[512];	/* a partial line of output */
	size_t		len;
	char		last[256];	/* the last line that wasn't a record */
} job = { .fd = -1 };

/* What probe-disks.sh says about the system and about one disk. */
struct probe {
	bool		installed;	/* SYSTEM installed (else image) */
	unsigned long long need;	/* NEED */
	bool		found;		/* DISK <name> was listed */
	bool		is_media;
	unsigned long long bytes;
};

/* "ada1", "da0", "nvd1", "md3": letters, then digits. */
static bool
valid_disk_name(const char *disk)
{
	const char *p = disk;

	if (!isalpha((unsigned char)*p))
		return (false);
	while (isalpha((unsigned char)*p))
		p++;
	if (!isdigit((unsigned char)*p))
		return (false);
	while (isdigit((unsigned char)*p))
		p++;
	return (*p == '\0' && p - disk <= SH_DISK_MAX);
}

/*
 * Fork and exec `argv` (argv[0] a full path) with the engine's
 * environment, stdin on /dev/null and stdout and stderr on a pipe whose
 * read end is *fd. The child gets a process group of its own, so that it
 * and what it starts can be told apart from the helper.
 */
static pid_t
spawn(const char *const argv[], int *fd)
{
	int p[2], null;
	pid_t pid;

	if (pipe(p) == -1)
		return (-1);
	pid = fork();
	if (pid == -1) {
		(void)close(p[0]);
		(void)close(p[1]);
		return (-1);
	}
	if (pid == 0) {
		(void)setpgid(0, 0);
		null = open("/dev/null", O_RDONLY);
		if (null != -1)
			(void)dup2(null, STDIN_FILENO);
		(void)dup2(p[1], STDOUT_FILENO);
		(void)dup2(p[1], STDERR_FILENO);
		closefrom(STDERR_FILENO + 1);
		(void)signal(SIGPIPE, SIG_DFL);
		execve(argv[0], (char *const *)argv, engine_env);
		_exit(127);
	}
	(void)close(p[1]);
	(void)fcntl(p[0], F_SETFD, FD_CLOEXEC);
	*fd = p[0];
	return (pid);
}

/* Split a line at its tabs, in place: up to max fields. */
static int
fields(char *line, char *f[], int max)
{
	int n = 0;

	while (n < max) {
		f[n++] = line;
		line = strchr(line, '\t');
		if (line == NULL)
			break;
		*line++ = '\0';
	}
	return (n);
}

/* Run probe-disks.sh and read what it says about the system and `disk`. */
static bool
probe(const char *disk, struct probe *pr)
{
	const char *const argv[] = { PROBE, NULL };
	char *out, *line, *next, *f[8];
	size_t len = 0;
	ssize_t n;
	pid_t pid;
	int fd, st, nf;
	bool system_seen = false;

	memset(pr, 0, sizeof(*pr));
	out = malloc(PROBE_MAX + 1);
	if (out == NULL)
		return (false);
	pid = spawn(argv, &fd);
	if (pid == -1) {
		free(out);
		return (false);
	}
	while (len < PROBE_MAX) {
		n = read(fd, out + len, PROBE_MAX - len);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	(void)close(fd);
	while (waitpid(pid, &st, 0) == -1 && errno == EINTR)
		;
	out[len] = '\0';

	for (line = out; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		nf = fields(line, f, 8);
		if (nf >= 2 && strcmp(f[0], "SYSTEM") == 0) {
			pr->installed = strcmp(f[1], "installed") == 0;
			system_seen = true;
		} else if (nf >= 2 && strcmp(f[0], "NEED") == 0)
			pr->need = strtoull(f[1], NULL, 10);
		else if (nf >= 6 && strcmp(f[0], "DISK") == 0 &&
		    strcmp(f[1], disk) == 0) {
			pr->found = true;
			pr->is_media = strcmp(f[4], "0") != 0;
			pr->bytes = strtoull(f[5], NULL, 10);
		}
	}
	free(out);
	return (system_seen && pr->need > 0);
}

int
sh_install_start(const struct sh_caller *caller, const char *disk, int mode)
{
	const char *argv[5];
	struct probe pr;
	int fd, n = 0;
	pid_t pid;

	sh_install_poll();
	if (job.state == SH_INSTALL_RUNNING)
		return (SH_ERR_BUSY);
	if (!valid_disk_name(disk) ||
	    (mode != SH_INSTALL_IMAGE && mode != SH_INSTALL_COPY))
		return (SH_ERR_INVALID);
	if (!probe(disk, &pr)) {
		sh_log("InstallStart: %s gave no answer", PROBE);
		return (SH_ERR_FAILED);
	}
	if (!pr.found) {
		sh_log("InstallStart: %s isn't a disk the installer lists", disk);
		return (SH_ERR_INVALID);
	}
	if (pr.is_media) {
		sh_log("InstallStart: %s is the disk this system started from",
		    disk);
		return (SH_ERR_INVALID);
	}
	if (pr.bytes < pr.need) {
		sh_log("InstallStart: %s is too small (%llu bytes, %llu needed)",
		    disk, pr.bytes, pr.need);
		return (SH_ERR_INVALID);
	}
	if (pr.installed != (mode == SH_INSTALL_COPY)) {
		sh_log("InstallStart: asked to %s, but this system is %s",
		    mode == SH_INSTALL_COPY ? "copy it" : "install the image",
		    pr.installed ? "installed" : "the install image");
		return (SH_ERR_INVALID);
	}

	argv[n++] = INSTALL;
	argv[n++] = "-d";
	argv[n++] = disk;
	if (mode == SH_INSTALL_COPY)
		argv[n++] = "-c";
	argv[n] = NULL;
	pid = spawn(argv, &fd);
	if (pid == -1) {
		sh_log("InstallStart: %s: %s", INSTALL, strerror(errno));
		return (SH_ERR_FAILED);
	}
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);

	memset(&job, 0, sizeof(job));
	job.state = SH_INSTALL_RUNNING;
	job.phase = SH_INSTALL_PREPARE;
	job.pid = pid;
	job.fd = fd;
	(void)strlcpy(job.disk, disk, sizeof(job.disk));
	job.test_disk = strncmp(disk, "md", 2) == 0;
	job.leader = caller->console.leader;
	job.leader_start = caller->console.leader_start;
	sh_log("pid %d: InstallStart %s%s: do-install.sh is pid %d",
	    (int)caller->pid, disk, mode == SH_INSTALL_COPY ? " (copy)" : "",
	    (int)pid);
	return (SH_OK);
}

/* One line of do-install.sh's output. */
static void
engine_line(char *line)
{
	static const char *const phases[] = { "prepare", "copy", "setup",
	    "finish" };
	char *f[3];
	int i, nf, p;

	nf = fields(line, f, 3);
	if (nf >= 2 && strcmp(f[0], "PROGRESS") == 0) {
		p = atoi(f[1]);
		if (p > job.percent && p <= 100)
			job.percent = p;
	} else if (nf >= 2 && strcmp(f[0], "PHASE") == 0) {
		for (i = 0; i < 4; i++)
			if (strcmp(f[1], phases[i]) == 0 && i > job.phase) {
				job.phase = i;
				sh_log("install on %s: %s", job.disk, phases[i]);
			}
	} else if (nf >= 1 && strcmp(f[0], "STATUS") != 0 &&
	    strcmp(f[0], "DONE") != 0 && line[0] != '\0') {
		/* The tools' own messages: the last one usually says why
		 * an install failed. */
		(void)strlcpy(job.last, line, sizeof(job.last));
	}
}

/* Read what the engine has written so far, line by line. */
static void
drain(void)
{
	char buf[1024];
	ssize_t n;
	size_t i;

	while (job.fd != -1) {
		n = read(job.fd, buf, sizeof(buf));
		if (n == -1 && errno == EINTR)
			continue;
		if (n == -1)	/* EAGAIN: nothing more for now */
			break;
		if (n == 0) {	/* the engine and its children are done */
			(void)close(job.fd);
			job.fd = -1;
			break;
		}
		for (i = 0; i < (size_t)n; i++) {
			if (buf[i] == '\n') {
				job.line[job.len] = '\0';
				engine_line(job.line);
				job.len = 0;
			} else if (job.len < sizeof(job.line) - 1)
				job.line[job.len++] = buf[i];
		}
	}
}

void
sh_install_poll(void)
{
	int st;

	if (job.state != SH_INSTALL_RUNNING)
		return;
	drain();
	/* Reap it as soon as it exits, and read what it wrote last: a
	 * child it left running (sleep in its progress loop) may hold the
	 * pipe open a little longer, so don't wait for the end of it. */
	if (waitpid(job.pid, &st, WNOHANG) != job.pid)
		return;
	drain();
	if (job.fd != -1) {
		(void)close(job.fd);
		job.fd = -1;
	}
	job.finished = sh_now();
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
		job.state = SH_INSTALL_DONE;
		job.percent = 100;
		job.phase = SH_INSTALL_FINISH;
		sh_log("install on %s: done", job.disk);
	} else {
		job.state = SH_INSTALL_FAILED;
		sh_log("install on %s: failed (%s %d): %s", job.disk,
		    WIFEXITED(st) ? "exit" : "signal",
		    WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st), job.last);
	}
}

bool
sh_install_busy(void)
{
	sh_install_poll();
	if (job.state == SH_INSTALL_RUNNING || job.state == SH_INSTALL_DONE)
		return (true);
	return (job.state != SH_INSTALL_IDLE &&
	    sh_now() - job.finished < RESULT_LIFE);
}

/* Is this the caller whose session started the install? */
static bool
owner(const struct sh_caller *caller)
{
	if (caller->root)
		return (true);
	return (caller->in_console && job.state != SH_INSTALL_IDLE &&
	    caller->console.leader == job.leader &&
	    caller->console.leader_start == job.leader_start);
}

int
sh_install_progress(const struct sh_caller *caller, int *state, int *percent,
    int *phase)
{
	*state = SH_INSTALL_IDLE;
	*percent = 0;
	*phase = SH_INSTALL_PREPARE;
	if (!caller->root && !caller->in_console)
		return (SH_ERR_NOT_CONSOLE);
	sh_install_poll();
	if (!owner(caller))
		return (SH_OK);		/* nothing of this session's */
	*state = job.state;
	*percent = job.percent;
	*phase = job.phase;
	return (SH_OK);
}

int
sh_install_restart(const struct sh_caller *caller)
{
	const char *const argv[] = { SHUTDOWN, "-r", "now", NULL };

	if (!caller->root && !caller->in_console)
		return (SH_ERR_NOT_CONSOLE);
	sh_install_poll();
	if (!owner(caller) || job.state != SH_INSTALL_DONE)
		return (SH_ERR_DENIED);
	if (job.test_disk) {
		sh_log("pid %d: InstallRestart after a test disk (%s): not "
		    "restarting", (int)caller->pid, job.disk);
		return (SH_OK);
	}
	sh_log("pid %d: InstallRestart: restarting", (int)caller->pid);
	return (sh_run(argv) ? SH_OK : SH_ERR_FAILED);
}
