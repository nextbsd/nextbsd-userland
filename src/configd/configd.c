/*
 * configd.c — SCDynamicStore configuration daemon (freebsd-launchd-mach
 * port).
 *
 * Apple's configd hosts the SCDynamicStore — a system-wide key->plist
 * store with change notifications — over the MIG `config` subsystem
 * (config.defs, base id 20000). This port keeps that real Mach IPC
 * design (see the configd-port memory / plan).
 *
 * configd checks the bootstrap service in with launchd and runs a raw
 * mach_msg receive loop that hands each message to the MIG-generated
 * config_server() demux. Apple's configd.tproj drives IPC through
 * libdispatch's dispatch_mach channel API; that API is unfinished in
 * this port, so configd uses a raw mach_msg loop instead — the same
 * proven pattern hwregd and launchd use here.
 *
 * iter 4 adds per-session ports and change notifications:
 *   - configopen allocates a fresh per-session Mach port (config_session.c),
 *     hands the client a send right, and arms a no-senders notification
 *     so the session is torn down when the client exits. Every session
 *     port joins a port set, so the one receive loop serves them all;
 *     the port a request arrives on (the MIG `server` argument) names
 *     the session.
 *   - notifyadd / notifyviaport / notifychanges let a session watch
 *     explicit keys or POSIX-regex patterns (iter 5), register a
 *     notification port, and drain the keys that changed. configset /
 *     configremove / confignotify fan a change out to every watching
 *     session.
 *   - configlist (iter 6) lists store keys by prefix or POSIX regex.
 *   - notifyset / configget_m / configset_m (iter 7) do batch watch
 *     registration, multi-key fetch and multi-key update.
 * notifyviafd and snapshot remain stubs (see their handlers).
 *
 * config.defs carries the key/value payloads out-of-line, as Apple's
 * does: a handler owns each xmlData_t argument and vm_deallocate()s it
 * before returning, and hands an xmlDataOut_t reply back in a
 * vm_allocate()d buffer that MIG's `dealloc` releases on send.
 * configopen receives the client's kernel-stamped audit token, which
 * the session keeps as the client's identity.
 *
 * `configd --selftest` runs an in-process round trip (a server thread
 * plus the client stubs) — no launchd or bootstrap — as a quick
 * self-check of the config.defs RPC and notification path.
 */

#include <sys/types.h>

#include <mach/mach.h>
#include <mach/mig_errors.h>		/* mig_reply_error_t */
#include <mach/notify.h>		/* MACH_NOTIFY_NO_SENDERS */
#include <servers/bootstrap.h>

#include <pthread.h>
#include <regex.h>			/* regcomp / regexec — configlist isRegex */
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config_types.h"		/* SCD_SERVER, kSCStatus*, CONFIG_DATA_MAX */
#include "config_store.h"
#include "config_session.h"
#include "config_wire.h"	/* keylist / kvmap batch encodings */
#include "config_link_monitor.h"	/* in-process KernelEventMonitor (#257) */
#include "configServer.h"	/* MIG: config_server() demux + _config* protos */

/*
 * Message buffer. config.defs carries the xmlData payloads
 * out-of-line, so a request is only a header, descriptors, scalars
 * and the audit trailer; 32 KiB is generous.
 */
#define CONFIGD_MSG_BUFSZ		32768

/* Apple's <mach/vm_statistics.h> value; libmach's headers lack it. */
#ifndef VM_FLAGS_ANYWHERE
#define VM_FLAGS_ANYWHERE		0x0001
#endif

/*
 * Receive options for the serve loop: ask the kernel to append the
 * sender's audit token, which configopen's ServerAuditToken argument
 * reads from the trailer (MIG rejects the request without it).
 */
#define CONFIGD_RCV_TRAILER						\
	(MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |		\
	 MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT))

static volatile sig_atomic_t	got_term;

static void
on_signal(int sig)
{
	got_term = sig;
}

/* Timestamped diagnostic line to stderr (the configd launchd plist
 * routes stderr to /var/log/configd.stderr, the CI boot test's path). */
static void
clog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "configd %s ", tbuf);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * kvmap_has — 1 if the wire_kvmap-encoded buffer `buf` (of `len`
 * bytes) already holds a record for key/klen. configget_m uses it to
 * keep its reply a set: a key matched by several patterns, or both
 * requested and matched, is emitted once.
 */
static int
kvmap_has(const uint8_t *buf, size_t len, const void *key, size_t klen)
{
	const uint8_t	*cur = buf;
	const uint8_t	*end = buf + len;
	const void	*k;
	const void	*v;
	size_t		kl;
	size_t		vl;

	while (wire_kvmap_next(&cur, end, &k, &kl, &v, &vl)) {
		if (kl == klen && memcmp(k, key, klen) == 0)
			return 1;
	}
	return 0;
}

/*
 * ool_release — give back an out-of-line request argument. The kernel
 * copied it out into a fresh mapping in configd, which the handler owns
 * once it returns KERN_SUCCESS (Apple's configd does the same).
 */
static void
ool_release(xmlData_t data, mach_msg_type_number_t len)
{
	if (data != NULL && len != 0)
		(void)vm_deallocate(mach_task_self(),
		    (vm_address_t)(uintptr_t)data, len);
}

/*
 * A reply payload under construction: a malloc'd buffer that grows as
 * records are appended, then is copied into the vm_allocate()d memory
 * the reply carries out-of-line.
 */
struct obuf {
	uint8_t	*buf;
	size_t	cap;
	size_t	off;
};

/* Make room for `need` more bytes. Returns 0, or -1 on overflow / ENOMEM. */
static int
obuf_reserve(struct obuf *b, size_t need)
{
	size_t	ncap;
	uint8_t	*nbuf;

	if (need > SIZE_MAX - b->off)
		return -1;
	if (b->off + need <= b->cap)
		return 0;
	ncap = (b->cap != 0) ? b->cap : 4096;
	while (ncap < b->off + need) {
		if (ncap > SIZE_MAX / 2)
			return -1;
		ncap *= 2;
	}
	nbuf = realloc(b->buf, ncap);
	if (nbuf == NULL)
		return -1;
	b->buf = nbuf;
	b->cap = ncap;
	return 0;
}

static int
obuf_keylist_put(struct obuf *b, const void *key, size_t klen)
{
	if (obuf_reserve(b, sizeof(uint32_t) + klen) != 0)
		return -1;
	return wire_keylist_put(b->buf, b->cap, &b->off, key, klen);
}

static int
obuf_kvmap_put(struct obuf *b, const void *key, size_t klen,
    const void *val, size_t vlen)
{
	if (obuf_reserve(b, 2 * sizeof(uint32_t) + klen + vlen) != 0)
		return -1;
	return wire_kvmap_put(b->buf, b->cap, &b->off, key, klen, val, vlen);
}

/*
 * ool_reply — copy `len` bytes at `bytes` into a fresh vm_allocate()d
 * buffer for an xmlDataOut_t reply argument. config.defs marks those
 * arguments `dealloc`, so the kernel unmaps the buffer from configd as
 * the reply is sent. An empty payload goes out as NULL / 0. Returns 0,
 * or -1 if the buffer could not be allocated.
 */
static int
ool_reply(const void *bytes, size_t len, xmlDataOut_t *out,
    mach_msg_type_number_t *outCnt)
{
	vm_address_t	addr = 0;

	*out = NULL;
	*outCnt = 0;
	if (len == 0)
		return 0;
	if (len > UINT32_MAX)
		return -1;
	if (vm_allocate(mach_task_self(), &addr, len, VM_FLAGS_ANYWHERE) !=
	    KERN_SUCCESS)
		return -1;
	memcpy((void *)addr, bytes, len);
	*out = (xmlDataOut_t)addr;
	*outCnt = (mach_msg_type_number_t)len;
	return 0;
}

/*
 * write_allowed — may the session on `server` change `key`? Apple's
 * configd (session.c hasWriteAccess) refuses every write from a non-root
 * client that lacks the SCDynamicStore write-access entitlement. This
 * port has no entitlements and its daemons have long written as whatever
 * uid launchd gives them, so for now only the keys that grant something
 * are guarded: State:/Users/ (State:/Users/ConsoleUser names the console
 * user, which diskarbitrationd gives removable media to; #199) is
 * root-only, as on Apple. A write that does not arrive on a session
 * (configopen) carries no identity and is refused for those keys too.
 */
#define CONFIG_USERS_PREFIX	"State:/Users/"

static int
write_allowed(mach_port_t server, const void *key, size_t klen)
{
	if (klen < sizeof(CONFIG_USERS_PREFIX) - 1 ||
	    memcmp(key, CONFIG_USERS_PREFIX,
	    sizeof(CONFIG_USERS_PREFIX) - 1) != 0)
		return 1;
	if (session_has_root_access(server))
		return 1;
	clog("refused a non-root write to %.*s", (int)klen,
	    (const char *)key);
	return 0;
}

/*
 * MIG routine handlers — config_server() dispatches to these. An
 * xmlData_t argument is out-of-line memory the handler releases with
 * ool_release() before returning; an xmlDataOut_t reply is built with
 * ool_reply(). The matching mach_msg_type_number_t carries the length.
 * Out-parameters are given a safe value first so the MIG reply
 * marshalling never ships an uninitialised port or count. configadd_s,
 * notifyviafd and snapshot are still stubs returning kSCStatusFailed.
 */

kern_return_t
_configopen(mach_port_t server, xmlData_t name,
    mach_msg_type_number_t nameCnt, xmlData_t options,
    mach_msg_type_number_t optionsCnt, mach_port_t *session, int *status,
    audit_token_t audit_token)
{
	/*
	 * Hand the client its own per-session port. session_open()
	 * allocates the receive right, inserts a MAKE_SEND right (MIG's
	 * mach_port_move_send_t moves it out in the reply), joins it to
	 * configd's port set and arms the no-senders notification. The
	 * session records the client's audit token.
	 */
	(void)server;
	ool_release(name, nameCnt);
	ool_release(options, optionsCnt);

	if (session_open(session, &audit_token) != 0) {
		*session = MACH_PORT_NULL;
		*status = kSCStatusFailed;
		return KERN_SUCCESS;
	}
	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

/*
 * configlist (SCDynamicStoreCopyKeyList) — return the store keys that
 * match `key`: with isRegex set, the keys matching `key` as a POSIX
 * regex; otherwise the keys prefixed by `key` (an empty `key` lists
 * every key). The result is the keylist_append [len][bytes] encoding.
 */
kern_return_t
_configlist(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    int isRegex, xmlDataOut_t *list, mach_msg_type_number_t *listCnt,
    int *status)
{
	struct obuf	out = { NULL, 0, 0 };
	size_t		count, i;
	regex_t		re;
	int		have_re = 0;

	(void)server;
	*list = NULL;
	*listCnt = 0;

	if (isRegex != 0) {
		char buf[CONFIG_DATA_MAX + 3];	/* ^ + pattern + $ + NUL */

		if (config_pattern_anchor(key, keyCnt, buf, sizeof(buf)) != 0 ||
		    regcomp(&re, buf, REG_EXTENDED) != 0) {
			*status = kSCStatusInvalidArgument;
			goto done;
		}
		have_re = 1;
	}

	*status = kSCStatusOK;
	count = store_count();
	for (i = 0; i < count; i++) {
		const void	*skey;
		size_t		sklen;
		int		match;

		if (store_key_at(i, &skey, &sklen) != 0)
			continue;

		if (isRegex != 0) {
			char keystr[CONFIG_DATA_MAX + 1];	/* NUL-term */

			if (sklen >= sizeof(keystr))
				continue;	/* key too long to match */
			memcpy(keystr, skey, sklen);
			keystr[sklen] = '\0';
			match = (regexec(&re, keystr, 0, NULL, 0) == 0);
		} else {
			/* prefix match; an empty key matches every key */
			match = (keyCnt == 0) ||
			    (sklen >= keyCnt &&
			     memcmp(skey, key, keyCnt) == 0);
		}

		if (match && obuf_keylist_put(&out, skey, sklen) != 0) {
			*status = kSCStatusFailed;
			break;
		}
	}

	if (*status == kSCStatusOK &&
	    ool_reply(out.buf, out.off, list, listCnt) != 0)
		*status = kSCStatusFailed;

	if (have_re)
		regfree(&re);
done:
	free(out.buf);
	ool_release(key, keyCnt);
	return KERN_SUCCESS;
}

/*
 * configadd (SCDynamicStoreAddValue) — store a key/value, but only if
 * the key does not already exist. An existing key is left untouched
 * and reported as kSCStatusKeyExists; this is what distinguishes add
 * from set. Existence check via store_get, like _configget; the store
 * + change-notification path is _configset's.
 */
kern_return_t
_configadd(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    xmlData_t data, mach_msg_type_number_t dataCnt, int *newInstance,
    int *status)
{
	const void	*val;
	size_t		vlen;

	*newInstance = 0;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (!write_allowed(server, key, keyCnt)) {
		*status = kSCStatusAccessError;
	} else if (store_get(key, keyCnt, &val, &vlen) == 0) {
		/* key already defined — add must not clobber it */
		*status = kSCStatusKeyExists;
	} else if (store_set(key, keyCnt, data, dataCnt) != 0) {
		*status = kSCStatusFailed;
	} else {
		/* a new key is a change watching sessions must hear about */
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
	ool_release(key, keyCnt);
	ool_release(data, dataCnt);
	return KERN_SUCCESS;
}

kern_return_t
_configget(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    xmlDataOut_t *data, mach_msg_type_number_t *dataCnt, int *newInstance,
    int *status)
{
	const void	*val;
	size_t		vlen;

	(void)server;
	*data = NULL;
	*dataCnt = 0;
	*newInstance = 0;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (store_get(key, keyCnt, &val, &vlen) != 0) {
		*status = kSCStatusNoKey;
	} else if (ool_reply(val, vlen, data, dataCnt) != 0) {
		*status = kSCStatusFailed;
	} else {
		*status = kSCStatusOK;
	}
	ool_release(key, keyCnt);
	return KERN_SUCCESS;
}

kern_return_t
_configset(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    xmlData_t data, mach_msg_type_number_t dataCnt, int instance,
    int *newInstance, int *status)
{
	(void)instance;		/* the "instance" generation count is unused */
	*newInstance = 0;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (!write_allowed(server, key, keyCnt)) {
		*status = kSCStatusAccessError;
	} else if (store_set(key, keyCnt, data, dataCnt) != 0) {
		*status = kSCStatusFailed;
	} else {
		/* fan the change out to every watching session */
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
	ool_release(key, keyCnt);
	ool_release(data, dataCnt);
	return KERN_SUCCESS;
}

kern_return_t
_configremove(mach_port_t server, xmlData_t key,
    mach_msg_type_number_t keyCnt, int *status)
{
	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (!write_allowed(server, key, keyCnt)) {
		*status = kSCStatusAccessError;
	} else if (store_remove(key, keyCnt) != 0) {
		*status = kSCStatusNoKey;
	} else {
		/* removing a key is a change watchers must hear about */
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
	ool_release(key, keyCnt);
	return KERN_SUCCESS;
}

kern_return_t
_configadd_s(mach_port_t server, xmlData_t key,
    mach_msg_type_number_t keyCnt, xmlData_t data,
    mach_msg_type_number_t dataCnt, int *newInstance, int *status)
{
	(void)server;
	ool_release(key, keyCnt);
	ool_release(data, dataCnt);
	*newInstance = 0;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

/*
 * confignotify (SCDynamicStoreNotifyValue) — force a change
 * notification for a key without touching its value.
 */
kern_return_t
_confignotify(mach_port_t server, xmlData_t key,
    mach_msg_type_number_t keyCnt, int *status)
{
	(void)server;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else {
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
	ool_release(key, keyCnt);
	return KERN_SUCCESS;
}

/*
 * configget_m (SCDynamicStoreCopyMultiple) — fetch several store
 * values at once: the values for the requested `keys` that exist,
 * plus the value of every store key matching one of the `patterns`.
 * keys and patterns are wire_keylist payloads; the reply `data` is a
 * wire_kvmap payload. Each key is emitted at most once.
 */
kern_return_t
_configget_m(mach_port_t server, xmlData_t keys,
    mach_msg_type_number_t keysCnt, xmlData_t patterns,
    mach_msg_type_number_t patternsCnt, xmlDataOut_t *data,
    mach_msg_type_number_t *dataCnt, int *status)
{
	struct obuf	out = { NULL, 0, 0 };
	const uint8_t	*cur, *end;
	const void	*item;
	size_t		ilen;

	(void)server;
	*data = NULL;
	*dataCnt = 0;
	*status = kSCStatusOK;

	/* Requested explicit keys: emit the value of each that exists. */
	cur = (const uint8_t *)keys;
	end = cur + keysCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		const void	*val;
		size_t		vlen;

		if (store_get(item, ilen, &val, &vlen) != 0)
			continue;	/* no such key — skip it */
		if (kvmap_has(out.buf, out.off, item, ilen))
			continue;	/* already emitted */
		if (obuf_kvmap_put(&out, item, ilen, val, vlen) != 0) {
			*status = kSCStatusFailed;
			goto done;
		}
	}

	/* Pattern keys: emit the value of every store key that matches. */
	cur = (const uint8_t *)patterns;
	end = cur + patternsCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		char	abuf[CONFIG_DATA_MAX + 3];	/* ^ + pattern + $ + NUL */
		regex_t	re;
		size_t	i, count;

		if (config_pattern_anchor(item, ilen, abuf, sizeof(abuf)) != 0 ||
		    regcomp(&re, abuf, REG_EXTENDED) != 0) {
			*status = kSCStatusInvalidArgument;
			goto done;
		}

		count = store_count();
		for (i = 0; i < count; i++) {
			const void	*skey, *val;
			size_t		sklen, vlen;
			char		keystr[CONFIG_DATA_MAX + 1];

			if (store_key_at(i, &skey, &sklen) != 0)
				continue;
			if (sklen >= sizeof(keystr))
				continue;	/* too long to match */
			memcpy(keystr, skey, sklen);
			keystr[sklen] = '\0';
			if (regexec(&re, keystr, 0, NULL, 0) != 0)
				continue;	/* no match */
			if (kvmap_has(out.buf, out.off, skey, sklen))
				continue;	/* already emitted */
			if (store_get(skey, sklen, &val, &vlen) != 0)
				continue;
			if (obuf_kvmap_put(&out, skey, sklen, val, vlen) != 0) {
				regfree(&re);
				*status = kSCStatusFailed;
				goto done;
			}
		}
		regfree(&re);
	}

	if (ool_reply(out.buf, out.off, data, dataCnt) != 0)
		*status = kSCStatusFailed;
done:
	free(out.buf);
	ool_release(keys, keysCnt);
	ool_release(patterns, patternsCnt);
	return KERN_SUCCESS;
}

/*
 * configset_m (SCDynamicStoreSetMultiple) — apply a batch of store
 * changes in one call: set every key/value pair in `data` (a
 * wire_kvmap payload), remove every key in `removeData`, and force a
 * change notification for every key in `notify` (both wire_keylist
 * payloads). Each set / remove / notify fans out to watching sessions.
 */
kern_return_t
_configset_m(mach_port_t server, xmlData_t data,
    mach_msg_type_number_t dataCnt, xmlData_t removeData,
    mach_msg_type_number_t removeCnt, xmlData_t notify,
    mach_msg_type_number_t notifyCnt, int *status)
{
	const uint8_t	*cur, *end;
	const void	*k, *v;
	size_t		kl, vl;

	*status = kSCStatusOK;

	/* Every key the batch changes must be writable, or none is. */
	cur = (const uint8_t *)data;
	end = cur + dataCnt;
	while (wire_kvmap_next(&cur, end, &k, &kl, &v, &vl)) {
		if (kl != 0 && !write_allowed(server, k, kl)) {
			*status = kSCStatusAccessError;
			goto done;
		}
	}
	cur = (const uint8_t *)removeData;
	end = cur + removeCnt;
	while (wire_keylist_next(&cur, end, &k, &kl)) {
		if (kl != 0 && !write_allowed(server, k, kl)) {
			*status = kSCStatusAccessError;
			goto done;
		}
	}

	/* Set each key/value pair. */
	cur = (const uint8_t *)data;
	end = cur + dataCnt;
	while (wire_kvmap_next(&cur, end, &k, &kl, &v, &vl)) {
		if (kl == 0)
			continue;	/* skip an empty key */
		if (store_set(k, kl, v, vl) != 0) {
			*status = kSCStatusFailed;
			goto done;
		}
		session_key_changed(k, kl);
	}

	/* Remove each key, notifying watchers of the ones that existed. */
	cur = (const uint8_t *)removeData;
	end = cur + removeCnt;
	while (wire_keylist_next(&cur, end, &k, &kl)) {
		if (kl != 0 && store_remove(k, kl) == 0)
			session_key_changed(k, kl);
	}

	/* Force a change notification for each notify key. */
	cur = (const uint8_t *)notify;
	end = cur + notifyCnt;
	while (wire_keylist_next(&cur, end, &k, &kl)) {
		if (kl != 0)
			session_key_changed(k, kl);
	}

done:
	ool_release(data, dataCnt);
	ool_release(removeData, removeCnt);
	ool_release(notify, notifyCnt);
	return KERN_SUCCESS;
}

/*
 * notifyadd (SCDynamicStoreAddWatchedKey) — add a watch to the session.
 * isRegex selects an explicit-key watch or a POSIX-regex pattern watch.
 */
kern_return_t
_notifyadd(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    int isRegex, int *status)
{
	if (keyCnt == 0)
		*status = kSCStatusInvalidArgument;
	else if (isRegex != 0)
		*status = session_pattern_add(server, key, keyCnt);
	else
		*status = session_watch_add(server, key, keyCnt);
	ool_release(key, keyCnt);
	return KERN_SUCCESS;
}

/*
 * notifyremove (SCDynamicStoreRemoveWatchedKey) — drop a watch from the
 * session; isRegex picks the explicit-key or the pattern watch list.
 */
kern_return_t
_notifyremove(mach_port_t server, xmlData_t key,
    mach_msg_type_number_t keyCnt, int isRegex, int *status)
{
	if (keyCnt == 0)
		*status = kSCStatusInvalidArgument;
	else if (isRegex != 0)
		*status = session_pattern_remove(server, key, keyCnt);
	else
		*status = session_watch_remove(server, key, keyCnt);
	ool_release(key, keyCnt);
	return KERN_SUCCESS;
}

/*
 * notifychanges (SCDynamicStoreCopyNotifiedKeys) — return and clear the
 * session's list of keys that changed since the last call. The list is
 * encoded as a run of [uint32 little-endian length][key bytes] records.
 */
kern_return_t
_notifychanges(mach_port_t server, xmlDataOut_t *list,
    mach_msg_type_number_t *listCnt, int *status)
{
	uint8_t	*buf = NULL;
	size_t	cap = 4096;
	ssize_t	n;

	*list = NULL;
	*listCnt = 0;

	if (!session_valid(server)) {
		*status = kSCStatusNoStoreSession;
		return KERN_SUCCESS;
	}

	/*
	 * session_drain_changes() leaves the pending list in place when it
	 * does not fit, so grow the buffer until the whole list does.
	 */
	for (;;) {
		uint8_t *nbuf = realloc(buf, cap);

		if (nbuf == NULL) {
			n = -1;
			break;
		}
		buf = nbuf;
		n = session_drain_changes(server, buf, cap);
		if (n >= 0 || cap > SIZE_MAX / 2)
			break;
		cap *= 2;
	}

	if (n < 0 || ool_reply(buf, (size_t)n, list, listCnt) != 0)
		*status = kSCStatusFailed;
	else
		*status = kSCStatusOK;
	free(buf);
	return KERN_SUCCESS;
}

/*
 * notifyviaport (SCDynamicStoreNotifyMachPort) — register the Mach
 * port configd messages when a watched key changes. `port` arrives as
 * a moved-in send right configd takes ownership of.
 */
kern_return_t
_notifyviaport(mach_port_t server, mach_port_t port, mach_msg_id_t msgid,
    int *status)
{
	if (msgid != 0) {
		/* Apple's per-message identifier is obsolete; reject it. */
		if (port != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), port);
		*status = kSCStatusInvalidArgument;
		return KERN_SUCCESS;
	}
	*status = session_set_notify_port(server, port);
	return KERN_SUCCESS;
}

/*
 * notifycancel (SCDynamicStoreNotifyCancel) — drop the session's
 * notification port and watch list.
 */
kern_return_t
_notifycancel(mach_port_t server, int *status)
{
	*status = session_clear_notify_port(server);
	return KERN_SUCCESS;
}

/*
 * notifyset (SCDynamicStoreSetNotificationKeys) — replace the
 * session's entire notification key set: after this call it watches
 * exactly the explicit `keys` and the regex `patterns` given (both
 * wire_keylist payloads).
 */
kern_return_t
_notifyset(mach_port_t server, xmlData_t keys,
    mach_msg_type_number_t keysCnt, xmlData_t patterns,
    mach_msg_type_number_t patternsCnt, int *status)
{
	const uint8_t	*cur, *end;
	const void	*item;
	size_t		ilen;
	int		rc;

	if (!session_valid(server)) {
		*status = kSCStatusNoStoreSession;
		goto done;
	}

	/* Drop the old watch set, then install the new one. */
	(void)session_watch_clear(server);

	cur = (const uint8_t *)keys;
	end = cur + keysCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		rc = session_watch_add(server, item, ilen);
		if (rc != kSCStatusOK) {
			*status = rc;
			goto done;
		}
	}

	cur = (const uint8_t *)patterns;
	end = cur + patternsCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		rc = session_pattern_add(server, item, ilen);
		if (rc != kSCStatusOK) {
			*status = rc;
			goto done;
		}
	}

	*status = kSCStatusOK;
done:
	ool_release(keys, keysCnt);
	ool_release(patterns, patternsCnt);
	return KERN_SUCCESS;
}

/*
 * notifyviafd (SCDynamicStoreNotifyFileDescriptor) — stubbed. It would
 * deliver notifications by writing to a client file descriptor passed
 * as a Mach fileport, but this port has no fileport->fd support; the
 * Mach-port path (notifyviaport) is the supported notification route.
 */
kern_return_t
_notifyviafd(mach_port_t server, mach_port_t fileport, int identifier,
    int *status)
{
	(void)server; (void)identifier;
	if (fileport != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), fileport);
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

/*
 * snapshot — stubbed. Apple's configd writes a debug dump of the whole
 * store and session table to a file; it is a root-gated diagnostic
 * with no consumer in this port.
 */
kern_return_t
_snapshot(mach_port_t server, int *status)
{
	(void)server;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

/*
 * configd_serve — the raw mach_msg receive loop. It receives on a
 * port set holding the bootstrap service port and every per-session
 * port. config_server() is the MIG demux for the config subsystem; it
 * writes the routine's reply (or a MIG error) into `rep`. A
 * MACH_NOTIFY_NO_SENDERS message means a client exited — its session
 * port's no-senders notification fired — so the session is closed.
 * The 1s receive timeout lets the loop notice a SIGTERM for a clean
 * shutdown.
 */
static void
configd_serve(mach_port_t port_set)
{
	while (!got_term) {
		union {
			mach_msg_header_t hdr;
			char buf[CONFIGD_MSG_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;
		int rejected;

		memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr,
		    MACH_RCV_MSG | MACH_RCV_TIMEOUT | CONFIGD_RCV_TRAILER, 0,
		    sizeof(req), port_set, 1000, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			clog("mach_msg receive failed: 0x%x — exiting",
			    (unsigned)mr);
			break;
		}

		/* A client exited: tear its session down. */
		if (req.hdr.msgh_id == MACH_NOTIFY_NO_SENDERS) {
			if (session_close(req.hdr.msgh_local_port) == 0)
				clog("session port 0x%x closed (client gone)",
				    (unsigned)req.hdr.msgh_local_port);
			continue;
		}

		memset(&rep, 0, sizeof(rep));
		if (!config_server(&req.hdr, &rep.hdr)) {
			clog("unhandled message id=%d", req.hdr.msgh_id);
			/*
			 * Demux didn't claim the message: its reply send-once
			 * right (and any moved-in rights) are still ours to
			 * dispose, otherwise they leak one Mach port per
			 * undemuxable RPC. See #342.
			 */
			mach_msg_destroy(&req.hdr);
			continue;
		}
		rejected = !(rep.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
		    ((mig_reply_error_t *)(void *)&rep.hdr)->RetCode !=
		    KERN_SUCCESS;
		if (rejected) {
			/*
			 * MIG rejected the request before a handler took
			 * ownership of it (bad arguments, a missing audit
			 * trailer): its out-of-line memory and moved-in
			 * rights are still ours to release, as Apple's
			 * mach_msg_server() does. The reply consumes the
			 * reply right, so keep that out of the destroy.
			 */
			req.hdr.msgh_remote_port = MACH_PORT_NULL;
			mach_msg_destroy(&req.hdr);
		}
		if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
			mr = mach_msg(&rep.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 1000,
			    MACH_PORT_NULL);
			if (mr != MACH_MSG_SUCCESS) {
				clog("reply send failed: 0x%x", (unsigned)mr);
				/*
				 * A failed/timed-out send does not consume the
				 * reply send-once right or any reply-carried
				 * ports — drop them rather than leak.
				 */
				mach_msg_destroy(&rep.hdr);
			}
		} else if (!rejected) {
			/*
			 * MIG produced no reply (simpleroutine, or a routine
			 * that errored before building one): the request's
			 * reply send-once right is still ours to dispose.
			 */
			mach_msg_destroy(&req.hdr);
		}
	}
}

/*
 * configd_make_port_set — allocate the port set configd_serve()
 * receives on and move `initial` (the bootstrap service port, or the
 * selftest port) into it. The session layer adds new session ports to
 * the same set. Returns 0 / -1.
 */
static int
configd_make_port_set(mach_port_t initial, mach_port_t *port_set)
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
	    port_set);
	if (kr != KERN_SUCCESS) {
		clog("port set allocate failed: 0x%x", (unsigned)kr);
		return -1;
	}
	kr = mach_port_move_member(mach_task_self(), initial, *port_set);
	if (kr != KERN_SUCCESS) {
		clog("port set move_member failed: 0x%x", (unsigned)kr);
		return -1;
	}
	session_layer_init(*port_set);
	return 0;
}

/*
 * In-process self test (`configd --selftest`). A server thread runs
 * configd_serve() on a private port set; the main thread drives the
 * config.defs client stubs against it — configopen / configset /
 * configget, then notifyadd / configset / notifychanges — with no
 * launchd or bootstrap in the loop. The client stubs come from the
 * MIG-generated configUser.c.
 */
extern kern_return_t configopen(mach_port_t, xmlData_t,
    mach_msg_type_number_t, xmlData_t, mach_msg_type_number_t,
    mach_port_t *, int *);
extern kern_return_t configset(mach_port_t, xmlData_t,
    mach_msg_type_number_t, xmlData_t, mach_msg_type_number_t, int,
    int *, int *);
extern kern_return_t configget(mach_port_t, xmlData_t,
    mach_msg_type_number_t, xmlDataOut_t *, mach_msg_type_number_t *,
    int *, int *);
extern kern_return_t configlist(mach_port_t, xmlData_t,
    mach_msg_type_number_t, int, xmlDataOut_t *, mach_msg_type_number_t *,
    int *);
extern kern_return_t configget_m(mach_port_t, xmlData_t,
    mach_msg_type_number_t, xmlData_t, mach_msg_type_number_t,
    xmlDataOut_t *, mach_msg_type_number_t *, int *);
extern kern_return_t configset_m(mach_port_t, xmlData_t,
    mach_msg_type_number_t, xmlData_t, mach_msg_type_number_t, xmlData_t,
    mach_msg_type_number_t, int *);
extern kern_return_t notifyadd(mach_port_t, xmlData_t,
    mach_msg_type_number_t, int, int *);
extern kern_return_t notifyviaport(mach_port_t, mach_port_t,
    mach_msg_id_t, int *);
extern kern_return_t notifychanges(mach_port_t, xmlDataOut_t *,
    mach_msg_type_number_t *, int *);

/*
 * selftest_ool_free — release an out-of-line reply the selftest has
 * finished with, and reset it for the next call.
 */
static void
selftest_ool_free(xmlDataOut_t *data, mach_msg_type_number_t *len)
{
	if (*data != NULL && *len != 0)
		(void)vm_deallocate(mach_task_self(), (vm_address_t)*data,
		    *len);
	*data = NULL;
	*len = 0;
}

/*
 * selftest_list_has — 1 if the keylist_append-encoded [len][bytes]
 * list `list` (of `len` bytes) contains a record equal to want/wlen.
 */
static int
selftest_list_has(const uint8_t *list, mach_msg_type_number_t len,
    const void *want, size_t wlen)
{
	size_t off = 0;

	while (off + sizeof(uint32_t) <= (size_t)len) {
		uint32_t reclen;

		memcpy(&reclen, list + off, sizeof(reclen));
		off += sizeof(reclen);
		if (off + reclen > (size_t)len)
			break;
		if (reclen == wlen && memcmp(list + off, want, wlen) == 0)
			return 1;
		off += reclen;
	}
	return 0;
}

static mach_port_t selftest_pset;

static void *
selftest_server(void *arg)
{
	(void)arg;
	configd_serve(selftest_pset);
	return NULL;
}

static int
run_selftest(void)
{
	pthread_t		th;
	kern_return_t		kr;
	mach_port_t		selftest_port = MACH_PORT_NULL;
	mach_port_t		session = MACH_PORT_NULL;
	mach_port_t		notify_port = MACH_PORT_NULL;
	int			status = -1;
	int			newInstance = 0;
	const char		name[] = "selftest";
	const char		key[] = "selftest:key";
	const char		pat[] = "selftest:pat[0-9]+";
	const char		patkey[] = "selftest:pat42";
	const char		val[] = "configd selftest value blob";
	const char		val2[] = "configd selftest changed value";
	xmlDataOut_t		got = NULL;
	mach_msg_type_number_t	gotCnt = 0;
	union {
		mach_msg_header_t	hdr;
		uint8_t			buf[256];	/* header + trailer */
	} nmsg;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &selftest_port);
	if (kr != KERN_SUCCESS) {
		clog("selftest: mach_port_allocate failed: 0x%x", (unsigned)kr);
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), selftest_port,
	    selftest_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		clog("selftest: insert_right failed: 0x%x", (unsigned)kr);
		return 1;
	}
	if (configd_make_port_set(selftest_port, &selftest_pset) != 0)
		return 1;

	if (pthread_create(&th, NULL, selftest_server, NULL) != 0) {
		clog("selftest: pthread_create failed");
		return 1;
	}
	(void)sleep(1);		/* let the server thread reach mach_msg RCV */

	kr = configopen(selftest_port, name,
	    (mach_msg_type_number_t)(sizeof(name) - 1), NULL, 0,
	    &session, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configopen kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configset(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    val, (mach_msg_type_number_t)(sizeof(val) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configget(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    &got, &gotCnt, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configget kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}
	if (gotCnt != (mach_msg_type_number_t)(sizeof(val) - 1) ||
	    memcmp(got, val, gotCnt) != 0) {
		clog("selftest: value MISMATCH (gotCnt=%u)", (unsigned)gotCnt);
		return 1;
	}
	selftest_ool_free(&got, &gotCnt);

	/*
	 * Change-notification path: watch the key, change it, and
	 * confirm notifychanges reports exactly that key.
	 */
	kr = notifyadd(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    0, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifyadd kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configset(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    val2, (mach_msg_type_number_t)(sizeof(val2) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset(notify) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}

	kr = notifychanges(session, &got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifychanges kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}
	{
		/* The list is one [uint32 len][key bytes] record. */
		uint32_t	klen;
		size_t		want = sizeof(key) - 1;

		if (gotCnt != (mach_msg_type_number_t)(sizeof(klen) + want)) {
			clog("selftest: notifychanges list size %u (want %zu)",
			    (unsigned)gotCnt, sizeof(klen) + want);
			return 1;
		}
		memcpy(&klen, got, sizeof(klen));
		if (klen != want ||
		    memcmp(got + sizeof(klen), key, want) != 0) {
			clog("selftest: notifychanges key MISMATCH");
			return 1;
		}
	}
	selftest_ool_free(&got, &gotCnt);

	/*
	 * Notification-port path: register a Mach notification port,
	 * change the watched key again, and confirm configd messages
	 * the port.
	 */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &notify_port);
	if (kr != KERN_SUCCESS) {
		clog("selftest: notify-port allocate failed: 0x%x",
		    (unsigned)kr);
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), notify_port,
	    notify_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		clog("selftest: notify-port insert_right failed: 0x%x",
		    (unsigned)kr);
		return 1;
	}
	kr = notifyviaport(session, notify_port, 0, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifyviaport kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configset(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    val, (mach_msg_type_number_t)(sizeof(val) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset(viaport) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}

	memset(&nmsg, 0, sizeof(nmsg));
	kr = mach_msg(&nmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(nmsg), notify_port, 5000, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		clog("selftest: no notification message (mach_msg: 0x%x)",
		    (unsigned)kr);
		return 1;
	}
	if (nmsg.hdr.msgh_id != CONFIGD_NOTIFY_MSGID) {
		clog("selftest: notification msgh_id 0x%x (expected 0x%x)",
		    (unsigned)nmsg.hdr.msgh_id, (unsigned)CONFIGD_NOTIFY_MSGID);
		return 1;
	}

	/* Drain the change left pending by the notify-port step. */
	kr = notifychanges(session, &got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifychanges(drain) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}
	selftest_ool_free(&got, &gotCnt);

	/*
	 * Pattern watch: register a POSIX regex, change a key that
	 * matches it, and confirm notifychanges reports that key.
	 */
	kr = notifyadd(session, pat, (mach_msg_type_number_t)(sizeof(pat) - 1),
	    1, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifyadd(regex) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}
	kr = configset(session, patkey,
	    (mach_msg_type_number_t)(sizeof(patkey) - 1), val,
	    (mach_msg_type_number_t)(sizeof(val) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset(regex) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}
	kr = notifychanges(session, &got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifychanges(regex) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}
	{
		/* One [uint32 len][key bytes] record — the matched key. */
		uint32_t	klen2;
		size_t		want2 = sizeof(patkey) - 1;

		if (gotCnt != (mach_msg_type_number_t)(sizeof(klen2) + want2)) {
			clog("selftest: regex change list size %u (want %zu)",
			    (unsigned)gotCnt, sizeof(klen2) + want2);
			return 1;
		}
		memcpy(&klen2, got, sizeof(klen2));
		if (klen2 != want2 ||
		    memcmp(got + sizeof(klen2), patkey, want2) != 0) {
			clog("selftest: regex change key MISMATCH");
			return 1;
		}
	}
	selftest_ool_free(&got, &gotCnt);

	/*
	 * configlist: list every store key and confirm both keys this
	 * selftest stored (the explicit key and the pattern-matched key)
	 * appear in the result.
	 */
	kr = configlist(session, NULL, 0, 0, &got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configlist kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}
	if (!selftest_list_has((const uint8_t *)got, gotCnt, key,
	    sizeof(key) - 1) ||
	    !selftest_list_has((const uint8_t *)got, gotCnt, patkey,
	    sizeof(patkey) - 1)) {
		clog("selftest: configlist is missing a stored key");
		return 1;
	}
	selftest_ool_free(&got, &gotCnt);

	/*
	 * configset_m / configget_m: set a key through a batch update,
	 * fetch it back through a batch query, and confirm the value
	 * round-trips through the wire_kvmap encoding.
	 */
	{
		const char	mkey[] = "selftest:multi";
		const char	mval[] = "configd selftest multi value";
		char		reqbuf[256];
		size_t		reqoff;
		const uint8_t	*cur, *end;
		const void	*gk, *gv;
		size_t		gkl, gvl;
		int		found = 0;

		reqoff = 0;
		if (wire_kvmap_put((uint8_t *)reqbuf, sizeof(reqbuf), &reqoff, mkey,
		    sizeof(mkey) - 1, mval, sizeof(mval) - 1) != 0) {
			clog("selftest: configset_m payload build failed");
			return 1;
		}
		kr = configset_m(session, reqbuf, (mach_msg_type_number_t)reqoff,
		    NULL, 0, NULL, 0, &status);
		if (kr != KERN_SUCCESS || status != kSCStatusOK) {
			clog("selftest: configset_m kr=0x%x status=%d",
			    (unsigned)kr, status);
			return 1;
		}

		reqoff = 0;
		if (wire_keylist_put((uint8_t *)reqbuf, sizeof(reqbuf), &reqoff, mkey,
		    sizeof(mkey) - 1) != 0) {
			clog("selftest: configget_m payload build failed");
			return 1;
		}
		kr = configget_m(session, reqbuf,
		    (mach_msg_type_number_t)reqoff, NULL, 0, &got, &gotCnt,
		    &status);
		if (kr != KERN_SUCCESS || status != kSCStatusOK) {
			clog("selftest: configget_m kr=0x%x status=%d",
			    (unsigned)kr, status);
			return 1;
		}

		cur = (const uint8_t *)got;
		end = cur + gotCnt;
		while (wire_kvmap_next(&cur, end, &gk, &gkl, &gv, &gvl)) {
			if (gkl != sizeof(mkey) - 1 ||
			    memcmp(gk, mkey, gkl) != 0)
				continue;
			found = 1;
			if (gvl != sizeof(mval) - 1 ||
			    memcmp(gv, mval, gvl) != 0) {
				clog("selftest: configget_m value MISMATCH");
				return 1;
			}
		}
		if (!found) {
			clog("selftest: configget_m did not return the key");
			return 1;
		}
		selftest_ool_free(&got, &gotCnt);
	}

	clog("CONFIGD-SELFTEST-OK: in-process config.defs round-trip + "
	    "change notification + notify port + regex watch + list + "
	    "multi works");
	return 0;
}

int
main(int argc, char **argv)
{
	mach_port_t	service_port = MACH_PORT_NULL;
	mach_port_t	port_set = MACH_PORT_NULL;
	kern_return_t	kr;
	const char	*service_name = SCD_SERVER;

	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGINT, on_signal);

	if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
		return run_selftest();

	/*
	 * Check the SCDynamicStore service in with launchd. configd is a
	 * launchd job whose plist declares this MachService, so launchd
	 * created the port and hands us the receive right here.
	 */
	kr = bootstrap_check_in(bootstrap_port, service_name, &service_port);
	if (kr != KERN_SUCCESS) {	/* BOOTSTRAP_SUCCESS == KERN_SUCCESS == 0 */
		clog("bootstrap_check_in(%s) failed: 0x%x — exiting",
		    service_name, (unsigned)kr);
		return 1;
	}
	clog("Mach service '%s' checked in (port=0x%x)",
	    service_name, (unsigned)service_port);

	/*
	 * configd serves the bootstrap service port and every
	 * per-session port from one receive loop: a port set holds them
	 * all, and configopen joins new session ports to it.
	 */
	if (configd_make_port_set(service_port, &port_set) != 0) {
		clog("could not set up the configd port set — exiting");
		return 1;
	}

	/*
	 * Start the in-process KernelEventMonitor: a thread that reads the
	 * PF_ROUTE socket and publishes State:/Network/Interface/<if>/Link via
	 * a configd session (so the store write lands on this serve thread).
	 * Replaces the standalone KernelEventMonitor daemon (#257). The service
	 * is checked in above, so the thread's configopen resolves; it must
	 * start before configd_serve() blocks here.
	 */
	config_link_monitor_start();

	configd_serve(port_set);

	clog("shutting down (signal %d)", (int)got_term);
	return 0;
}
