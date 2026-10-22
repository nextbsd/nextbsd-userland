/*
 * os_alloc_once.c — Apple's once-per-slot allocator for libnotify.
 *
 * The shim this replaces was a static inline that called calloc()+init()
 * unconditionally and ignored the token, while its comment said it allocated
 * once per slot (nextbsd-userland#188). libnotify calls it from
 * _notify_globals() (notify_client.c) at the top of every public entry
 * point, so every call got a fresh, zeroed globals struct:
 *
 *   - registration_table and name_node_table were empty on entry, so
 *     notify_check(), notify_get_state() and notify_cancel() never found a
 *     token they had returned, and notify_register_dispatch() failed with
 *     NOTIFY_STATUS_FAILED when its own registration_node_find() missed.
 *   - notify_server_port and notify_common_port were always MACH_PORT_NULL,
 *     so every call re-ran _notify_lib_init (bootstrap look-up, checkin,
 *     _notify_generate_common_port) and notifyd made a new port per call.
 *   - client_opts was always zero, so NOTIFY_OPT_DISABLE (set by notifyd
 *     and by _notify_fork_child) never stuck.
 *   - A globals struct and its hash tables leaked on every call.
 *
 * The table lives here, in a .c, so there is exactly one per library: a
 * static in the header would give each translation unit its own table and
 * its own "singleton".
 *
 * Fork: the child inherits the table and the slots already published, so
 * _notify_fork_child() resets the parent's globals as Apple intends. Only a
 * fork while another thread is allocating a slot for the first time could
 * inherit a held mutex; that window is once per slot per process.
 */

#include <sys/cdefs.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include <os/alloc_once_private.h>

static void *os_alloc_once_slots[OS_ALLOC_ONCE_KEY_MAX + 1];
static pthread_mutex_t os_alloc_once_mtx = PTHREAD_MUTEX_INITIALIZER;

void *
os_alloc_once(os_alloc_token_t token, size_t sz, void (*init)(void *))
{
	void *p;

	if ((unsigned)token > OS_ALLOC_ONCE_KEY_MAX) {
		fprintf(stderr, "os_alloc_once: token %d out of range\n",
		    (int)token);
		abort();
	}

	/*
	 * Fast path. A slot is published (release) only after init() has run,
	 * so a non-NULL acquire load always sees a fully initialised block.
	 */
	p = __atomic_load_n(&os_alloc_once_slots[token], __ATOMIC_ACQUIRE);
	if (p != NULL)
		return (p);

	pthread_mutex_lock(&os_alloc_once_mtx);
	p = os_alloc_once_slots[token];
	if (p == NULL) {
		p = calloc(1, sz);
		if (p == NULL) {
			/*
			 * Every caller dereferences the result at once, so a
			 * NULL would only fault one frame up with no clue why.
			 * Apple's os_alloc_once does not return NULL either.
			 */
			fprintf(stderr, "os_alloc_once: out of memory "
			    "allocating %zu bytes for slot %d\n", sz,
			    (int)token);
			abort();
		}
		if (init != NULL)
			init(p);
		__atomic_store_n(&os_alloc_once_slots[token], p,
		    __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&os_alloc_once_mtx);

	return (p);
}
