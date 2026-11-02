/*
 * child.c — the child-process supervisor. See child.h.
 *
 * Everything here runs on the caller's queue: the exit source's handler, the
 * kill timer and child_stop() itself, so the struct needs no lock. It is
 * reference-counted because a kill timer may still be pending when the child
 * has already exited and its owner has forgotten it.
 */
#include "child.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

struct child {
	pid_t			pid;
	char			name[32];	/* basename, for the logs */
	dispatch_queue_t	q;
	dispatch_source_t	src;
	child_exit_fn		fn;
	void			*ctx;
	int			refs;		/* the source, kill timers */
	bool			exited;
	bool			stopping;
};

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[child] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void
child_put(struct child *c)
{
	if (--c->refs > 0)
		return;
	dispatch_release(c->q);
	free(c);
}

/* Reap the child if it has exited; report it once. On c->q. */
static void
reap(struct child *c)
{
	pid_t r;
	int status = 0;

	if (c->exited)
		return;
	do {
		r = waitpid(c->pid, &status, WNOHANG);
	} while (r < 0 && errno == EINTR);
	if (r == 0)
		return;		/* still running */
	if (r < 0)
		status = 0;	/* already reaped elsewhere: report it gone */
	c->exited = true;
	if (WIFSIGNALED(status))
		xlog("%s (pid %d) ended by signal %d", c->name, (int)c->pid,
		    WTERMSIG(status));
	else if (WIFEXITED(status))
		xlog("%s (pid %d) exited %d", c->name, (int)c->pid,
		    WEXITSTATUS(status));
	if (c->src != NULL) {
		dispatch_source_cancel(c->src);
		dispatch_release(c->src);
		c->src = NULL;
	}
	c->fn(c->ctx, c, status);
	child_put(c);		/* the source's reference */
}

static void
exit_event(void *arg)
{
	reap(arg);
}

/* The first reap check, after the source is armed: a child that died
 * before kevent saw it is still found. */
static void
first_check(void *arg)
{
	struct child *c = arg;

	reap(c);
	child_put(c);
}

struct child *
child_spawn(const char *path, char *const argv[], dispatch_queue_t q,
    child_exit_fn fn, void *ctx)
{
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	sigset_t none, all;
	struct child *c;
	const char *base;
	int rc;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return (NULL);
	base = strrchr(path, '/');
	(void)strlcpy(c->name, base != NULL ? base + 1 : path,
	    sizeof(c->name));

	if (posix_spawn_file_actions_init(&fa) != 0) {
		free(c);
		errno = ENOMEM;
		return (NULL);
	}
	if (posix_spawnattr_init(&attr) != 0) {
		(void)posix_spawn_file_actions_destroy(&fa);
		free(c);
		errno = ENOMEM;
		return (NULL);
	}
	(void)posix_spawn_file_actions_addopen(&fa, STDIN_FILENO,
	    "/dev/null", O_RDONLY, 0);
	/* The HCI, L2CAP and SDP sockets are bluetoothd's alone. */
	(void)posix_spawn_file_actions_addclosefrom_np(&fa, STDERR_FILENO + 1);
	(void)sigemptyset(&none);
	(void)sigfillset(&all);
	(void)sigdelset(&all, SIGKILL);
	(void)sigdelset(&all, SIGSTOP);
	(void)posix_spawnattr_setsigmask(&attr, &none);
	(void)posix_spawnattr_setsigdefault(&attr, &all);
	(void)posix_spawnattr_setflags(&attr,
	    POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

	rc = posix_spawn(&c->pid, path, &fa, &attr, argv, environ);
	(void)posix_spawn_file_actions_destroy(&fa);
	(void)posix_spawnattr_destroy(&attr);
	if (rc != 0) {
		xlog("cannot start %s: %s", path, strerror(rc));
		free(c);
		errno = rc;
		return (NULL);
	}

	dispatch_retain(q);
	c->q = q;
	c->fn = fn;
	c->ctx = ctx;
	c->refs = 1;		/* the source, released by reap() */
	c->src = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
	    (uintptr_t)c->pid, DISPATCH_PROC_EXIT, q);
	if (c->src != NULL) {
		dispatch_set_context(c->src, c);
		dispatch_source_set_event_handler_f(c->src, exit_event);
		dispatch_resume(c->src);
	} else
		xlog("%s (pid %d): no exit source; relying on stop",
		    c->name, (int)c->pid);
	c->refs++;
	dispatch_async_f(q, c, first_check);
	xlog("started %s (pid %d)", c->name, (int)c->pid);
	return (c);
}

/* The grace period is over: kill whatever is left. On c->q. */
static void
kill_timer(void *arg)
{
	struct child *c = arg;

	if (!c->exited) {
		xlog("%s (pid %d) did not stop; killing it", c->name,
		    (int)c->pid);
		(void)kill(c->pid, SIGKILL);
		/* Without an exit source, this is where it gets reaped. */
		if (c->src == NULL) {
			struct timespec ts = { 0, 100 * 1000 * 1000 };

			(void)nanosleep(&ts, NULL);
			reap(c);
		}
	}
	child_put(c);
}

void
child_stop(struct child *c, int grace_ms)
{
	if (c == NULL || c->exited || c->stopping)
		return;
	c->stopping = true;
	(void)kill(c->pid, SIGTERM);
	c->refs++;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)grace_ms * 1000000), c->q, c, kill_timer);
}

pid_t
child_pid(const struct child *c)
{
	return (c != NULL ? c->pid : -1);
}
