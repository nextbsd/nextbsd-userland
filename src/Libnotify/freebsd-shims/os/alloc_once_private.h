/* os/alloc_once_private.h — FreeBSD shim. Apple's slot-indexed
 * once-init allocation. Used for libsystem-wide singletons. Map to
 * a mutex-guarded slot table + heap allocation. */
#ifndef _FREEBSD_SHIM_OS_ALLOC_ONCE_PRIVATE_H_
#define _FREEBSD_SHIM_OS_ALLOC_ONCE_PRIVATE_H_

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Apple defines os_alloc_token_* enum slots. Like Apple's, ours index
 * a fixed per-process table and allocate a slot on first use. */
typedef int os_alloc_token_t;

/* Apple's predefined slot keys. We just pick numbers; each must be
 * unique and no greater than OS_ALLOC_ONCE_KEY_MAX, which indexes the
 * slot table in os_alloc_once.c. */
#define OS_ALLOC_ONCE_KEY_LIBSYSTEM_NOTIFY	1
#define OS_ALLOC_ONCE_KEY_LIBXPC		2
#define OS_ALLOC_ONCE_KEY_LIBDISPATCH		3
#define OS_ALLOC_ONCE_KEY_LIBSYSTEM_C		4
#define OS_ALLOC_ONCE_KEY_LIBSYSTEM_INFO	5
#define OS_ALLOC_ONCE_KEY_LIBSYSTEM_NETWORK	6
#define OS_ALLOC_ONCE_KEY_LIBSYSTEM_ASL		7
#define OS_ALLOC_ONCE_KEY_LIBSYSTEM_TRACE	8
#define OS_ALLOC_ONCE_KEY_MAX			32

/*
 * Generic once-allocator, as on Apple: one zero-filled block per slot,
 * init() run exactly once, and every later call for that slot returns
 * the same pointer for the life of the process.
 *
 * Declared here and defined in freebsd-shims/os_alloc_once.c, so
 * libnotify has a single slot table. The old static inline ignored the
 * token and called calloc()+init() on every call (nextbsd-userland#188),
 * and even a correct static inline would give each translation unit its
 * own table. Hidden, so the table stays private to libnotify.so.
 */
__attribute__((visibility("hidden")))
void *os_alloc_once(os_alloc_token_t token, size_t sz, void (*init)(void *));

#endif
