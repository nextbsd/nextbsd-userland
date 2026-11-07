/*
 * os/assumes.h — FreeBSD shim for Apple's os/assumes.h.
 *
 * Apple's os_assumes / os_assumes_zero are "soft assert" macros: they
 * evaluate the expression, and on the failure path log a diagnostic
 * (and on Apple, file a telemetry report) but do NOT abort — they
 * return the value so the caller can keep going. launchd-842's
 * libvproc.c uses os_assumes_zero() to wrap proc_track_dirty() /
 * proc_set_dirty() calls whose failure is non-fatal.
 *
 * Our shim keeps the "evaluate, return the value, don't abort"
 * contract but drops the telemetry. Failures are logged the way
 * Darwin's libplatform does it, never with a raw write to stderr:
 *
 *   - the _ctx variants call their callback first (launchd's
 *     job_log_bug / jobmgr_log_bug -> job_log / jobmgr_log ->
 *     launchd_syslog);
 *   - otherwise, or if the callback returns false, the image's
 *     os_redirect_assumes() function is called (launchd's
 *     _launchd_os_redirect -> launchd_syslog);
 *   - an image with no redirect (liblaunch.so in other processes,
 *     launchctl) falls back to syslog(3) at LOG_ERR, as Darwin's
 *     default does.
 *
 * Darwin finds an image's redirect through a __DATA,__os_assumes_log
 * section. Here os_redirect_assumes() defines _os_assumes_redirect and
 * every unit refers to it weakly, so a process without one sees NULL.
 * launchd is the only definer; liblaunch.so inside launchd binds to
 * launchd's, so libvproc failures in PID 1 also reach launchd_syslog.
 * (Default visibility on purpose: a hidden weak undefined reference
 * would need a PC-relative address that can be NULL.)
 * The harder os_assert* (abort-on-failure) variants map to assert().
 */
#ifndef _OS_ASSUMES_H_SHIM_
#define _OS_ASSUMES_H_SHIM_

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <syslog.h>

typedef bool (*_os_redirect_assumes_fn_t)(const char *);

/* Defined by os_redirect_assumes() in at most one unit per process. */
extern _os_redirect_assumes_fn_t _os_assumes_redirect __attribute__((weak));

#define _OS_ASSUMES_MSGLEN	512

static __inline void
_os_assumes_log(const char *message)
{
	_os_redirect_assumes_fn_t fn =
	    (&_os_assumes_redirect != NULL) ? _os_assumes_redirect : NULL;

	if (fn != NULL && fn(message)) {
		return;
	}
	syslog(LOG_ERR, "%s", message);
}

/*
 * The _ctx path: Darwin calls the callback with (asl message, ctx,
 * message) and falls back to the default log if it returns false.
 * There is no ASL message object here, so the first argument is NULL;
 * launchd's callbacks ignore it.
 */
#define _os_assumes_log_ctx(func, ctx, message) do { \
	if (!(func)(NULL, (ctx), (message))) { \
		_os_assumes_log(message); \
	} \
} while (0)

#define _os_assumes_fmt_zero(buf, e, r) \
	snprintf((buf), _OS_ASSUMES_MSGLEN, \
	    "os_assumes_zero: %s == %lld (0x%llx) at %s:%d", \
	    #e, (long long)(r), (unsigned long long)(r), __FILE__, __LINE__)

#define _os_assumes_fmt_true(buf, e) \
	snprintf((buf), _OS_ASSUMES_MSGLEN, \
	    "os_assumes: %s is false at %s:%d", #e, __FILE__, __LINE__)

#define _os_assumes_fmt_posix(buf, e, err) \
	snprintf((buf), _OS_ASSUMES_MSGLEN, \
	    "posix_assumes_zero: %s == -1 (errno %d) at %s:%d", \
	    #e, (err), __FILE__, __LINE__)

/*
 * os_assumes_zero(e) — evaluate e, expect it to be zero. Returns the
 * value of e regardless. Logs if nonzero. Implemented as a
 * statement-expression so the value propagates and e is evaluated
 * exactly once.
 */
#define os_assumes_zero(e) __extension__({ \
	__typeof__(0 ? (e) : (e)) _r = (e); \
	if (_r != 0) { \
		char _m[_OS_ASSUMES_MSGLEN]; \
		_os_assumes_fmt_zero(_m, e, _r); \
		_os_assumes_log(_m); \
	} \
	_r; \
})

/* os_assumes(e) — expect e to be true; return e either way. */
#define os_assumes(e) __extension__({ \
	__typeof__(0 ? (e) : (e)) _r = (e); \
	if (!_r) { \
		char _m[_OS_ASSUMES_MSGLEN]; \
		_os_assumes_fmt_true(_m, e); \
		_os_assumes_log(_m); \
	} \
	_r; \
})

/*
 * posix_assumes_zero(e) — the POSIX-call sibling of os_assumes_zero.
 * launchd-842 wraps raw syscalls with it: a return of -1 is the
 * failure signal, so log errno on that path. Returns e unchanged so
 * callers can still inspect the result (many do `... != -1`). errno is
 * preserved across the logging for those callers.
 */
#define posix_assumes_zero(e) __extension__({ \
	__typeof__(0 ? (e) : (e)) _r = (e); \
	if (_r == -1) { \
		int _e = errno; \
		char _m[_OS_ASSUMES_MSGLEN]; \
		_os_assumes_fmt_posix(_m, e, _e); \
		_os_assumes_log(_m); \
		errno = _e; \
	} \
	_r; \
})

/* os_assert* / posix_assert_zero — hard assert variants. */
#define os_assert(e)		assert(e)
#define os_assert_zero(e)	assert((e) == 0)
#define posix_assert_zero(e)	assert((e) != -1)

/*
 * os_redirect_assumes(fn) — Apple's registration hook: failed
 * assumptions (outside the _ctx variants, or when their
 * callback declines) are passed to fn(message) instead of the default
 * log. Used at file scope with a trailing `;`. fn must be declared
 * before the use.
 */
#define os_redirect_assumes(fn) \
	__attribute__((used)) \
	_os_redirect_assumes_fn_t _os_assumes_redirect = (fn)

/*
 * _ctx variants — Apple's os/assumes.h passes the failure to
 * redirect(asl_message, ctx, message) first. launchd-842 wraps these
 * into job_assumes* / jobmgr_assumes* with job_log_bug / jobmgr_log_bug,
 * so a failure is logged against its job or job manager.
 */
#define os_assumes_ctx(redirect, ctx, e) __extension__({ \
	__typeof__(0 ? (e) : (e)) _r = (e); \
	if (!_r) { \
		char _m[_OS_ASSUMES_MSGLEN]; \
		_os_assumes_fmt_true(_m, e); \
		_os_assumes_log_ctx(redirect, ctx, _m); \
	} \
	_r; \
})

#define os_assumes_zero_ctx(redirect, ctx, e) __extension__({ \
	__typeof__(0 ? (e) : (e)) _r = (e); \
	if (_r != 0) { \
		char _m[_OS_ASSUMES_MSGLEN]; \
		_os_assumes_fmt_zero(_m, e, _r); \
		_os_assumes_log_ctx(redirect, ctx, _m); \
	} \
	_r; \
})

#define posix_assumes_zero_ctx(redirect, ctx, e) __extension__({ \
	__typeof__(0 ? (e) : (e)) _r = (e); \
	if (_r == -1) { \
		int _e = errno; \
		char _m[_OS_ASSUMES_MSGLEN]; \
		_os_assumes_fmt_posix(_m, e, _e); \
		_os_assumes_log_ctx(redirect, ctx, _m); \
		errno = _e; \
	} \
	_r; \
})

/*
 * __OS_COMPILETIME_ASSERT__(e) — Apple's compile-time assert builtin
 * used like `__OS_COMPILETIME_ASSERT__(sizeof(x) == 4);` at function
 * scope. _Static_assert is the C11 equivalent.
 */
#define __OS_COMPILETIME_ASSERT__(e)	_Static_assert((e), #e)

#endif /* !_OS_ASSUMES_H_SHIM_ */
