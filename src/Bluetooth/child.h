/*
 * child.h — bluetoothd's child-process supervisor (#218, taking over #212's).
 *
 * The one program bluetoothd runs is virtual_oss, NextBSD's base audio mixer,
 * for an A2DP sink it plays to (audio.c). Nothing here knows that: a child is
 * a path, an argv and an exit callback.
 *
 *   spawn     posix_spawn with every descriptor above stderr closed (the HCI,
 *             L2CAP and Mach-side sockets stay bluetoothd's), stdin from
 *             /dev/null, stdout and stderr inherited (launchd's log), and the
 *             signal mask and dispositions reset
 *   exit      a DISPATCH_SOURCE_TYPE_PROC source on the caller's queue; the
 *             handler reaps the child with waitpid and calls `fn` once, with
 *             the wait status, then frees the child
 *   stop      SIGTERM, then SIGKILL if it is still there after `grace_ms`;
 *             the exit callback still comes, from the reap
 *
 * Restart policy is the caller's: virtual_oss never exits by itself when its
 * Bluetooth sink goes away (it retries every two seconds and pages the
 * headset each time), so restart-on-exit alone would be wrong. audio.c stops
 * it when the link drops and restarts it, with a backoff, only while the link
 * is up.
 *
 * Not a launchd job per child: its lifetime is the link's, which launchd
 * cannot see.
 */
#ifndef _BLUETOOTHD_CHILD_H_
#define _BLUETOOTHD_CHILD_H_

#include <sys/types.h>

#include <dispatch/dispatch.h>

struct child;	/* opaque */

/*
 * Called once on `q` when the child has exited and been reaped. `status` is
 * waitpid's. The child is freed when this returns: drop every pointer to it.
 */
typedef void	(*child_exit_fn)(void *ctx, struct child *c, int status);

/*
 * Start `path` with `argv` (NULL-terminated, argv[0] included). Returns NULL
 * with errno set when it could not be started; `fn` is then never called.
 */
struct child	*child_spawn(const char *path, char *const argv[],
		    dispatch_queue_t q, child_exit_fn fn, void *ctx);

/* Ask the child to exit; kill it after `grace_ms`. Safe to call again. */
void		child_stop(struct child *c, int grace_ms);

pid_t		child_pid(const struct child *c);

#endif /* _BLUETOOTHD_CHILD_H_ */
