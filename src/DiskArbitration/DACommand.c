/*
 * DACommand.c — run a helper program (E15 C5, nextbsd-userland#195).
 *
 * Port of Apple's DACommand.c executor. Apple forks, drops to the disk
 * owner's uid and execs; NextBSD's daemon runs every helper as root (an
 * unprivileged mount would need vfs.usermount and device-node access, see
 * DAMount.c), so posix_spawn with a pipe for the output is all it needs.
 * The status is read as (signed char)WEXITSTATUS, like Apple's, so the
 * FSUR_* codes of the bundle helpers (loadable_fs.h) come through negative.
 */
#include "DACommand.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
trim(char *s)
{
	size_t n = strlen(s);
	char *p = s;

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
	    s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
	while (*p == ' ' || *p == '\t')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
}

int
DACommandRun(char *const argv[], const char *env_extra, int options,
    char *out, size_t len)
{
	posix_spawn_file_actions_t fa;
	char *env[3], *nl;
	size_t got = 0;
	ssize_t n;
	pid_t pid;
	int pfd[2], status, i = 0;

	env[i++] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
	if (env_extra != NULL)
		env[i++] = (char *)(uintptr_t)env_extra;
	env[i] = NULL;

	out[0] = '\0';
	if (pipe(pfd) != 0)
		return (DA_COMMAND_NOT_RUN);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
	if (options & kDACommandExecuteOptionCaptureStderr)
		posix_spawn_file_actions_adddup2(&fa, pfd[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, pfd[0]);
	posix_spawn_file_actions_addclose(&fa, pfd[1]);
	if (posix_spawn(&pid, argv[0], &fa, NULL, argv, env) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		(void)close(pfd[0]);
		(void)close(pfd[1]);
		return (DA_COMMAND_NOT_RUN);
	}
	posix_spawn_file_actions_destroy(&fa);
	(void)close(pfd[1]);
	while (got < len - 1 &&
	    (n = read(pfd[0], out + got, len - 1 - got)) > 0)
		got += (size_t)n;
	out[got] = '\0';
	/* Drain the rest so the helper never blocks on a full pipe. */
	{
		char sink[256];

		while (read(pfd[0], sink, sizeof(sink)) > 0)
			;
	}
	(void)close(pfd[0]);
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return (DA_COMMAND_NOT_RUN);
	if ((nl = strchr(out, '\n')) != NULL)
		*nl = '\0';
	trim(out);
	if (!WIFEXITED(status))
		return (DA_COMMAND_NOT_RUN);
	return ((signed char)WEXITSTATUS(status));
}

struct da_command_job {
	char			**argv;
	int			  options;
	dispatch_queue_t	  queue;
	DACommandCallback	  callback;
	void			 *context;
	int			  status;
	char			  output[512];
};

static void
job_free(struct da_command_job *job)
{
	size_t i;

	if (job->argv != NULL)
		for (i = 0; job->argv[i] != NULL; i++)
			free(job->argv[i]);
	free(job->argv);
	free(job);
}

/* On the daemon queue. */
static void
job_done(void *ctx)
{
	struct da_command_job *job = ctx;

	if (job->callback != NULL)
		job->callback(job->status, job->output, job->context);
	job_free(job);
}

/* On a background queue. */
static void
job_run(void *ctx)
{
	struct da_command_job *job = ctx;

	job->status = DACommandRun(job->argv, NULL, job->options, job->output,
	    sizeof(job->output));
	dispatch_async_f(job->queue, job, job_done);
}

void
DACommandExecute(char *const argv[], int options, dispatch_queue_t queue,
    DACommandCallback callback, void *context)
{
	struct da_command_job *job;
	size_t i, n;

	for (n = 0; argv[n] != NULL; n++)
		;
	job = calloc(1, sizeof(*job));
	if (job != NULL)
		job->argv = calloc(n + 1, sizeof(*job->argv));
	if (job == NULL || job->argv == NULL) {
		free(job);
		callback(DA_COMMAND_NOT_RUN, "out of memory", context);
		return;
	}
	for (i = 0; i < n; i++) {
		job->argv[i] = strdup(argv[i]);
		if (job->argv[i] == NULL) {
			job_free(job);
			callback(DA_COMMAND_NOT_RUN, "out of memory", context);
			return;
		}
	}
	job->options = options;
	job->queue = queue;
	job->callback = callback;
	job->context = context;
	/*
	 * A global queue rather than a serial one: a mount that hangs (a FUSE
	 * helper waiting on a device) must not hold up the next disk's mount.
	 */
	dispatch_async_f(dispatch_get_global_queue(0, 0), job, job_run);
}
