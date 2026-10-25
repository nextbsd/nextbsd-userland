/*
 * IOKitNotify.c — libIOKit: K2 device-arrival / departure notifications.
 *
 * Each IONotificationPort owns one Mach receive right and a
 * DISPATCH_SOURCE_TYPE_MACH_RECV source on it (running on a private serial
 * queue). Events fan in on the one port; the source handler self-drains the
 * port and re-matches each event's device fields against the per-watch criteria
 * stored client-side, then fires each matching IOServiceMatchingCallback (via
 * dispatch_async_f if a callback queue is set, inline otherwise).
 *
 * Backend (#218): watch registration uses the kernel notify channel —
 * ioctl(/dev/ioregistry, IOREGIOCWATCH) registers the recv port directly with
 * the in-kernel registry (#225), and the kernel pushes a binary ioreg_event_msg
 * (msgid IOKIT_NOTIFY_EVENT_MSGID) from its device_attach/device_detach
 * eventhandlers. The kernel is the registry; hwregd has been retired, so there
 * is no userland fallback.
 *
 * Interest notifications (#456, kernel nextbsd-kernel#338) share the port:
 * IOServiceAddInterestNotification registers the same recv right on ONE
 * registry entry with ioctl(IOREGIOCINTEREST), and the kernel pushes an
 * ioreg_interest_msg (msgid IOKIT_NOTIFY_INTEREST_MSGID) carrying a kIOMessage*
 * code, the entry id, two arguments and a devnode. The handler dispatches it to
 * the port's interest registrations for that id.
 *
 * Delivery is the native EVFILT_MACHPORT filter (mach.ko filt_machport, slot
 * -16) that libmach reroutes the source's kevent onto. task #41's "MACH_RECV
 * doesn't reliably deliver" was the module-era register_event_bell pipe bridge,
 * retired in #168 Stages 2-3 (#250/#254); SCNotify converted alongside this in
 * Stage 4 (#255).
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include "IOKitInternal.h"

#include "ioregistry.h"		/* vendored: IOREGIOCWATCH, ioreg_watch_reg, IOREG_EVENT_* */
#include "iokit_notify.h"	/* vendored: ioreg_event_msg + msgid */

#include <dispatch/dispatch.h>
#include <mach/mach.h>

#include <sys/ioctl.h>		/* ioctl(2) on /dev/ioregistry */

#include <errno.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One watch carried as an IOServiceAddMatchingNotification. The criteria
 * fields are kept client-side so that when multiple watches share one
 * IONotificationPort, the receive thread can re-match each event against them
 * and fire the right callback(s). There is no per-watch id — the kernel retires
 * the watch when the recv right is dropped.
 *
 * `event_mask` uses the IOREG_EVENT_* bits (value-identical to the
 * IOKIT_NOTIFY_KIND_* arrive/depart ones, 0x1 / 0x2), so the same mask drives
 * registration and the re-match in event_matches_watch.
 */
struct __io_watch {
	struct __io_watch		*next;
	uint32_t			 event_mask;	/* IOREG_EVENT_* */
	struct io_criteria		 criteria;
	IOServiceMatchingCallback	 callback;
	void				*refcon;
};

/*
 * One IOServiceAddInterestNotification registration. Linked on its port's
 * `interests` list while active; owned by the io_object_t handed to the caller
 * (IOOBJ_KIND_INTEREST), so it is freed only by IOObjectRelease. Unlinked
 * (linked = false) by that release, by kIOMessageServiceIsTerminated, or by
 * port destruction. Holds a reference on the port and on `service`.
 */
struct __io_interest {
	struct __io_interest		*next;
	struct IONotificationPort	*port;
	uint64_t			 id;		/* service registry id */
	io_service_t			 service;	/* retained */
	IOServiceInterestCallback	 callback;
	void				*refcon;
	bool				 linked;
};

/*
 * Registry ids this port holds a kernel interest watch on. The kernel has no
 * per-watch unregister (the watch ends when the port dies or the entry
 * departs), so a second registration for the same id on the same port reuses
 * the existing watch instead of adding one; otherwise every message would
 * arrive twice and fire each registration twice.
 */
struct __io_kinterest {
	struct __io_kinterest	*next;
	uint64_t		 id;
};

struct IONotificationPort {
	atomic_int		 refcnt;	/* 1 for the port itself, plus one
						 * per interest registration */
	struct __io_interest	*interests;	/* guarded by `lock` */
	struct __io_kinterest	*kinterests;	/* guarded by `lock` */
	mach_port_t		 recv_port;	/* receive right we own */
	dispatch_source_t	 source;	/* MACH_RECV source on recv_port */
	dispatch_queue_t	 recv_queue;	/* private serial queue the source
						 * runs on (distinct from `queue`,
						 * the callback target) */
	pthread_mutex_t		 lock;		/* guards `watches` + queue */
	struct __io_watch	*watches;
	dispatch_queue_t	 queue;		/* optional callback target */
};

/*
 * Bundle passed to dispatch_async_f so the receive thread can hand
 * the callback off to a user dispatch queue. The bundle owns the
 * io_iterator_t (allocated by us, released by IOObjectRelease in
 * the dispatch trampoline after the user callback returns).
 */
struct callout_bundle {
	IOServiceMatchingCallback	 callback;
	void				*refcon;
	io_iterator_t			 iterator;
};

static void
callout_trampoline(void *ctx)
{
	struct callout_bundle *b = ctx;

	b->callback(b->refcon, b->iterator);
	IOObjectRelease(b->iterator);
	free(b);
}

static void
fire_callback(struct IONotificationPort *port, struct __io_watch *w,
    uint64_t node_id)
{
	uint64_t *ids = calloc(1, sizeof(*ids));
	io_iterator_t it;
	dispatch_queue_t q;

	if (ids == NULL)
		return;
	ids[0] = node_id;
	it = __io_alloc_iterator(ids, 1);	/* takes ownership of ids */
	if (it == IO_OBJECT_NULL)
		return;

	pthread_mutex_lock(&port->lock);
	q = port->queue;
	pthread_mutex_unlock(&port->lock);

	if (q != NULL) {
		struct callout_bundle *b = calloc(1, sizeof(*b));

		if (b == NULL) {
			IOObjectRelease(it);
			return;
		}
		b->callback = w->callback;
		b->refcon = w->refcon;
		b->iterator = it;
		dispatch_async_f(q, b, callout_trampoline);
	} else {
		/* No queue — run on the receive thread directly. Acceptable
		 * for simple consumers (the iter-4 iokitnotifytest is one);
		 * real consumers usually SetDispatchQueue. */
		w->callback(w->refcon, it);
		IOObjectRelease(it);
	}
}

/*
 * Does this watch's criteria match the device described by the
 * event? Empty criteria fields are wildcards (Apple's
 * dictionary-omit semantics).
 */
static bool
event_matches_watch(struct __io_watch *w, uint32_t evt_bit,
    const char *name, const char *klass)
{
	if ((w->event_mask & evt_bit) == 0)
		return (false);
	if (w->criteria.name[0] != '\0' && strcmp(w->criteria.name, name) != 0)
		return (false);
	if (w->criteria.klass[0] != '\0' &&
	    strcmp(w->criteria.klass, klass) != 0)
		return (false);
	/* `driver` is not in the event payload; a watch with a driver-only
	 * criterion still gets the event (treated as a wildcard here). */
	return (true);
}

/*
 * One mach_msg receive buffer big enough for the kernel notify channel's binary
 * event (ioreg_event_msg, msgid IOKIT_NOTIFY_EVENT_MSGID) plus the largest
 * trailer the kernel might append.
 */
union event_rcv_buf {
	mach_msg_header_t	hdr;
	struct {
		ioreg_event_msg		msg;
		mach_msg_max_trailer_t	trailer;
	} k;
	struct {
		ioreg_interest_msg	msg;
		mach_msg_max_trailer_t	trailer;
	} i;
};

/*
 * Drop one reference on the port; the last one frees the struct and its lock.
 * The rights, queues and watch list go in io_source_cancel; only the memory an
 * outstanding interest registration can still reach lives until here.
 */
static void
port_unref(struct IONotificationPort *port)
{
	if (atomic_fetch_sub(&port->refcnt, 1) != 1)
		return;
	(void)pthread_mutex_destroy(&port->lock);
	free(port);
}

static void
interest_unlink_locked(struct IONotificationPort *port,
    struct __io_interest *rec)
{
	struct __io_interest **pp;

	if (!rec->linked)
		return;
	for (pp = &port->interests; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == rec) {
			*pp = rec->next;
			break;
		}
	}
	rec->next = NULL;
	rec->linked = false;
}

static void
kinterest_forget_locked(struct IONotificationPort *port, uint64_t id)
{
	struct __io_kinterest **pp, *k;

	for (pp = &port->kinterests; (k = *pp) != NULL; pp = &k->next) {
		if (k->id == id) {
			*pp = k->next;
			free(k);
			return;
		}
	}
}

static void
kinterests_free_locked(struct IONotificationPort *port)
{
	struct __io_kinterest *k, *next;

	for (k = port->kinterests; k != NULL; k = next) {
		next = k->next;
		free(k);
	}
	port->kinterests = NULL;
}

/*
 * One interest callout. The callback, refcon and a retained service are copied
 * out of the registration under the port lock, so the callout does not touch
 * the registration (which a concurrent IOObjectRelease may free). `arg` is the
 * messageArgument the callback sees; it lives as long as the callout.
 */
struct interest_callout {
	IOServiceInterestCallback		 callback;
	void					*refcon;
	io_service_t				 service;	/* retained */
	uint32_t				 message;
	IOServiceInterestMessageArgument	 arg;
};

static void
interest_trampoline(void *ctx)
{
	struct interest_callout *c = ctx;

	c->callback(c->refcon, c->service, c->message, &c->arg);
	IOObjectRelease(c->service);
	free(c);
}

/*
 * Handle one ioreg_interest_msg: fire every active registration on this port
 * for the message's entry id, on the callback queue if one is set and inline
 * otherwise (the fire_callback rule). kIOMessageServiceIsTerminated is the
 * last message for the entry: the kernel drops its watch after sending it, so
 * the registrations are unlinked and the port forgets its kernel watch.
 */
#define INTEREST_SNAP_MAX	16

static void
handle_interest(struct IONotificationPort *port, const union event_rcv_buf *buf)
{
	const ioreg_interest_msg *m = &buf->i.msg;
	struct interest_callout *snap[INTEREST_SNAP_MAX];
	IOServiceInterestMessageArgument arg;
	struct __io_interest *rec, *next;
	dispatch_queue_t q;
	bool terminated;
	int n_snap = 0, i;

	if (buf->hdr.msgh_size < sizeof(*m) - sizeof(m->trailer))
		return;		/* malformed: drop */

	memset(&arg, 0, sizeof(arg));
	arg.registryEntryID = m->id;
	arg.argument[0] = m->argument[0];
	arg.argument[1] = m->argument[1];
	/* Untrusted wire data: copy and force-terminate. */
	_Static_assert(sizeof(arg.devnode) == sizeof(m->devnode),
	    "IOServiceInterestMessageArgument.devnode != wire devnode");
	memcpy(arg.devnode, m->devnode, sizeof(arg.devnode));
	arg.devnode[sizeof(arg.devnode) - 1] = '\0';
	terminated = (m->message == kIOMessageServiceIsTerminated);

	pthread_mutex_lock(&port->lock);
	q = port->queue;
	if (q != NULL)
		dispatch_retain(q);
	for (rec = port->interests; rec != NULL; rec = next) {
		next = rec->next;
		if (rec->id != m->id)
			continue;
		if (n_snap < INTEREST_SNAP_MAX) {
			struct interest_callout *c = calloc(1, sizeof(*c));

			if (c != NULL) {
				c->callback = rec->callback;
				c->refcon = rec->refcon;
				c->service = rec->service;
				(void)IOObjectRetain(c->service);
				c->message = m->message;
				c->arg = arg;
				snap[n_snap++] = c;
			}
		}
		if (terminated)
			interest_unlink_locked(port, rec);
	}
	if (terminated)
		kinterest_forget_locked(port, m->id);
	pthread_mutex_unlock(&port->lock);

	for (i = 0; i < n_snap; i++) {
		if (q != NULL)
			dispatch_async_f(q, snap[i], interest_trampoline);
		else
			interest_trampoline(snap[i]);
	}
	if (q != NULL)
		dispatch_release(q);
}

/*
 * Decode one received message into (evt, id, name, class). Returns true if the
 * message was a recognized kernel notify event and was decoded; false for an
 * unknown msgid or a malformed body (caller drops it). `evt` is the
 * IOREG_EVENT_* bit (arrive/depart) used by event_matches_watch.
 */
static bool
decode_event(const union event_rcv_buf *buf, uint32_t *evt, uint64_t *id,
    char *name, size_t name_sz, char *klass, size_t klass_sz)
{
	switch (buf->hdr.msgh_id) {
	case IOKIT_NOTIFY_EVENT_MSGID: {
		const ioreg_event_msg *m = &buf->k.msg;
		char tmp_name[IOKIT_NOTIFY_NAME_MAX];
		char tmp_klass[IOKIT_NOTIFY_NAME_MAX];

		/* The kernel re-bounds these, but treat them as untrusted wire
		 * data: copy into a local, force-terminate, then hand out. */
		memcpy(tmp_name, m->name, sizeof(tmp_name));
		memcpy(tmp_klass, m->classname, sizeof(tmp_klass));
		tmp_name[sizeof(tmp_name) - 1] = '\0';
		tmp_klass[sizeof(tmp_klass) - 1] = '\0';

		*id = m->id;
		if (name != NULL && name_sz > 0) {
			strncpy(name, tmp_name, name_sz - 1);
			name[name_sz - 1] = '\0';
		}
		if (klass != NULL && klass_sz > 0) {
			strncpy(klass, tmp_klass, klass_sz - 1);
			klass[klass_sz - 1] = '\0';
		}
		/* DEPART maps to departed; ARRIVE and MATCHED both map to
		 * arrived (the client-side mask only carries arrive/depart). */
		*evt = (m->kind == IOKIT_NOTIFY_KIND_DEPART) ? IOREG_EVENT_DEPART
		    : IOREG_EVENT_ARRIVE;
		return (true);
	}
	default:
		return (false);
	}
}

/*
 * MACH_RECV source event handler. The kernel notify channel pushes binary
 * ioreg_event_msg events to recv_port; the native EVFILT_MACHPORT source wakes
 * us here (the task-#41 "doesn't reliably deliver" was the retired module-era
 * pipe bridge — native delivery works, #168/#250). The source registers in
 * notify mode and carries no message, so drain recv_port ourselves in a loop:
 * several events can coalesce into one fire, and a left-over message would
 * re-fire the source forever. Runs on the private recv_queue.
 */
static void
io_source_handler(void *arg)
{
	struct IONotificationPort *port = arg;

	for (;;) {
		union event_rcv_buf buf;
		mach_msg_return_t mr;
		uint64_t id;
		uint32_t evt;
		char name[32], klass[32];
		struct __io_watch *w, *snap[16];
		int n_snap = 0, i;

		mr = mach_msg(&buf.hdr,
		    MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(buf), port->recv_port,
		    0 /* non-blocking */, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS)
			break;		/* drained (RCV_TIMED_OUT) or transient */
		if (buf.hdr.msgh_id == IOKIT_NOTIFY_INTEREST_MSGID) {
			handle_interest(port, &buf);
			continue;
		}
		if (!decode_event(&buf, &evt, &id, name, sizeof(name),
		    klass, sizeof(klass)))
			continue;

		/* Snapshot the matching watches under the lock into a
		 * small array, then fire outside the lock so a user
		 * callback can safely add or remove notifications. The
		 * 16-entry cap is plenty: one IONotificationPort with more
		 * than 16 simultaneous
		 * matching watches on a single event is not a real
		 * shape. */
		pthread_mutex_lock(&port->lock);
		for (w = port->watches; w != NULL && n_snap < 16;
		    w = w->next)
			if (event_matches_watch(w, evt, name, klass))
				snap[n_snap++] = w;
		pthread_mutex_unlock(&port->lock);

		for (i = 0; i < n_snap; i++)
			fire_callback(port, snap[i], id);
	}
}

/*
 * MACH_RECV source cancel handler. Runs asynchronously after cancellation and
 * any in-flight event handler complete, so recv_port and the lock stay valid
 * until the source is truly done. Frees the watch list, drops the port's send +
 * receive rights (the kernel prunes this port's watches on its next emission),
 * releases the callback + receive queues, deactivates any interest
 * registrations still outstanding, and drops the port's own reference (the
 * lock and struct go when the last registration is released too).
 */
static void
io_source_cancel(void *arg)
{
	struct IONotificationPort *port = arg;
	struct __io_watch *w, *next;
	dispatch_queue_t q;

	for (w = port->watches; w != NULL; w = next) {
		next = w->next;
		free(w);
	}
	pthread_mutex_lock(&port->lock);
	while (port->interests != NULL)
		interest_unlink_locked(port, port->interests);
	kinterests_free_locked(port);
	q = port->queue;
	port->queue = NULL;
	pthread_mutex_unlock(&port->lock);
	(void)mach_port_mod_refs(mach_task_self(), port->recv_port,
	    MACH_PORT_RIGHT_SEND, -1);
	(void)mach_port_mod_refs(mach_task_self(), port->recv_port,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	if (q != NULL)
		dispatch_release(q);
	if (port->recv_queue != NULL)
		dispatch_release(port->recv_queue);
	port_unref(port);
}

IONotificationPortRef
IONotificationPortCreate(mach_port_t mainPort __unused)
{
	struct IONotificationPort *p;
	mach_port_t mp = MACH_PORT_NULL;

	if (mach_port_allocate(mach_task_self(),
	    MACH_PORT_RIGHT_RECEIVE, &mp) != KERN_SUCCESS)
		return (NULL);

	/*
	 * Insert a send right into our own name space for `mp` (MAKE_SEND).
	 * The kernel notify channel resolves this port by *name* and needs that
	 * name to carry a send right: IOREGIOCWATCH passes the name to
	 * iokit_notify_copyin_port, which does ipc_object_copyin(...,
	 * MACH_MSG_TYPE_COPY_SEND, ...) on the calling task's IPC space. COPY_SEND
	 * requires the name to already hold a send right (ipc_right_copyin rejects
	 * a name with no MACH_PORT_TYPE_SEND_RIGHTS as KERN_INVALID_RIGHT); a bare
	 * receive-right name fails, the watch never registers, and no event is ever
	 * delivered. (allocate RECEIVE, then insert MAKE_SEND.)
	 */
	if (mach_port_insert_right(mach_task_self(), mp, mp,
	    MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		return (NULL);
	}

	p = calloc(1, sizeof(*p));
	if (p == NULL) {
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_SEND, -1);
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		return (NULL);
	}
	p->recv_port = mp;
	atomic_store(&p->refcnt, 1);
	(void)pthread_mutex_init(&p->lock, NULL);

	/*
	 * libIOKit has no caller queue at create time (IONotificationPortSet-
	 * DispatchQueue sets the *callback* target, separately), so drive the
	 * MACH_RECV source from a private serial queue.
	 */
	p->recv_queue = dispatch_queue_create("com.apple.iokit.notify", NULL);
	if (p->recv_queue != NULL)
		p->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,
		    mp, 0, p->recv_queue);
	if (p->recv_queue == NULL || p->source == NULL) {
		if (p->recv_queue != NULL)
			dispatch_release(p->recv_queue);
		(void)pthread_mutex_destroy(&p->lock);
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_SEND, -1);
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		free(p);
		return (NULL);
	}
	dispatch_set_context(p->source, p);
	dispatch_source_set_event_handler_f(p->source, io_source_handler);
	dispatch_source_set_cancel_handler_f(p->source, io_source_cancel);
	dispatch_resume(p->source);
	return (p);
}

void
IONotificationPortSetDispatchQueue(IONotificationPortRef port,
    dispatch_queue_t queue)
{
	dispatch_queue_t old;

	if (port == NULL)
		return;
	pthread_mutex_lock(&port->lock);
	old = port->queue;
	port->queue = queue;
	if (queue != NULL)
		dispatch_retain(queue);
	pthread_mutex_unlock(&port->lock);
	if (old != NULL)
		dispatch_release(old);
}

mach_port_t
IONotificationPortGetMachPort(IONotificationPortRef port)
{
	return (port != NULL ? port->recv_port : MACH_PORT_NULL);
}

void
IONotificationPortDestroy(IONotificationPortRef port)
{
	if (port == NULL)
		return;

	/*
	 * Cancel the MACH_RECV source. The cancel handler (io_source_cancel,
	 * async, after any in-flight event handler) frees the watch list, drops
	 * the port's send + receive rights — which signals the kernel notify
	 * channel to prune this port's watches on its next emission
	 * (iokit_notify_port_dead) — releases the callback + receive queues,
	 * destroys the lock, and frees the port. Doing teardown there (not here)
	 * keeps recv_port and the lock valid until the source is truly done, so a
	 * concurrent in-flight event can't use-after-free. We drop only our own
	 * source ref here.
	 */
	dispatch_source_cancel(port->source);
	dispatch_release(port->source);
}

/*
 * Register the watch on the kernel notify channel: ioctl(/dev/ioregistry,
 * IOREGIOCWATCH) with this port's recv-right name, the flat by-value criteria
 * struct (#218; same fixed struct IOREGIOCLOOKUP uses) and the IOREG_EVENT_*
 * mask. The kernel copies a send right to the port and emits ioreg_event_msg on
 * each matching event. Returns KERN_SUCCESS on success, a kIOReturn* error
 * otherwise. No nvlist packing is involved, so the libxpc-vs-libnv mismatch
 * that broke the #218 round-trip cannot recur.
 */
static kern_return_t
kernel_watch_register(struct IONotificationPort *port,
    const struct io_criteria *c, uint32_t mask)
{
	struct ioreg_watch_reg reg;
	int fd = __io_ioregistry_fd();

	if (fd < 0)
		return (kIOReturnNoDevice);

	memset(&reg, 0, sizeof(reg));
	__io_fill_criteria(c, &reg.criteria);
	reg.event_mask = mask;	/* OR of IOREG_EVENT_* */
	reg.notify_port = (uint32_t)port->recv_port;	/* recv right name */

	if (ioctl(fd, IOREGIOCWATCH, &reg) != 0)
		return (kIOReturnError);
	return (KERN_SUCCESS);
}

kern_return_t
IOServiceAddMatchingNotification(IONotificationPortRef port,
    const io_name_t notification_type, CFDictionaryRef matching,
    IOServiceMatchingCallback callback, void *refcon,
    io_iterator_t *out_iterator)
{
	struct __io_watch *w;
	struct io_criteria c;
	uint32_t mask;
	kern_return_t kr;

	if (port == NULL || notification_type == NULL || matching == NULL ||
	    callback == NULL || out_iterator == NULL) {
		if (matching != NULL)
			CFRelease(matching);
		return (KERN_INVALID_ARGUMENT);
	}

	/* Notification type → event mask. The three "device arrived" flavours
	 * map to IOREG_EVENT_ARRIVE (no internal Publish/FirstMatch/Matched
	 * distinction in the kernel channel). */
	if (strcmp(notification_type, kIOTerminatedNotification) == 0)
		mask = IOREG_EVENT_DEPART;
	else
		mask = IOREG_EVENT_ARRIVE;

	__io_extract_criteria(matching, &c);

	/* Kernel notify channel: the criteria travel as a flat by-value struct
	 * (#218), so there is no nvlist packing and nothing to mismatch —
	 * kernel_watch_register fills it from `c` directly. The kernel has no
	 * per-watch handle and retires the watch when the recv right is dropped. */
	kr = kernel_watch_register(port, &c, mask);
	if (kr != KERN_SUCCESS) {
		CFRelease(matching);
		return (kr);
	}

	w = calloc(1, sizeof(*w));
	if (w == NULL) {
		CFRelease(matching);
		return (KERN_RESOURCE_SHORTAGE);
	}
	w->event_mask = mask;
	w->criteria = c;
	w->callback = callback;
	w->refcon = refcon;

	pthread_mutex_lock(&port->lock);
	w->next = port->watches;
	port->watches = w;
	pthread_mutex_unlock(&port->lock);

	/* Initial arming: hand the caller an iterator over the
	 * currently-matching services. IOServiceGetMatchingServices
	 * consumes the matching ref (the contract this routine
	 * also owes the caller). */
	return (IOServiceGetMatchingServices(kIOMainPortDefault,
	    matching, out_iterator));
}

/*
 * Register an interest watch for registry entry `id` on this port, unless the
 * port already holds one (see struct __io_kinterest). Called with the port
 * lock held so two concurrent registrations for one id add one kernel watch.
 */
static kern_return_t
kernel_interest_register_locked(struct IONotificationPort *port, uint64_t id)
{
	struct ioreg_interest_reg reg;
	struct __io_kinterest *k;
	int fd = __io_ioregistry_fd();

	for (k = port->kinterests; k != NULL; k = k->next)
		if (k->id == id)
			return (KERN_SUCCESS);
	if (fd < 0)
		return (kIOReturnNoDevice);
	k = calloc(1, sizeof(*k));
	if (k == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	memset(&reg, 0, sizeof(reg));
	reg.id = id;
	reg.interest = IOREG_INTEREST_GENERAL;
	reg.notify_port = (uint32_t)port->recv_port;	/* recv right name */
	if (ioctl(fd, IOREGIOCINTEREST, &reg) != 0) {
		int e = errno;

		free(k);
		switch (e) {
		case ENOENT:
			return (kIOReturnNotFound);	/* departed / unknown */
		case ENOSYS:
		case ENOTTY:	/* kernel predating IOREGIOCINTEREST */
			return (kIOReturnUnsupported);
		case EINVAL:
			return (kIOReturnBadArgument);
		default:
			return (kIOReturnError);
		}
	}
	k->id = id;
	k->next = port->kinterests;
	port->kinterests = k;
	return (KERN_SUCCESS);
}

kern_return_t
IOServiceAddInterestNotification(IONotificationPortRef port,
    io_service_t service, const io_name_t interestType,
    IOServiceInterestCallback callback, void *refcon,
    io_object_t *notification)
{
	struct __io_interest *rec;
	io_object_t obj;
	kern_return_t kr;

	if (port == NULL || service == NULL ||
	    service->kind != IOOBJ_KIND_ENTRY || interestType == NULL ||
	    callback == NULL || notification == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (strcmp(interestType, kIOGeneralInterest) != 0)
		return (kIOReturnUnsupported);

	rec = calloc(1, sizeof(*rec));
	obj = calloc(1, sizeof(*obj));
	if (rec == NULL || obj == NULL) {
		free(rec);
		free(obj);
		return (KERN_RESOURCE_SHORTAGE);
	}
	rec->port = port;
	rec->id = service->node_id;
	rec->service = service;
	rec->callback = callback;
	rec->refcon = refcon;

	pthread_mutex_lock(&port->lock);
	kr = kernel_interest_register_locked(port, rec->id);
	if (kr != KERN_SUCCESS) {
		pthread_mutex_unlock(&port->lock);
		free(rec);
		free(obj);
		return (kr);
	}
	(void)IOObjectRetain(service);
	(void)atomic_fetch_add(&port->refcnt, 1);
	rec->linked = true;
	rec->next = port->interests;
	port->interests = rec;
	pthread_mutex_unlock(&port->lock);

	obj->kind = IOOBJ_KIND_INTEREST;
	atomic_store(&obj->refcnt, 1);
	obj->node_id = rec->id;
	obj->interest = rec;
	*notification = obj;
	return (KERN_SUCCESS);
}

void
__io_interest_release(struct __io_interest *rec)
{
	struct IONotificationPort *port;

	if (rec == NULL)
		return;
	port = rec->port;
	pthread_mutex_lock(&port->lock);
	interest_unlink_locked(port, rec);
	pthread_mutex_unlock(&port->lock);
	IOObjectRelease(rec->service);
	free(rec);
	port_unref(port);
}
