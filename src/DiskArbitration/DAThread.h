/*
 * DAThread.h — run blocking work off the daemon queue (nextbsd-userland#198).
 *
 * Apple's diskarbitrationd/DAThread.c (DiskArbitration-535.120.1.0.1):
 * DAThreadExecute runs a function on a worker thread and hands its status
 * back to a callback on the daemon's main loop. Here the worker is a
 * libdispatch global queue and the callback runs on the daemon queue.
 * The function must not touch the disk model; the request (DARequest.c)
 * copies what it needs before the call.
 */
#ifndef DISKARBITRATION_DATHREAD_H
#define DISKARBITRATION_DATHREAD_H

#include <dispatch/dispatch.h>

typedef int (*DAThreadFunction)(void *context);
typedef void (*DAThreadExecuteCallback)(int status, void *context);

/* Set the daemon queue the callbacks run on. Call once, first. */
void	DAThreadInit(dispatch_queue_t queue);

void	DAThreadExecute(DAThreadFunction function, void *functionContext,
	    DAThreadExecuteCallback callback, void *callbackContext);

#endif /* DISKARBITRATION_DATHREAD_H */
