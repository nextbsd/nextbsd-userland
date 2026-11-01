/*
 * DAThread.c — run blocking work off the daemon queue (nextbsd-userland#198).
 *
 * Port of Apple's diskarbitrationd/DAThread.c (DiskArbitration-535.120.1.0.1).
 * Apple keeps a pool of pthreads and signals the main run loop through a
 * Mach port; a global dispatch queue and dispatch_async_f back to the
 * daemon queue do the same here.
 */
#include "DAThread.h"

#include <stdlib.h>

struct da_thread_job {
	DAThreadFunction	 function;
	void			*function_context;
	DAThreadExecuteCallback	 callback;
	void			*callback_context;
	int			 status;
};

static dispatch_queue_t	g_queue;

void
DAThreadInit(dispatch_queue_t queue)
{
	g_queue = queue;
}

/* On the daemon queue: __DAThreadFunctionCallback. */
static void
thread_done(void *context)
{
	struct da_thread_job *job = context;

	if (job->callback != NULL)
		job->callback(job->status, job->callback_context);
	free(job);
}

/* On a worker thread: __DAThreadFunction. */
static void
thread_run(void *context)
{
	struct da_thread_job *job = context;

	job->status = job->function(job->function_context);
	dispatch_async_f(g_queue, job, thread_done);
}

void
DAThreadExecute(DAThreadFunction function, void *functionContext,
    DAThreadExecuteCallback callback, void *callbackContext)
{
	struct da_thread_job *job;

	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		/* Out of memory: run it here rather than lose the request. */
		int status = function(functionContext);

		if (callback != NULL)
			callback(status, callbackContext);
		return;
	}
	job->function = function;
	job->function_context = functionContext;
	job->callback = callback;
	job->callback_context = callbackContext;
	dispatch_async_f(dispatch_get_global_queue(
	    DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), job, thread_run);
}
