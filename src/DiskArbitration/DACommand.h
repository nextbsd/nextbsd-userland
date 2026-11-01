/*
 * DACommand.h — run a helper program (E15 C5, nextbsd-userland#195).
 *
 * Apple's DACommand.c forks and execs a helper (the fs-bundle utilities,
 * mount(8)), captures its stdout and hands the exit status back as
 * (char)WEXITSTATUS. Here it is posix_spawn with a pipe; DACommandRun is the
 * synchronous core the probe queue uses, DACommandExecute the asynchronous
 * form the mount stage uses.
 */
#ifndef DISKARBITRATION_DACOMMAND_H
#define DISKARBITRATION_DACOMMAND_H

#include <dispatch/dispatch.h>

#include <stddef.h>

/* Status for a helper that could not be started or did not exit. */
#define DA_COMMAND_NOT_RUN	(-1000)

enum {
	kDACommandExecuteOptionDefault		= 0,
	kDACommandExecuteOptionCaptureStderr	= 1,	/* stderr into `out` too */
};

/*
 * Run argv[0] (an absolute path) with `argv` and a minimal environment
 * (PATH, plus `env_extra` when not NULL, "NAME=value"). The first line of
 * its output, trimmed, lands in `out`; the status is (signed char)
 * WEXITSTATUS, or DA_COMMAND_NOT_RUN. Blocks: call it off the daemon queue.
 */
int	DACommandRun(char *const argv[], const char *env_extra, int options,
	    char *out, size_t len);

typedef void (*DACommandCallback)(int status, const char *output,
	    void *context);

/*
 * DACommandRun on a background queue; `callback` then runs on `queue`.
 * argv is copied, so the caller's strings may go away.
 */
void	DACommandExecute(char *const argv[], int options,
	    dispatch_queue_t queue, DACommandCallback callback, void *context);

#endif /* DISKARBITRATION_DACOMMAND_H */
