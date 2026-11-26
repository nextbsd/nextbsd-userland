/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_update.c — Update.app's jobs: check, download and install NextBSD
 * from its pkg repository, and the restart that finishes an update
 * (nextbsd/nextbsd#661).
 *
 * NextBSD is updated as a whole: every build replaces all of the
 * repository's packages, so an update is plain pkg, with no package
 * names:
 *
 *	check		pkg update, then pkg upgrade -n: an update is there
 *			when it has something to do, and says how much to
 *			download
 *	download	pkg upgrade -F: into pkg's cache, nothing installed
 *	install		pkg upgrade, from the cache
 *
 * After the check, each keeps to the catalogue it read (-U). A package
 * name is used only to show the update's version: the repository's
 * meta-package's (metas[] below), and none if that fails. pkg runs with
 * fork and exec, never a shell, with a fixed environment and arguments
 * that are all the helper's own: nothing from the caller reaches it.
 *
 * A job takes minutes and the helper answers one message at a time, so
 * each job runs in a child process in a session of its own (so that
 * Cancel can stop it and its pkg together) and the helper goes on
 * answering. The child reports through a page shared with the helper
 * (struct job), which sh_update_status copies out; Update.app asks for
 * it twice a second while a job runs. The helper doesn't exit idle while
 * a job runs (SettingsHelper.c).
 *
 * Progress comes from pkg's event pipe (-o EVENT_PIPE), a FIFO in a
 * root-only directory, whose JSON lines say which file is being fetched
 * or which package installed, with byte and file counts. Only the size
 * and the number of packages, which pkg doesn't send as events, are read
 * from pkg upgrade -n's summary.
 *
 * An install over the running system needs a restart, and the install
 * itself may replace this program and SettingsRights.plist (an update
 * brings NextBSD's own SettingsHelper). So the administrator's approval
 * of the install covers the restart, and the restart reads nothing the
 * install can replace: once an install succeeds, this process remembers
 * it (restart_approved) and stays running until the restart, and
 * RESTART_FILE records the boot it happened in, the new version (if
 * known) and that the restart was approved. Until the next boot an
 * update waits for a restart, which sh_update_restart allows without a
 * rights check; after it Update says the update succeeded until the next
 * check.
 *
 * On a ZFS install the install doesn't touch the running system: it goes
 * into a new boot environment (nextbsd/nextbsd#703). bootenv-update begin
 * clones the running system and mounts the clone, pkg -r <clone> upgrade
 * installs into it, and bootenv-update commit names it after the version
 * now in it, copies its loader to the EFI partition, makes it start next
 * and removes versions beyond the number kept. Any failure runs
 * bootenv-update abort, which removes the clone: the running system is as
 * it was. On UFS the install goes over the running system, as before.
 *
 * Built with -DSH_UPDATE_TEST=\"/dir\" (make UPDATE_TEST=/dir), pkg
 * works on a throwaway database, cache and repository under /dir
 * instead of the system's, and restart only logs: a way to try the
 * whole flow without touching the system. Never install that build.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	PKG		"/usr/sbin/pkg"
#define	SHUTDOWN	"/sbin/shutdown"
#define	BOOTENV		"/usr/libexec/nextbsd/bootenv-update"
#define	BECTL		"/sbin/bectl"
#define	KEEP_DIR	"/Local/Library/Preferences"
#define	KEEP_FILE	KEEP_DIR "/org.nextbsd.bootenv-update.conf"

#ifdef SH_UPDATE_TEST
#define	STATE_DIR	SH_UPDATE_TEST "/state"
#else
#define	STATE_DIR	"/var/db/SettingsHelper"
#endif
#define	RESTART_FILE	STATE_DIR "/UpdateRestart"
#define	EVENT_DIR	"/var/run/SettingsHelper.XXXXXX"

#define	OUT_MAX		(64 * 1024)	/* pkg upgrade -n's output */
#define	PKG_UNIT	1000		/* progress a package */

/* What the job's child reports, in a page shared with the helper. */
struct job {
	int		job;		/* SH_UPDATE_JOB_* */
	int		state;		/* SH_UPDATE_* */
	bool		cancelled;
	bool		changed;
	bool		pending;	/* pkg upgrade has something to do */
	SHUpdateVersion	available;
	int64_t		size;
	int64_t		done;
	int64_t		total;
	SHUpdateText	line;
};

static struct job *job;
static pid_t job_pid = -1;
static bool in_job;		/* this is the job's child */
static const char *pkg_root;	/* pkg -r: the boot environment being
				   installed into */
static bool restart_approved;	/* this process installed an update */

/* Event-pipe bookkeeping, in the child. */
struct progress {
	bool		in_item;	/* in a file's or package's events */
	int64_t		base;		/* bytes of files fetched */
	int64_t		item_total;
	int		packages;	/* to install (pkg upgrade -n) */
	int		index;		/* packages begun */
};

static bool
job_page(void)
{
	void *p;

	if (job != NULL)
		return (true);
	p = mmap(NULL, sizeof(struct job), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) {
		sh_log("update: mmap: %s", strerror(errno));
		return (false);
	}
	job = p;
	memset(job, 0, sizeof(*job));
	return (true);
}

static void
set_line(const char *text)
{
	(void)strlcpy(job->line, text, sizeof(job->line));
}

/* ---- running pkg ------------------------------------------------------- */

/* pkg's arguments before the command: the test build's throwaway
 * database, cache and repositories, and the event pipe. */
static int
pkg_argv(const char *argv[], int max, const char *evpipe,
    const char *const args[])
{
	static char evopt[PATH_MAX + 16];
	int n = 0;

	argv[n++] = PKG;
#ifdef SH_UPDATE_TEST
	argv[n++] = "-o";
	argv[n++] = "PKG_DBDIR=" SH_UPDATE_TEST "/db";
	argv[n++] = "-o";
	argv[n++] = "PKG_CACHEDIR=" SH_UPDATE_TEST "/cache";
	argv[n++] = "-o";
	argv[n++] = "REPOS_DIR=" SH_UPDATE_TEST "/repos";
	argv[n++] = "-r";
	argv[n++] = SH_UPDATE_TEST "/root";
#endif
	if (pkg_root != NULL) {
		argv[n++] = "-r";
		argv[n++] = pkg_root;
	}
	if (evpipe != NULL) {
		(void)snprintf(evopt, sizeof(evopt), "EVENT_PIPE=%s", evpipe);
		argv[n++] = "-o";
		argv[n++] = evopt;
	}
	while (*args != NULL && n < max - 1)
		argv[n++] = *args++;
	argv[n] = NULL;
	return (n);
}

/* Start pkg: stdin /dev/null, stdout to `out_fd` (or /dev/null), stderr
 * the helper's log. */
static pid_t
pkg_spawn(const char *const argv[], int out_fd)
{
	static const char *const env[] = {
		"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
		"LC_ALL=C",
		NULL
	};
	pid_t pid;

	if (in_job && job->cancelled)	/* no next step after a cancel */
		return (-1);
	pid = fork();
	if (pid == -1) {
		sh_log("update: fork: %s", strerror(errno));
		return (-1);
	}
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd != -1) {
			(void)dup2(fd, STDIN_FILENO);
			(void)dup2(out_fd != -1 ? out_fd : fd, STDOUT_FILENO);
		}
		/* The job ignores SIGTERM (a cancel); pkg mustn't. */
		(void)signal(SIGTERM, SIG_DFL);
		execve(argv[0], (char *const *)argv, (char *const *)env);
		_exit(127);
	}
	return (pid);
}

static int
pkg_wait(pid_t pid)
{
	int st;

	while (waitpid(pid, &st, 0) == -1)
		if (errno != EINTR)
			return (-1);
	return (WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

/*
 * Run pkg with `args` and keep its standard output (at most OUT_MAX
 * bytes) in `out`, which the caller frees. Returns pkg's exit status, -1
 * if it didn't run or was killed.
 */
static int
pkg_output(const char *const args[], char **out)
{
	const char *argv[32];
	char *buf;
	size_t len = 0;
	ssize_t n;
	pid_t pid;
	int fds[2], rc;

	*out = NULL;
	buf = malloc(OUT_MAX + 1);
	if (buf == NULL || pipe2(fds, O_CLOEXEC) == -1) {
		free(buf);
		return (-1);
	}
	(void)pkg_argv(argv, 32, NULL, args);
	pid = pkg_spawn(argv, fds[1]);
	(void)close(fds[1]);
	for (;;) {
		char rest[512];

		/* Past OUT_MAX, read on but drop it, so pkg never blocks. */
		if (len < OUT_MAX)
			n = read(fds[0], buf + len, OUT_MAX - len);
		else
			n = read(fds[0], rest, sizeof(rest));
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (len < OUT_MAX)
			len += (size_t)n;
	}
	(void)close(fds[0]);
	buf[len] = '\0';
	rc = pid == -1 ? -1 : pkg_wait(pid);
	*out = buf;
	return (rc);
}

/* pkg's first line of output, trimmed, into `dst`; false if pkg failed
 * or said nothing. */
static bool
pkg_line(const char *const args[], char *dst, size_t size)
{
	char *out;
	int rc;

	rc = pkg_output(args, &out);
	if (out == NULL)
		return (false);
	out[strcspn(out, "\r\n")] = '\0';
	(void)strlcpy(dst, out, size);
	free(out);
	return (rc == 0 && dst[0] != '\0');
}

/*
 * Run bootenv-update `verb`: its first line of output into `out` (when
 * given), its messages to the helper's log. Returns its exit status, -1
 * if it didn't run.
 */
static int
bootenv(const char *verb, char *out, size_t size)
{
	static const char *const env[] = {
		"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
		"LC_ALL=C",
		NULL
	};
	const char *argv[] = { BOOTENV, verb, NULL };
	char buf[PATH_MAX];
	size_t len = 0;
	ssize_t n;
	pid_t pid;
	int fds[2], rc;

	if (out != NULL && size > 0)
		out[0] = '\0';
	if (pipe2(fds, O_CLOEXEC) == -1)
		return (-1);
	pid = fork();
	if (pid == -1) {
		(void)close(fds[0]);
		(void)close(fds[1]);
		return (-1);
	}
	if (pid == 0) {
		int fd = open("/dev/null", O_RDONLY);

		if (fd != -1)
			(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fds[1], STDOUT_FILENO);
		/* A cancel mustn't stop it half way through a step. */
		(void)signal(SIGTERM, SIG_IGN);
		execve(argv[0], (char *const *)argv, (char *const *)env);
		_exit(127);
	}
	(void)close(fds[1]);
	while ((n = read(fds[0], buf + len, sizeof(buf) - 1 - len)) != 0) {
		if (n == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		len += (size_t)n;
		if (len == sizeof(buf) - 1)
			break;
	}
	(void)close(fds[0]);
	buf[len] = '\0';
	buf[strcspn(buf, "\r\n")] = '\0';
	if (out != NULL)
		(void)strlcpy(out, buf, size);
	rc = pkg_wait(pid);
	sh_log("update: bootenv-update %s: %d%s%s", verb, rc,
	    buf[0] != '\0' ? " " : "", buf);
	return (rc);
}

/* ---- the event pipe ---------------------------------------------------- */

/* The value of "key" in one event line: a string into `str`, or a
 * number into `num`. */
static bool
event_value(const char *line, const char *key, char *str, size_t size,
    int64_t *num)
{
	char pat[64];
	const char *p;
	size_t n;

	(void)snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(line, pat);
	if (p == NULL)
		return (false);
	p += strlen(pat);
	p += strspn(p, " \t");
	if (*p++ != ':')
		return (false);
	p += strspn(p, " \t");
	if (str != NULL) {
		if (*p++ != '"')
			return (false);
		n = strcspn(p, "\"");
		if (n >= size)
			n = size - 1;
		memcpy(str, p, n);
		str[n] = '\0';
		return (true);
	}
	*num = strtoll(p, NULL, 10);
	return (true);
}

static bool
is_event(const char *line, const char *type)
{
	char t[64];

	return (event_value(line, "type", t, sizeof(t), NULL) &&
	    strcmp(t, type) == 0);
}

static void
on_event(const char *line, struct progress *pr)
{
	char name[96], text[SH_UPDATE_TEXT_MAX + 1];
	int64_t cur = 0, tot = 0;

	if (job->job == SH_UPDATE_JOB_DOWNLOAD) {
		if (is_event(line, "INFO_FETCH_BEGIN")) {
			pr->in_item = true;
			pr->item_total = 0;
		} else if (is_event(line, "INFO_FETCH_FINISHED")) {
			pr->base += pr->item_total;
			pr->in_item = false;
			job->done = pr->base;
		} else if (pr->in_item &&
		    is_event(line, "INFO_PROGRESS_TICK") &&
		    event_value(line, "current", NULL, 0, &cur) &&
		    event_value(line, "total", NULL, 0, &tot)) {
			pr->item_total = tot;
			job->done = pr->base + cur;
		}
		if (job->done > job->total)
			job->total = job->done;
		return;
	}

	/* The install: each package begun counts one, ticks within it a
	 * part; fetching or verifying first only change the line. */
	if (is_event(line, "INFO_UPGRADE_BEGIN") ||
	    is_event(line, "INFO_INSTALL_BEGIN")) {
		job->changed = true;
		pr->in_item = true;
		if (pr->index < pr->packages)
			pr->index++;
		if (!event_value(line, "pkgname", name, sizeof(name), NULL))
			name[0] = '\0';
		(void)snprintf(text, sizeof(text), "Installing %s (%d of %d)",
		    name, pr->index, pr->packages);
		set_line(text);
		job->done = (int64_t)(pr->index - 1) * PKG_UNIT;
	} else if (is_event(line, "INFO_UPGRADE_FINISHED") ||
	    is_event(line, "INFO_INSTALL_FINISHED")) {
		pr->in_item = false;
		job->done = (int64_t)pr->index * PKG_UNIT;
	} else if (pr->in_item && is_event(line, "INFO_PROGRESS_TICK") &&
	    event_value(line, "current", NULL, 0, &cur) &&
	    event_value(line, "total", NULL, 0, &tot) && tot > 0) {
		job->done = (int64_t)(pr->index - 1) * PKG_UNIT +
		    cur * PKG_UNIT / tot;
	} else if (pr->index == 0 && is_event(line, "INFO_FETCH_BEGIN")) {
		set_line("Downloading the update");
	} else if (pr->index == 0 &&
	    is_event(line, "INFO_INTEGRITYCHECK_BEGIN")) {
		set_line("Verifying the update");
	}
}

/* Read whole lines from the FIFO; a partial one waits in `buf`. */
static void
read_events(int fd, char *buf, size_t size, size_t *len,
    struct progress *pr)
{
	ssize_t n;
	char *nl;

	for (;;) {
		n = read(fd, buf + *len, size - 1 - *len);
		if (n <= 0)
			return;
		*len += (size_t)n;
		buf[*len] = '\0';
		while ((nl = strchr(buf, '\n')) != NULL) {
			*nl = '\0';
			on_event(buf, pr);
			*len -= (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, *len + 1);
		}
		if (*len == size - 1)
			*len = 0;	/* a line too long to be an event */
	}
}

/*
 * Run pkg with `args` and its event pipe, following its progress. The
 * FIFO is opened for reading before pkg starts: pkg opens it
 * non-blocking, which fails without a reader. Returns pkg's exit status.
 */
static int
pkg_events(const char *const args[], struct progress *pr)
{
	char dir[] = EVENT_DIR, path[PATH_MAX], buf[8192];
	const char *argv[40];
	struct pollfd pfd;
	size_t len = 0;
	pid_t pid;
	int fd, st, rc = -1;

	if (mkdtemp(dir) == NULL) {
		sh_log("update: mkdtemp: %s", strerror(errno));
		return (-1);
	}
	(void)snprintf(path, sizeof(path), "%s/events", dir);
	if (mkfifo(path, 0600) == -1 ||
	    (fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) == -1) {
		sh_log("update: %s: %s", path, strerror(errno));
		(void)unlink(path);
		(void)rmdir(dir);
		return (-1);
	}
	(void)pkg_argv(argv, 40, path, args);
	pid = pkg_spawn(argv, -1);
	pfd.fd = fd;
	pfd.events = POLLIN;
	while (pid != -1) {
		(void)poll(&pfd, 1, 250);
		read_events(fd, buf, sizeof(buf), &len, pr);
		if (waitpid(pid, &st, WNOHANG) == pid) {
			read_events(fd, buf, sizeof(buf), &len, pr);
			rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			break;
		}
	}
	(void)close(fd);
	(void)unlink(path);
	(void)rmdir(dir);
	return (rc);
}

/* ---- pkg upgrade -n's summary ------------------------------------------ */

/* "535 MiB to be downloaded." in bytes; 0 when nothing is to be. */
static int64_t
summary_size(const char *out)
{
	static const char *const units[] = { "B", "KiB", "MiB", "GiB",
	    "TiB" };
	const char *p, *line;
	char unit[8];
	double v;
	size_t i;

	p = strstr(out, " to be downloaded.");
	if (p == NULL)
		return (0);
	for (line = p; line > out && line[-1] != '\n'; line--)
		;
	if (sscanf(line, "%lf %7s", &v, unit) != 2)
		return (0);
	for (i = 0; i < sizeof(units) / sizeof(units[0]); i++, v *= 1024)
		if (strcmp(unit, units[i]) == 0)
			return ((int64_t)v);
	return (0);
}

/* The packages to be upgraded, installed, reinstalled or downgraded. */
static int
summary_packages(const char *out)
{
	static const char *const kinds[] = { "upgraded", "installed",
	    "reinstalled", "downgraded" };
	char pat[64];
	const char *p;
	size_t i;
	int sum = 0;

	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		(void)snprintf(pat, sizeof(pat),
		    "Number of packages to be %s: ", kinds[i]);
		p = strstr(out, pat);
		if (p != NULL)
			sum += atoi(p + strlen(pat));
	}
	return (sum);
}

/* pkg upgrade -n: the download's bytes and the install's packages. */
static bool
dry_run(int64_t *size, int *packages)
{
	static const char *const args[] = { "upgrade", "-n", "-U", NULL };
	char *out;
	int rc;

	rc = pkg_output(args, &out);
	if (out == NULL)
		return (false);
	*size = summary_size(out);
	*packages = summary_packages(out);
	free(out);
	return (rc == 0 || rc == 1);	/* 1: something to do, in some
					   pkg versions */
}

/* ---- the version shown ------------------------------------------------ */

/*
 * NextBSD's meta-packages, the first the repository has naming the
 * update's version for Update to show: nothing else. Whether there is
 * an update, and what it changes, is pkg upgrade's alone. NextBSD-
 * everything is giving way to NextBSD-server and NextBSD-desktop, which
 * requires it (nextbsd/nextbsd#680); keep this list in step.
 */
static const char *const metas[] = {
	"NextBSD-desktop",
	"NextBSD-server",
	"NextBSD-everything",
	NULL
};

/* The update's version, or "" if the repository has none of them. */
static void
available_version(char *v, size_t size)
{
	const char *args[] = { "rquery", "-U", "%v", NULL, NULL };
	int i;

	for (i = 0; metas[i] != NULL; i++) {
		args[3] = metas[i];
		if (pkg_line(args, v, size))
			return;
	}
	v[0] = '\0';
}

/* ---- the restart record ------------------------------------------------ */

static time_t
boot_time(void)
{
	struct timeval tv;
	size_t len = sizeof(tv);

	if (sysctlbyname("kern.boottime", &tv, &len, NULL, 0) == -1)
		return (0);
	return (tv.tv_sec);
}

static void
restart_record(const char *version)
{
	char text[128];

	(void)mkdir(STATE_DIR, 0755);
	(void)snprintf(text, sizeof(text),
	    "boot %lld\nversion %s\napproved restart\n",
	    (long long)boot_time(), version);
	(void)sh_write_file(RESTART_FILE, text, strlen(text));
}

/* SH_UPDATE_RESTART_*, and the version installed into `version` (if
 * not NULL; "" if not known). */
static int
restart_state(char *version, size_t size)
{
	char *text, *v;
	size_t len;
	long long boot;
	int state = SH_UPDATE_RESTART_NONE;

	if (version != NULL)
		version[0] = '\0';
	text = sh_read_file(RESTART_FILE, &len);
	if (text == NULL)
		return (state);
	if (sscanf(text, "boot %lld", &boot) == 1)
		state = boot == (long long)boot_time() ?
		    SH_UPDATE_RESTART_PENDING : SH_UPDATE_RESTART_DONE;
	if (version != NULL && (v = strstr(text, "\nversion ")) != NULL) {
		v += strlen("\nversion ");
		v[strcspn(v, "\n")] = '\0';
		(void)strlcpy(version, v, size);
	}
	free(text);
	return (state);
}

/* ---- the jobs, in the child -------------------------------------------- */

/*
 * The check: pkg update, then pkg upgrade -n. An update is there when
 * pkg upgrade has something to do; `available` only names it.
 */
static int
run_check(void)
{
	static const char *const update[] = { "update", NULL };
	char *out;
	int packages, rc;

	set_line("Checking for updates");
	rc = pkg_output(update, &out);
	free(out);
	if (rc != 0 || !dry_run(&job->size, &packages))
		return (SH_UPDATE_FAILED);
	job->pending = packages > 0;
	if (job->pending)
		available_version(job->available, sizeof(job->available));
	/* A check after the restart ends "Updated successfully". */
	if (restart_state(NULL, 0) == SH_UPDATE_RESTART_DONE)
		(void)unlink(RESTART_FILE);
	return (SH_UPDATE_DONE);
}

static int
run_download(void)
{
	static const char *const args[] = { "upgrade", "-F", "-y", "-U",
	    NULL };
	struct progress pr;
	int packages;

	memset(&pr, 0, sizeof(pr));
	set_line("Downloading the update");
	if (!dry_run(&job->size, &packages) || packages == 0)
		return (SH_UPDATE_FAILED);
	job->pending = true;
	available_version(job->available, sizeof(job->available));
	job->total = job->size;
	if (job->size > 0 && pkg_events(args, &pr) != 0)
		return (SH_UPDATE_FAILED);
	job->done = job->total;
	return (SH_UPDATE_DONE);
}

static int
run_install(void)
{
	static const char *const args[] = { "upgrade", "-y", "-U", NULL };
	static const char *const clean[] = { "clean", "-a", "-y", NULL };
	static char root[PATH_MAX];
	char name[128], *out;
	struct progress pr;
	int64_t size;
	int pass;
	bool be = false;

	set_line("Preparing to install");
	available_version(job->available, sizeof(job->available));
#ifndef SH_UPDATE_TEST
	/* ZFS: into a new boot environment (see the top of the file). */
	if (bootenv("check", NULL, 0) == 0) {
		set_line("Copying the system into a new boot environment");
		if (bootenv("begin", root, sizeof(root)) != 0 ||
		    root[0] != '/')
			return (SH_UPDATE_FAILED);
		pkg_root = root;
		be = true;
	}
#endif
	/* Until pkg upgrade has nothing left to do; a second pass if pkg
	 * upgraded only itself the first time. */
	for (pass = 0; pass < 3; pass++) {
		memset(&pr, 0, sizeof(pr));
		if (!dry_run(&size, &pr.packages))
			goto fail;
		if (pr.packages == 0)
			break;
		if (pass == 2)
			goto fail;
		job->pending = true;
		job->total = (int64_t)pr.packages * PKG_UNIT;
		job->done = 0;
		if (pkg_events(args, &pr) != 0)
			goto fail;
		job->done = job->total;
	}
	if (!job->pending)
		goto fail;	/* there was nothing to install */
	set_line("Finishing up");
	if (be) {
		/* The packages the clone was handed are installed now. */
		(void)pkg_output(clean, &out);
		free(out);
		pkg_root = NULL;
		if (bootenv("commit", name, sizeof(name)) != 0)
			goto fail;
		/* And the download, which the running system no longer
		   needs either. */
		(void)pkg_output(clean, &out);
		free(out);
	}
	job->pending = false;
	restart_record(job->available);
	return (SH_UPDATE_DONE);
fail:
	if (be) {
		pkg_root = NULL;
		(void)bootenv("abort", NULL, 0);
	}
	return (SH_UPDATE_FAILED);
}

/* ---- the helper's side ------------------------------------------------- */

/* Collect a finished job's child. */
void
sh_update_reap(void)
{
	int st;

	if (job_pid == -1 || waitpid(job_pid, &st, WNOHANG) != job_pid)
		return;
	job_pid = -1;
	if (job->state == SH_UPDATE_RUNNING)
		job->state = job->cancelled ? SH_UPDATE_CANCELLED :
		    SH_UPDATE_FAILED;
	sh_log("update: job %d ended: state %d", job->job, job->state);
	/* The install's approval covers the restart that finishes it. */
	if (job->job == SH_UPDATE_JOB_INSTALL &&
	    job->state == SH_UPDATE_DONE) {
		restart_approved = true;
		sh_log("update: installed; staying up for the restart");
	}
}

bool
sh_update_busy(void)
{
	sh_update_reap();
	return (job_pid != -1);
}

bool
sh_update_live(void)
{
	return (sh_update_busy() || restart_approved);
}

/* RESTART_FILE says an approved install waits for this boot's restart. */
static bool
restart_record_approved(void)
{
	char *text;
	size_t len;
	bool approved;

	if (restart_state(NULL, 0) != SH_UPDATE_RESTART_PENDING)
		return (false);
	text = sh_read_file(RESTART_FILE, &len);
	if (text == NULL)
		return (false);
	approved = strstr(text, "\napproved restart\n") != NULL;
	free(text);
	return (approved);
}

int
sh_update_start(int which)
{
	pid_t pid;
	int state;

	if (!job_page())
		return (SH_ERR_FAILED);
	if (sh_update_busy())
		return (SH_ERR_BUSY);
	memset(job, 0, sizeof(*job));
	job->job = which;
	job->state = SH_UPDATE_RUNNING;

	pid = fork();
	if (pid == -1) {
		sh_log("update: fork: %s", strerror(errno));
		job->state = SH_UPDATE_FAILED;
		return (SH_ERR_FAILED);
	}
	if (pid == 0) {
		/* Its own session, so a cancel reaches pkg too: pkg stops,
		 * and the child, which ignores it, cleans up after it. */
		(void)setsid();
		(void)signal(SIGTERM, SIG_IGN);
		in_job = true;
		switch (which) {
		case SH_UPDATE_JOB_CHECK:
			state = run_check();
			break;
		case SH_UPDATE_JOB_DOWNLOAD:
			state = run_download();
			break;
		default:
			state = run_install();
			break;
		}
		job->state = job->cancelled ? SH_UPDATE_CANCELLED : state;
		_exit(0);
	}
	job_pid = pid;
	return (SH_OK);
}

int
sh_update_cancel(void)
{
	if (!sh_update_busy())
		return (SH_OK);
	if (job->job == SH_UPDATE_JOB_INSTALL)
		return (SH_ERR_INVALID);
	job->cancelled = true;
	(void)kill(-job_pid, SIGTERM);
	return (SH_OK);
}

void
sh_update_status(struct sh_update_status *out)
{
	memset(out, 0, sizeof(*out));
	if (job_page()) {
		sh_update_reap();
		out->job = job->job;
		out->state = job->state;
		out->pending = job->pending;
		(void)strlcpy(out->available, job->available,
		    sizeof(out->available));
		out->size = job->size;
		out->done = job->done;
		out->total = job->total;
		(void)strlcpy(out->line, job->line, sizeof(out->line));
		out->changed = job->changed;
	}
	out->restart = restart_state(out->restart_version,
	    sizeof(out->restart_version));
}

/* ---- Installed Versions ------------------------------------------------ */

/*
 * Is `name` a boot environment bectl lists? Its Active column ("N", "R",
 * "NR" or "-") into `active`. Only names a boot environment can have.
 */
static bool
be_listed(const char *name, char *active, size_t size)
{
	static const char *const env[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
	    "LC_ALL=C", NULL };
	const char *argv[] = { BECTL, "list", "-H", NULL };
	char buf[16384], *line, *next, *tab;
	size_t len = 0, n = strlen(name);
	ssize_t r;
	pid_t pid;
	int fds[2];
	bool found = false;

	if (n == 0 || n >= sizeof(SHUpdateVersion) || name[0] == '-' ||
	    strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	    "0123456789._-") != n)
		return (false);
	if (pipe2(fds, O_CLOEXEC) == -1)
		return (false);
	if ((pid = fork()) == -1) {
		(void)close(fds[0]);
		(void)close(fds[1]);
		return (false);
	}
	if (pid == 0) {
		(void)dup2(fds[1], STDOUT_FILENO);
		execve(argv[0], (char *const *)argv, (char *const *)env);
		_exit(127);
	}
	(void)close(fds[1]);
	while (len < sizeof(buf) - 1 &&
	    (r = read(fds[0], buf + len, sizeof(buf) - 1 - len)) != 0) {
		if (r == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		len += (size_t)r;
	}
	(void)close(fds[0]);
	buf[len] = '\0';
	(void)pkg_wait(pid);
	for (line = buf; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		tab = strchr(line, '\t');
		if (tab == NULL || (size_t)(tab - line) != n ||
		    strncmp(line, name, n) != 0)
			continue;
		found = true;
		line = tab + 1;
		tab = strchr(line, '\t');
		if (tab != NULL)
			*tab = '\0';
		(void)strlcpy(active, line, size);
		break;
	}
	return (found);
}

static int
versions_ready(void)
{
	if (bootenv("check", NULL, 0) != 0)
		return (SH_ERR_UNSUPPORTED);
	if (sh_update_busy())
		return (SH_ERR_BUSY);
	return (SH_OK);
}

int
sh_update_start_version(const char *version)
{
	const char *activate[] = { BECTL, "activate", version, NULL };
#ifndef SH_UPDATE_TEST
	static const char *const restart[] = { SHUTDOWN, "-r", "now", NULL };
#endif
	char active[8];
	int rc;

	if ((rc = versions_ready()) != SH_OK)
		return (rc);
	if (!be_listed(version, active, sizeof(active)) ||
	    strchr(active, 'N') != NULL)
		return (SH_ERR_INVALID);
	if (!sh_run(activate))
		return (SH_ERR_FAILED);
#ifdef SH_UPDATE_TEST
	sh_log("update: test build: would run %s -r now", SHUTDOWN);
	return (SH_OK);
#else
	return (sh_run(restart) ? SH_OK : SH_ERR_FAILED);
#endif
}

int
sh_update_remove_version(const char *version)
{
	const char *destroy[] = { BECTL, "destroy", "-o", version, NULL };
	char active[8];
	int rc;

	if ((rc = versions_ready()) != SH_OK)
		return (rc);
	if (!be_listed(version, active, sizeof(active)) ||
	    strpbrk(active, "NR") != NULL)
		return (SH_ERR_INVALID);
	return (sh_run(destroy) ? SH_OK : SH_ERR_FAILED);
}

int
sh_update_set_keep(int keep)
{
	char tmp[] = KEEP_FILE ".XXXXXX", text[32];
	int fd, rc, len;

	if (keep < 2 || keep > 10)
		return (SH_ERR_INVALID);
	if ((rc = versions_ready()) != SH_OK && rc != SH_ERR_BUSY)
		return (rc);
	(void)mkdir(KEEP_DIR, 0755);
	if ((fd = mkstemp(tmp)) == -1)
		return (SH_ERR_FAILED);
	len = snprintf(text, sizeof(text), "keep=%d\n", keep);
	if (fchmod(fd, 0644) == -1 || write(fd, text, (size_t)len) != len ||
	    fsync(fd) == -1 || close(fd) == -1 || rename(tmp, KEEP_FILE) == -1) {
		(void)unlink(tmp);
		return (SH_ERR_FAILED);
	}
	return (SH_OK);
}

int
sh_update_restart(void)
{
#ifndef SH_UPDATE_TEST
	static const char *const argv[] = { SHUTDOWN, "-r", "now", NULL };
#endif

	if (sh_update_busy() ||
	    (!restart_approved && !restart_record_approved()))
		return (SH_ERR_INVALID);
#ifdef SH_UPDATE_TEST
	sh_log("update: test build: would run %s -r now", SHUTDOWN);
	return (SH_OK);
#else
	return (sh_run(argv) ? SH_OK : SH_ERR_FAILED);
#endif
}
