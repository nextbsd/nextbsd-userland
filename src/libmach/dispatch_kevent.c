/*
 * dispatch_kevent.c — libmach kevent_qos() + EVFILT_MACHPORT bridge.
 *
 * See <mach/dispatch_kevent.h> for the architecture rationale. Short
 * version: libmach provides a kevent_qos()-style entry point + a
 * widened kevent struct (kevent_qos_s with ext[4]) so libdispatch's
 * HAVE_MACH paths compile against Apple-shape APIs. The wrapper
 * translates kevent_qos_s ↔ FreeBSD's struct kevent at the
 * __sys_kevent boundary, intercepting EVFILT_MACHPORT registrations
 * and routing them to mach.ko's NATIVE EVFILT_MACHPORT kqueue filter
 * (slot -16) on a wrap port set. (Previously this used the
 * register_event_bell self-pipe bridge, task #39 Path B; the native
 * filter — proven in nextbsd#249 — delivers reliably, fixing the
 * task-#41 dropped-notification flakiness, and is the Apple-shape path.)
 * The filter is registered in notify mode (fflags = 0): filt_machport
 * signals readiness without consuming, and the wrapper drains the pset
 * via mach_msg into the per-registration backlog, exactly as before.
 * A plain MACH_RECV source's knote also carries the caller's EV_DISPATCH,
 * so it fires once per arm as on Darwin (see machport_native_flags()).
 *
 * Concurrency. A single global mutex protects the registration list.
 * The list is expected to stay small (a handful of dispatch_mach_t
 * channels + per-RPC reply ports), so linear search is fine.
 *
 * Limits in this first cut:
 *   - data_out / data_available (kevent_qos's data-buffer feature
 *     used by workloops) are accepted and ignored. No HAVE_MACH-
 *     non-workloop path in libdispatch exercises them.
 *   - ke->qos and ke->ext[2] (priority) are zeroed on synthesized
 *     events. Mach msg priority would come from msgh_voucher/trailer
 *     parsing — deferred to a follow-up.
 *   - kevent_qos's `flags` arg: IMMEDIATE forces zero timeout
 *     downward; WORKQ is ignored; ERROR_EVENTS isn't honored
 *     (errors surface via errno on the function return). libdispatch
 *     only passes these on submit-only calls where the difference
 *     doesn't matter.
 *
 * Buffer ownership. mach_msg() is called with MACH_RCV_LARGE; on
 * MACH_RCV_TOO_LARGE we re-malloc to the reported size and retry.
 * The synthesized event always carries DISPATCH_EV_MSG_NEEDS_FREE
 * (0x10000) in flags so _dispatch_kevent_mach_msg_drain() free()s
 * the buffer after consuming it.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/dispatch_kevent.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/port.h>

/*
 * DISPATCH_EV_MSG_NEEDS_FREE — defined inside libdispatch
 * (event_config.h:205). Replicated here so the synth eventlist we
 * hand back tells libdispatch to free() the malloc'd buffer.
 */
#define DISPATCH_EV_MSG_NEEDS_FREE 0x10000

/*
 * MACH_RCV_LARGE / MACH_RCV_TIMEOUT / MACH_RCV_MSG — Apple-canonical
 * mach_msg option bits. Re-declared so this TU doesn't have to pull
 * in libdispatch's internal event_config.h.
 */
#ifndef MACH_RCV_TIMEOUT
#define MACH_RCV_TIMEOUT 0x00000100
#endif
#ifndef MACH_RCV_LARGE
#define MACH_RCV_LARGE 0x00000004
#endif
#ifndef MACH_RCV_TRAILER_CTX
#define MACH_RCV_TRAILER_CTX 4
#endif
#ifndef MACH_RCV_TRAILER_ELEMENTS
#define MACH_RCV_TRAILER_ELEMENTS(x) (((x) & 0xf) << 24)
#endif
#ifndef MACH_RCV_TRAILER_TYPE
#define MACH_RCV_TRAILER_TYPE(x) (((x) & 0xf) << 28)
#endif
#ifndef MACH_MSG_TRAILER_FORMAT_0
#define MACH_MSG_TRAILER_FORMAT_0 0
#endif
#ifndef MAX_TRAILER_SIZE
#define MAX_TRAILER_SIZE ((mach_msg_size_t)sizeof(mach_msg_max_trailer_t))
#endif
#ifndef MACH_RCV_MSG
#define MACH_RCV_MSG 0x00000002
#endif
#ifndef MACH_PORT_RIGHT_PORT_SET
#define MACH_PORT_RIGHT_PORT_SET 3
#endif

#define MACH_RCV_INITIAL_BUFSIZE 8192

/*
 * The kernel's NATIVE EVFILT_MACHPORT filter number (nextbsd-kernel patch
 * 0003; registered by mach.ko's kqueue_add_filteropts). DISTINCT from the
 * libmach API sentinel EVFILT_MACHPORT (-22, from <mach/dispatch_kevent.h>)
 * that libdispatch/callers use — we translate the sentinel to this real
 * filter when submitting to __sys_kevent. Proven to deliver (nextbsd#249).
 *
 * The reroute (was: register_event_bell pipe-bridge, task #39 Path B):
 * register this filter on the wrap pset in *notify* mode (fflags = 0, no
 * MACH_RCV_MSG) so filt_machport signals readiness WITHOUT consuming the
 * message; the wrapper then drains the pset via mach_msg into the backlog
 * exactly as before. Only the wakeup transport changed — the drain/backlog/
 * synth path to libdispatch is unchanged.
 */
#define EVFILT_MACHPORT_NATIVE	(-16)

/*
 * Flags that may appear on a *delivered* EVFILT_MACHPORT event. The
 * registration flags libdispatch passes (EV_ADD|EV_ENABLE|EV_DISPATCH|
 * EV_UDATA_SPECIFIC|EV_VANISHED = 0x385) include lifecycle/action
 * bits that must NOT be echoed on delivery — most critically
 * EV_VANISHED, which _dispatch_kevent_merge_ev_flags treats as
 * "the port died" and uses to mark the source DU_STATE_NEEDS_DELETE,
 * leaving it in limbo so the handler never runs. A real kqueue
 * echoes only the persistent attribute bits on delivery. We keep
 * EV_DISPATCH (so libdispatch clears DU_STATE_ARMED and re-arms),
 * EV_CLEAR, and EV_UDATA_SPECIFIC; everything else is dropped.
 */
#define MACH_KEV_DELIVERY_FLAGS_MASK \
	((uint16_t)(EV_DISPATCH | EV_CLEAR | EV_UDATA_SPECIFIC))

extern int __sys_kevent(int kq, const struct kevent *changelist, int nchanges,
    struct kevent *eventlist, int nevents, const struct timespec *timeout);

/*
 * Registration record. One per (kq, original ident) tuple.
 *   - wrap_pset: a libmach-owned pset always allocated; for port
 *     idents we move the ident into it; for pset idents we use the
 *     ident itself (mach_port_move_member rejects pset→pset). The native
 *     EVFILT_MACHPORT knote is registered on this pset; on its readiness
 *     wake we drain the pset via mach_msg into the backlog.
 */
/*
 * Per-message backlog entry. Multiple Mach messages can arrive
 * between userland wakes (burst). The wrapper drains the pset to
 * MACH_RCV_TIMED_OUT on each EVFILT_READ wake and queues messages
 * here; subsequent kevent_qos calls deliver from the backlog before
 * touching the kernel. Without this, messages 2..N of a burst sit
 * in the pset until the (N+1)'th arrival — observable as dropped /
 * delayed events for configd boot storms, IOKit power-state
 * transitions, and other multi-message bursts. See task #39
 * Path B design notes.
 */
struct mach_kev_msg {
	struct mach_kev_msg	*next;
	mach_msg_header_t	*hdr;	/* received message buffer or NULL */
	mach_msg_size_t		 size;	/* buffer size */
	mach_msg_return_t	 kr;	/* mach_msg return code */
};

struct mach_kev_reg {
	struct mach_kev_reg	*next;
	int			 kq;		/* user's kq fd */
	uint64_t		 ident;		/* original MACHPORT ident */
	mach_port_name_t	 wrap_pset;	/* libmach-owned pset; the native
						 * EVFILT_MACHPORT knote watches it */
	uint64_t		 udata;		/* original kev->udata */
	uint16_t		 flags;		/* original kev->flags */
	uint32_t		 fflags;	/* original kev->fflags */
	uint32_t		 qos;		/* original kev->qos */
	uint32_t		 xflags;	/* original kev->xflags */
	int64_t			 data;		/* original kev->data */
	/* Burst-handling backlog: messages drained from the pset but
	 * not yet delivered to the caller because they ran out of
	 * eventlist slots. */
	struct mach_kev_msg	*backlog_head;
	struct mach_kev_msg	*backlog_tail;
};

static struct mach_kev_reg	*g_reg_head;
static pthread_mutex_t		 g_reg_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct mach_kev_reg *
reg_find_locked(int kq, uint64_t ident)
{
	struct mach_kev_reg *r;
	for (r = g_reg_head; r != NULL; r = r->next) {
		if (r->kq == kq && r->ident == ident)
			return (r);
	}
	return (NULL);
}

static struct mach_kev_reg *
reg_find_by_wrap_pset_locked(mach_port_name_t wrap_pset)
{
	struct mach_kev_reg *r;
	for (r = g_reg_head; r != NULL; r = r->next) {
		if (r->wrap_pset == wrap_pset)
			return (r);
	}
	return (NULL);
}

static void
reg_insert_locked(struct mach_kev_reg *r)
{
	r->next = g_reg_head;
	g_reg_head = r;
}

static void
reg_remove_locked(struct mach_kev_reg *target)
{
	struct mach_kev_reg **pp;
	for (pp = &g_reg_head; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == target) {
			*pp = target->next;
			return;
		}
	}
}

/*
 * Build the userland half of a registration: allocate a wrap pset and move
 * the original ident into it (if ident is a port; pset-into-pset is rejected
 * by the kernel and we fall back to using the ident pset directly). The
 * caller registers the native EVFILT_MACHPORT knote on r->wrap_pset via the
 * `out` changelist entry. Returns 0 on success, errno on failure with all
 * partial state cleaned up.
 */
/*
 * Release a port set name.
 *
 * mach_port_deallocate() only drops send / send-once / dead-name urefs; against
 * a MACH_PORT_TYPE_PORT_SET entry it returns KERN_INVALID_RIGHT and does
 * nothing at all. Every wrap pset allocated here was therefore leaked -- and
 * because port names are file descriptors in this port (ipc_entry.c allocates
 * them through kern_fdalloc), each leak also burns an fd.
 *
 * This library already documents the rule for the analogous receive-right case,
 * in mach_traps.c:797: "must be reclaimed with mach_port_mod_refs(...,
 * RIGHT_RECEIVE, -1), not mach_port_deallocate (which only drops send/send-once
 * urefs) -- the previous error-path code used the wrong call." The same is true
 * of port sets; the rule was written down and then broken next door.
 *
 * Measured on hardware before this fix (#105): RPC-driven, monotonic fd growth,
 * never reclaimed, ~1 per 27-60 RPCs in both client and server.
 */
static void
reg_release_pset(mach_port_name_t pset)
{
	if (pset == MACH_PORT_NULL)
		return;
	(void)mach_port_mod_refs(mach_task_self(), pset,
	    MACH_PORT_RIGHT_PORT_SET, -1);
}

static int
reg_create(int kq, const struct kevent_qos_s *src, struct mach_kev_reg **out)
{
	struct mach_kev_reg *r;
	mach_port_name_t wrap = MACH_PORT_NULL;
	kern_return_t kr;
	int err;

	r = calloc(1, sizeof(*r));
	if (r == NULL)
		return (ENOMEM);

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
	    &wrap);
	if (kr != KERN_SUCCESS) {
		err = EINVAL;
		goto fail;
	}

	kr = mach_port_move_member(mach_task_self(),
	    (mach_port_name_t)src->ident, wrap);
	if (kr != KERN_SUCCESS) {
		/* ident is likely already a pset. Drop our wrap pset and use
		 * the ident pset directly as the EVFILT_MACHPORT target. */
		reg_release_pset(wrap);
		wrap = (mach_port_name_t)src->ident;
	}

	r->kq = kq;
	r->ident = src->ident;
	r->wrap_pset = wrap;
	r->udata = src->udata;
	r->flags = src->flags;
	r->fflags = src->fflags;
	r->qos = src->qos;
	r->xflags = src->xflags;
	r->data = src->data;
	*out = r;
	return (0);

fail:
	if (wrap != MACH_PORT_NULL &&
	    wrap != (mach_port_name_t)src->ident) {
		reg_release_pset(wrap);
	}
	free(r);
	return (err);
}

static void
reg_destroy(struct mach_kev_reg *r)
{
	if (r == NULL)
		return;
	/* Drain any queued backlog messages — buffers were malloc'd by
	 * backlog_recv_one_locked and never delivered. */
	while (r->backlog_head != NULL) {
		struct mach_kev_msg *bm = r->backlog_head;
		r->backlog_head = bm->next;
		if (bm->hdr != NULL) {
			/*
			 * mach_msg_destroy() before free(), always. These
			 * buffers hold a received message: freeing the memory
			 * leaks every port right and OOL region inside it. The
			 * remote port is typically the sender's reply port, so
			 * dropping its send right here means the peer never
			 * goes dead-name and its dead-name notification never
			 * fires. Apple pairs the two on every drop path
			 * (libdispatch mach.c:624, :680, :811).
			 */
			mach_msg_destroy(bm->hdr);
			free(bm->hdr);
		}
		free(bm);
	}
	r->backlog_tail = NULL;
	/*
	 * Release our wrap pset (unless it's the caller-owned ident pset).
	 * Releasing the pset detaches any native EVFILT_MACHPORT knote on it
	 * kernel-side (ipc_pset_destroy → knlist_clear), so the EV_DELETE the
	 * caller submits afterwards finds the knote already gone and
	 * filt_machportdetach (kn_knlist == NULL guard) is a no-op — no UAF,
	 * regardless of ordering. Requires the kernel knote-teardown hardening
	 * (nextbsd-kernel #29).
	 */
	if (r->wrap_pset != MACH_PORT_NULL &&
	    r->wrap_pset != (mach_port_name_t)r->ident) {
		/*
		 * Release the set, but do NOT un-member the caller's port first.
		 *
		 * An earlier version of this called
		 *
		 *	mach_port_move_member(task, r->ident, MACH_PORT_NULL)
		 *
		 * here, reasoning that releasing a set does not un-member its
		 * ports and the caller's receive right would otherwise be
		 * stranded in a set nothing receives on. Bisected on hardware:
		 * that call is what breaks event delivery. syslogd came up with
		 * one thread instead of five, accepted writes and processed
		 * none of them.
		 *
		 * reg_destroy() runs on EV_DELETE -- deregistration -- and
		 * libdispatch routinely re-registers the same port immediately
		 * afterwards. Un-membering here pulls the port out from under a
		 * knote that is still live, so messages have nowhere to be
		 * delivered. Releasing the set alone is safe: the membership
		 * goes away with it.
		 *
		 * Verified: release-only is healthy (6 threads, ASL round-trip
		 * works); release + un-member is not.
		 */
		reg_release_pset(r->wrap_pset);
	}
	free(r);
}

/*
 * Flags for the native readiness knote on the wrap pset.
 *
 * Message mode (MACH_RCV_MSG in fflags, a dispatch_mach_t channel): plain
 * EV_ADD, level-triggered -- see the EV_CLEAR note at the Phase 3 skip in
 * kevent_qos(). The wrapper drains the pset on every fire, so the
 * knote goes quiet by itself.
 *
 * Notify mode (a DISPATCH_SOURCE_TYPE_MACH_RECV source): the caller's
 * EV_DISPATCH / EV_ENABLE / EV_DISABLE go through to the kernel. Nothing
 * here receives the message -- the source's handler does -- so a
 * level-triggered knote without EV_DISPATCH stays active from the moment
 * the message lands until the handler's mach_msg() takes it, and every
 * kevent poll in that window fires the source again. libdispatch merges
 * each of those into ds_pending_data whether or not it believes the unote
 * is armed, so one that lands after the handler has latched its data runs
 * the handler a second time against a port it has just emptied: the
 * MACH_RCV_TIMED_OUT in test_libdispatch_mach (#420). It needs a
 * preemption in exactly that window, which is why it shows up on the
 * single-CPU arm64 CI guest and only occasionally.
 *
 * On Darwin the kernel disables an EV_DISPATCH knote when it delivers it
 * and libdispatch re-arms it with EV_ENABLE once the handler has run, so a
 * MACH_RECV source fires once per arm and only while a message is queued.
 * FreeBSD's kqueue implements EV_DISPATCH the same way (kqueue_scan sets
 * KN_DISABLED; an EV_ENABLE re-runs filt_machport before re-queueing), so
 * passing the bits through gives exactly that.
 */
static uint16_t
machport_native_flags(const struct kevent_qos_s *src)
{
	if (src->fflags & MACH_RCV_MSG)
		return (EV_ADD);
	return ((uint16_t)(EV_ADD |
	    (src->flags & (EV_DISPATCH | EV_ENABLE | EV_DISABLE))));
}

/*
 * Translate one user-supplied MACHPORT changelist entry into the
 * EVFILT_READ entry we submit to __sys_kevent. EV_ADD allocates a
 * registration (or updates an existing one); EV_DELETE looks one up
 * and tears down.
 */
static int
machport_change_translate(int kq, const struct kevent_qos_s *src,
    struct kevent *out)
{
	struct mach_kev_reg *r;
	int err;

	if (getenv("MACH_DEBUG_WRAP") != NULL) {
		fprintf(stderr, "[WRAP] change kq=%d ident=0x%llx flags=0x%x "
		    "fflags=0x%x udata=0x%llx\n", kq,
		    (unsigned long long)src->ident, src->flags, src->fflags,
		    (unsigned long long)src->udata);
	}

	pthread_mutex_lock(&g_reg_mtx);
	if (src->flags & EV_DELETE) {
		r = reg_find_locked(kq, src->ident);
		if (r != NULL)
			reg_remove_locked(r);
		pthread_mutex_unlock(&g_reg_mtx);
		if (r != NULL) {
			/* EV_DELETE the native knote on the wrap pset. (EV_SET
			 * reads r->wrap_pset before reg_destroy frees r.)
			 * reg_destroy then deallocates the pset, which already
			 * detaches the knote kernel-side, so this EV_DELETE is a
			 * harmless ENOENT for an our-owned pset, or a clean detach
			 * for the caller-owned ident-pset case. */
			EV_SET(out, (uintptr_t)r->wrap_pset,
			    EVFILT_MACHPORT_NATIVE, EV_DELETE, 0, 0, NULL);
			reg_destroy(r);
		} else {
			/* No matching registration — surface ENOENT
			 * via the real syscall on a sentinel ident. */
			EV_SET(out, (uintptr_t)-1, EVFILT_MACHPORT_NATIVE,
			    EV_DELETE, 0, 0, NULL);
		}
		return (0);
	}

	r = reg_find_locked(kq, src->ident);
	if (r != NULL) {
		r->udata = src->udata;
		r->flags = src->flags;
		r->fflags = src->fflags;
		r->qos = src->qos;
		r->xflags = src->xflags;
		r->data = src->data;
		/* Native EVFILT_MACHPORT on the wrap pset, notify mode
		 * (fflags = 0, no MACH_RCV_MSG): filt_machport signals
		 * readiness without consuming; we drain via mach_msg on fire.
		 * For a MACH_RECV source this is also the EV_ENABLE re-arm. */
		/* LEVEL-triggered, deliberately: see the EV_CLEAR note at
		 * the Phase 3 skip in mach_kevent_qos(). */
		EV_SET(out, (uintptr_t)r->wrap_pset, EVFILT_MACHPORT_NATIVE,
		    machport_native_flags(src), 0, 0, r);
		pthread_mutex_unlock(&g_reg_mtx);
		return (0);
	}
	pthread_mutex_unlock(&g_reg_mtx);

	err = reg_create(kq, src, &r);
	if (err != 0) {
		errno = err;
		return (-1);
	}
	pthread_mutex_lock(&g_reg_mtx);
	reg_insert_locked(r);
	pthread_mutex_unlock(&g_reg_mtx);
	/* LEVEL-triggered: see the EV_CLEAR note at the Phase 3 skip. */
	EV_SET(out, (uintptr_t)r->wrap_pset, EVFILT_MACHPORT_NATIVE,
	    machport_native_flags(src), 0, 0, r);
	return (0);
}

/*
 * Receive one Mach message from the pset (non-blocking) and push the
 * result onto the registration's backlog. Returns 0 if a message was
 * queued, 1 if the pset is empty (MACH_RCV_TIMED_OUT — stop draining),
 * or -1 on hard error (caller stops draining).
 *
 * Caller MUST hold g_reg_mtx. mach_msg with MACH_RCV_TIMEOUT=0 is
 * effectively non-blocking — short syscall — so holding the mutex
 * across it is acceptable.
 */
static int
backlog_recv_one_locked(struct mach_kev_reg *r)
{
	struct mach_kev_msg *bm;
	mach_msg_header_t *hdr;
	mach_msg_return_t kr;
	mach_msg_size_t bufsize = MACH_RCV_INITIAL_BUFSIZE;

	hdr = malloc(bufsize);
	if (hdr == NULL)
		return (-1);

	/*
	 * The trailer elements are load-bearing, not decoration.
	 *
	 * Without MACH_RCV_TRAILER_ELEMENTS the kernel never runs
	 *
	 *	trailer->msgh_trailer_size = REQUESTED_TRAILER_SIZE(option)
	 *
	 * (mach_msg.c, guarded by `option & MACH_RCV_TRAILER_MASK`), so the
	 * trailer stays at MACH_MSG_TRAILER_MINIMUM_SIZE -- 8 bytes, type and
	 * size only. Any MIG routine declaring ServerAuditToken then fails in
	 * its generated server stub on
	 *
	 *	trailer_size < sizeof(audit_token_t)		 (8 < 32)
	 *
	 * and returns MIG_TRAILER_ERROR (-309) to the client.
	 *
	 * That is #91. notify_ipc.defs declares ServerAuditToken on the checkin
	 * routines, so notifyd rejected every checkin; libnotify reported it as
	 * NOTIFY_STATUS_SERVER_CHECKIN_FAILED, flattened again to
	 * NOTIFY_STATUS_FAILED, and syslogd sat in ipc_mqueue_receive after an
	 * RPC that had genuinely failed. It was never a lost wakeup.
	 *
	 * Kernel diagnostic that pinned it (nextbsd-kernel#135):
	 *
	 *	MIGTRAILER: id=1023 size=32 opt=0x106 type=0 tsize=8 moved=0
	 *
	 * opt=0x106 is exactly RCV_MSG|RCV_LARGE|RCV_TIMEOUT with nothing in
	 * the trailer field, and tsize=8 is the resulting minimum trailer.
	 *
	 * CTX rather than AUDIT to match Apple's dispatch_mig_server()
	 * (libdispatch mach.c:2959), which asks for the same set. The trailer
	 * structs are cumulative, so mach_msg_context_trailer_t already
	 * contains msgh_audit.
	 */
	static const mach_msg_options_t rcv_opts =
	    MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT |
	    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX) |
	    MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0);

	kr = mach_msg(hdr, rcv_opts, 0, bufsize, r->wrap_pset, 0,
	    MACH_PORT_NULL);

	if (kr == MACH_RCV_TOO_LARGE) {
		/*
		 * msgh_size is the MESSAGE size and excludes the trailer, so
		 * allocate headroom for it -- now that we ask for a bigger
		 * trailer, retrying with exactly msgh_size can land back on
		 * MACH_RCV_TOO_LARGE forever.
		 */
		mach_msg_size_t need = hdr->msgh_size + MAX_TRAILER_SIZE;
		free(hdr);
		hdr = malloc(need);
		if (hdr == NULL)
			return (-1);
		bufsize = need;
		kr = mach_msg(hdr, rcv_opts, 0, bufsize, r->wrap_pset, 0,
		    MACH_PORT_NULL);
	}

	if (kr == MACH_RCV_TIMED_OUT) {
		/* Pset empty — done draining. */
		free(hdr);
		return (1);
	}

	bm = calloc(1, sizeof(*bm));
	if (bm == NULL) {
		free(hdr);
		return (-1);
	}
	bm->hdr = hdr;
	bm->size = bufsize;
	bm->kr = kr;

	if (r->backlog_tail != NULL) {
		r->backlog_tail->next = bm;
	} else {
		r->backlog_head = bm;
	}
	r->backlog_tail = bm;

	/* On non-success non-timeout errors, stop draining — the kr is
	 * recorded in this backlog entry; the next call would just hit
	 * the same error. */
	if (kr != MACH_MSG_SUCCESS)
		return (1);

	return (0);
}

/*
 * Drain the pipe's wakeup bytes, then populate the backlog. Caller
 * holds g_reg_mtx.
 *
 * Two modes, selected by the registration's fflags (saved from the
 * EVFILT_MACHPORT registration libdispatch issued):
 *
 *  - Message mode (fflags & MACH_RCV_MSG): the registration is a
 *    dispatch_mach_t channel (_dispatch_mach_type_recv, dst_fflags =
 *    DISPATCH_MACH_RCV_OPTIONS). libdispatch wants the message
 *    delivered inline — drain the pset via mach_msg into the backlog
 *    so each message becomes an EVFILT_MACHPORT event with ext[0].
 *
 *  - Notify mode (no MACH_RCV_MSG): the registration is a plain
 *    DISPATCH_SOURCE_TYPE_MACH_RECV source (dst_fflags = 0,
 *    dst_merge_msg = NULL). libdispatch wants only a readability
 *    notification — its handler does the mach_msg itself. Do NOT
 *    touch the pset; queue one bare backlog entry (hdr = NULL) so a
 *    single zero-message EVFILT_MACHPORT event fires the handler.
 *    Calling _dispatch_kevent_mach_msg_drain's NULL dst_merge_msg
 *    for this source type was the SIGSEGV.
 *
 *    Bare entries coalesce: if the backlog tail is already a bare
 *    entry, don't queue another — multiple wakeups collapse to one
 *    pending readability event (correct edge-trigger semantics).
 */
static void
backlog_drain_locked(struct mach_kev_reg *r)
{
	/* Woken by the native EVFILT_MACHPORT readiness event on the wrap
	 * pset (notify mode — the message was NOT consumed by the kernel), so
	 * drain it here exactly as the pipe-bridge path did. */
	if (r->fflags & MACH_RCV_MSG) {
		/* Message mode — pull every available message. */
		for (;;) {
			int rc = backlog_recv_one_locked(r);
			if (rc != 0)
				break;
		}
		return;
	}

	/* Notify mode — queue one bare readability event, coalescing. */
	if (r->backlog_tail != NULL && r->backlog_tail->hdr == NULL)
		return;
	struct mach_kev_msg *bm = calloc(1, sizeof(*bm));
	if (bm == NULL)
		return;
	bm->hdr = NULL;
	bm->size = 0;
	bm->kr = MACH_MSG_SUCCESS;
	if (r->backlog_tail != NULL)
		r->backlog_tail->next = bm;
	else
		r->backlog_head = bm;
	r->backlog_tail = bm;
}

/*
 * Undo the kernel's EV_DISPATCH disable for a readiness event Phase 3 is
 * not going to hand over: one skipped on an ERROR_EVENTS call, or one
 * past the end of the caller's eventlist.
 *
 * kqueue_scan() sets KN_DISABLED on an EV_DISPATCH knote as it hands the
 * event over, so for a notify-mode registration (machport_native_flags())
 * skipping the event would leave the knote disabled while libdispatch
 * still believes the unote is armed -- the source would never fire again.
 * Re-enable it instead: kqueue_register() re-runs filt_machport, and if
 * the message is still queued the knote is re-queued, waking whichever
 * thread is blocked in kevent, so the next ordinary poll delivers it.
 * EV_KEEPUDATA leaves udata alone; ENOENT (the registration was torn down
 * meanwhile) is harmless. Message-mode knotes are never disabled.
 */
static void
machport_reenable_undelivered(int kq, const struct kevent *ev)
{
	static const struct timespec zero_ts = { 0, 0 };
	struct mach_kev_reg *r;
	struct kevent en;

	pthread_mutex_lock(&g_reg_mtx);
	r = reg_find_by_wrap_pset_locked((mach_port_name_t)ev->ident);
	if (r != NULL && r->kq == kq && !(r->fflags & MACH_RCV_MSG) &&
	    (r->flags & EV_DISPATCH)) {
		EV_SET(&en, ev->ident, EVFILT_MACHPORT_NATIVE,
		    EV_ENABLE | EV_KEEPUDATA, 0, 0, NULL);
		(void)__sys_kevent(kq, &en, 1, NULL, 0, &zero_ts);
	}
	pthread_mutex_unlock(&g_reg_mtx);
}

/*
 * Pop one message from the registration's backlog and synthesize an
 * Apple-shape EVFILT_MACHPORT event into `synth`. Returns 1 if an
 * event was synthesized, 0 if the backlog was empty. Caller holds
 * g_reg_mtx.
 */
static int
backlog_pop_locked(struct mach_kev_reg *r, struct kevent_qos_s *synth)
{
	struct mach_kev_msg *bm = r->backlog_head;

	if (bm == NULL)
		return (0);

	r->backlog_head = bm->next;
	if (r->backlog_head == NULL)
		r->backlog_tail = NULL;

	memset(synth, 0, sizeof(*synth));
	synth->ident = r->ident;
	synth->filter = EVFILT_MACHPORT;
	synth->flags = r->flags & MACH_KEV_DELIVERY_FLAGS_MASK;
	synth->fflags = (uint32_t)bm->kr;
	synth->data = r->data;
	synth->udata = r->udata;
	synth->qos = r->qos;
	synth->xflags = r->xflags;
	if (bm->kr == MACH_MSG_SUCCESS && bm->hdr != NULL) {
		/* Message mode: carry the recv'd buffer inline. ext[1]
		 * non-zero is what makes _dispatch_kevent_drain route to
		 * _dispatch_kevent_mach_msg_drain; NEEDS_FREE tells
		 * libdispatch to free() it afterwards. */
		synth->ext[0] = (uint64_t)(uintptr_t)bm->hdr;
		synth->ext[1] = bm->size;
		synth->flags |= DISPATCH_EV_MSG_NEEDS_FREE;
	} else if (bm->hdr != NULL) {
		/* Errored message — buffer unused; free it now. ext[]
		 * stays zero so this delivers as a bare event. */
		free(bm->hdr);
	}
	/* bm->hdr == NULL → bare readability event: ext[0]/ext[1] stay
	 * 0, no NEEDS_FREE. _dispatch_kevent_drain sees zero msg size
	 * and routes to the normal merge path (dst_merge_evt). */
	if (getenv("MACH_DEBUG_WRAP") != NULL) {
		fprintf(stderr, "[WRAP] deliver ident=0x%llx udata=0x%llx "
		    "fflags=0x%x ext0=0x%llx ext1=%llu\n",
		    (unsigned long long)synth->ident,
		    (unsigned long long)synth->udata, synth->fflags,
		    (unsigned long long)synth->ext[0],
		    (unsigned long long)synth->ext[1]);
	}
	free(bm);
	return (1);
}

/*
 * Translate a FreeBSD struct kevent (output of __sys_kevent for a
 * non-mach changelist entry) back to kevent_qos_s. ext[] and qos
 * are zeroed.
 */
static void
kev_to_qos(const struct kevent *in, struct kevent_qos_s *out)
{
	memset(out, 0, sizeof(*out));
	out->ident = (uint64_t)in->ident;
	out->filter = in->filter;
	out->flags = in->flags;
	out->fflags = in->fflags;
	out->data = in->data;
	out->udata = (uint64_t)(uintptr_t)in->udata;
}

/*
 * Translate kevent_qos_s → struct kevent for non-MACHPORT entries.
 * ext[] and qos/xflags are dropped at the FreeBSD boundary.
 */
static void
qos_to_kev(const struct kevent_qos_s *in, struct kevent *out)
{
	EV_SET(out, (uintptr_t)in->ident, in->filter, in->flags,
	    in->fflags, in->data, (void *)(uintptr_t)in->udata);
}

#define KEVENT_STACK_SLOTS 16

/*
 * Report per-change failures as EV_ERROR events, the way a real kevent() does.
 *
 * libdispatch's KEVENT_FLAG_ERROR_EVENTS path inspects nothing else: it scans
 * for (flags & EV_ERROR) && data, and treats a zero/absent result as "the
 * change installed". Returning a non-negative count without these events is
 * how a failed registration became a permanently ARMED unote on a knote that
 * does not exist.
 *
 * ident / filter / udata are copied from the caller's original change so the
 * event lands on the right unote.
 */
static int
emit_failed_changes(struct kevent_qos_s *eventlist, int nevents, int filled,
    const struct kevent_qos_s *failed_ch, const int *failed_err, int nfailed)
{
	int k;

	for (k = 0; k < nfailed && filled < nevents; k++) {
		eventlist[filled] = failed_ch[k];
		eventlist[filled].flags |= EV_ERROR;
		eventlist[filled].data = (int64_t)failed_err[k];
		filled++;
	}
	return (filled);
}

/*
 * Drain previously-queued backlog into the eventlist, up to `nevents`
 * slots for this kq. Returns the number of slots filled.
 *
 * Called BEFORE __sys_kevent so a burst that arrived between
 * kevent_qos calls still drains in subsequent calls without
 * requiring a fresh pipe-bell wake.
 */
static int
drain_backlog_to_eventlist(int kq, struct kevent_qos_s *eventlist,
    int nevents)
{
	struct mach_kev_reg *r;
	int filled = 0;

	pthread_mutex_lock(&g_reg_mtx);
	for (r = g_reg_head; r != NULL && filled < nevents; r = r->next) {
		if (r->kq != kq)
			continue;
		while (r->backlog_head != NULL && filled < nevents) {
			if (backlog_pop_locked(r, &eventlist[filled]))
				filled++;
		}
	}
	pthread_mutex_unlock(&g_reg_mtx);
	return (filled);
}

int
kevent_qos(int kq, const struct kevent_qos_s *changelist, int nchanges,
    struct kevent_qos_s *eventlist, int nevents,
    void *data_out, size_t *data_available, unsigned int flags)
{
	struct kevent stack_ch[KEVENT_STACK_SLOTS];
	struct kevent stack_ev[KEVENT_STACK_SLOTS];
	struct kevent *scratch_ch = stack_ch;
	struct kevent *scratch_ev = stack_ev;
	struct timespec zero_ts = { 0, 0 };
	const struct timespec *timeout = NULL;
	int saved_errno = 0;
	int filled = 0;
	int i, n, rc;
	/*
	 * Per-change failures. A change that cannot be translated is NOT
	 * submitted to the kernel; it is reported back as an EV_ERROR event,
	 * which is what a real kevent() does and the only form libdispatch
	 * inspects on a KEVENT_FLAG_ERROR_EVENTS call.
	 */
	struct kevent_qos_s failed_ch[KEVENT_STACK_SLOTS];
	int failed_err[KEVENT_STACK_SLOTS];
	int nfailed = 0;
	int nsubmit = 0;

	(void)data_out;
	if (data_available != NULL)
		*data_available = 0;

	if (flags & KEVENT_FLAG_IMMEDIATE)
		timeout = &zero_ts;

	if (nchanges > 0) {
		if (nchanges > KEVENT_STACK_SLOTS) {
			scratch_ch = calloc((size_t)nchanges,
			    sizeof(*scratch_ch));
			if (scratch_ch == NULL) {
				errno = ENOMEM;
				return (-1);
			}
		}
		for (i = 0; i < nchanges; i++) {
			if (changelist[i].filter == EVFILT_MACHPORT) {
				if (machport_change_translate(kq,
				    &changelist[i],
				    &scratch_ch[nsubmit]) != 0) {
					/*
					 * Record the failure for this slot and
					 * submit nothing for it.
					 *
					 * This used to substitute an EV_ADD on
					 * EVFILT_READ against fd (uintptr_t)-1
					 * and still return a non-negative count,
					 * so libdispatch read it as success and
					 * left the unote ARMED on a knote that
					 * does not exist -- a permanent silent
					 * hang of that channel. When the bogus
					 * fd did surface it produced EBADF,
					 * which _dispatch_kq_poll turns into
					 * DISPATCH_CLIENT_CRASH("Do not close
					 * random Unix descriptors") -- a crash
					 * blamed on the wrong thing entirely.
					 *
					 * A real kevent reports per-change
					 * failure as an EV_ERROR event carrying
					 * errno in .data, which is the only
					 * thing libdispatch's ERROR_EVENTS loop
					 * inspects. Synthesize exactly that,
					 * preserving the caller's ident/filter/
					 * udata so it lands on the right unote.
					 */
					saved_errno = errno;
					if (nfailed < KEVENT_STACK_SLOTS) {
						failed_ch[nfailed] =
						    changelist[i];
						failed_err[nfailed] =
						    saved_errno;
						nfailed++;
					}
					continue;
				}
				nsubmit++;
			} else {
				qos_to_kev(&changelist[i],
				    &scratch_ch[nsubmit]);
				nsubmit++;
			}
		}
	}

	/*
	 * Phase 1: drain pre-existing backlog into the caller's
	 * eventlist. A burst may have left messages queued from a
	 * prior call.
	 */
	/*
	 * KEVENT_FLAG_ERROR_EVENTS means "report results for the submitted
	 * changes, do NOT dequeue pending events". libdispatch relies on that
	 * literally: _dispatch_kq_drain() keeps only entries with EV_ERROR set
	 * on such a call and discards everything else
	 * (event_kevent.c, the `flags & KEVENT_FLAG_ERROR_EVENTS` loop).
	 *
	 * Draining here handed real Mach messages back on a registration call
	 * and libdispatch threw them away. The message was already removed from
	 * the kernel queue, so it was gone -- no retry possible.
	 *
	 * That is a hang, and a timing-dependent one:
	 * _dispatch_mach_reply_kevent_register() runs AFTER the request is sent
	 * (mach.c: send, then register), so when a fast server's reply wins the
	 * race it is consumed and destroyed by the very call registering
	 * interest in it. The client then waits forever while the server is
	 * healthy and answering everyone else.
	 */
	if (nevents > 0 && !(flags & KEVENT_FLAG_ERROR_EVENTS))
		filled = drain_backlog_to_eventlist(kq, eventlist, nevents);

	/*
	 * Phase 2: invoke the real syscall. Always required if
	 * nchanges > 0 (must register the changes); also if we still
	 * have eventlist budget AFTER backlog drain. If backlog
	 * already filled the budget AND no changes, skip the syscall.
	 */
	if (nchanges > 0 && nsubmit == 0 && nfailed == nchanges) {
		/*
		 * The caller submitted changes and every one failed to
		 * translate. The failures are the whole result, so the syscall
		 * must not run.
		 *
		 * nchanges > 0 is load-bearing. Without it this also caught the
		 * ordinary "wait for events" call (nchanges == 0, nsubmit == 0)
		 * and returned immediately instead of invoking the syscall --
		 * which silently killed ALL event delivery: syslogd kept its
		 * dispatch workers idle, accepted writes, and processed
		 * nothing. Caught on hardware, not in CI.
		 */
		filled = emit_failed_changes(eventlist, nevents, filled,
		    failed_ch, failed_err, nfailed);
		if (nchanges > KEVENT_STACK_SLOTS)
			free(scratch_ch);
		if (saved_errno != 0)
			errno = saved_errno;
		return (filled);
	}
	/*
	 * Return whatever the backlog gave us rather than sleeping on it.
	 *
	 * This used to require filled == nevents, i.e. a COMPLETELY full
	 * eventlist. With 0 < filled < nevents and nothing to submit, control
	 * fell through to the blocking __sys_kevent() below -- whose timeout is
	 * NULL unless KEVENT_FLAG_IMMEDIATE -- and slept indefinitely while
	 * holding already-received messages in the userland backlog.
	 *
	 * Those messages have already been mach_msg()'d out of the wrap port
	 * set, so the kernel will never signal for them again: ipc_pset_signal()
	 * fires on ENQUEUE, and the enqueue already happened. The daemon then
	 * sits healthy and idle in the kernel while the client blocks forever
	 * waiting for a reply that is sitting in a linked list two frames away.
	 * That is the wedge in #78, and it needs no lost wakeup in the kernel to
	 * happen -- we strand the message ourselves.
	 *
	 * kevent()'s contract is to return once at least one event is available,
	 * not once the caller's array is full, so returning here is also simply
	 * what the syscall is supposed to do.
	 */
	if (nchanges == 0 && filled > 0) {
		if (saved_errno != 0)
			errno = saved_errno;
		return (filled);
	}

	int ev_budget = nevents - filled;
	if (ev_budget > KEVENT_STACK_SLOTS) {
		scratch_ev = calloc((size_t)ev_budget, sizeof(*scratch_ev));
		if (scratch_ev == NULL) {
			if (nchanges > KEVENT_STACK_SLOTS)
				free(scratch_ch);
			errno = ENOMEM;
			return (-1);
		}
	}

	rc = __sys_kevent(kq, nsubmit > 0 ? scratch_ch : NULL, nsubmit,
	    ev_budget > 0 ? scratch_ev : NULL, ev_budget, timeout);

	if (nchanges > KEVENT_STACK_SLOTS)
		free(scratch_ch);

	if (rc < 0) {
		if (ev_budget > KEVENT_STACK_SLOTS)
			free(scratch_ev);
		if (filled > 0) {
			/* Backlog already produced events; report those
			 * and swallow the syscall error this round. */
			return (filled);
		}
		if (saved_errno != 0)
			errno = saved_errno;
		return (rc);
	}

	/*
	 * Phase 3: process returned events. For tracked pipes, drain
	 * the pset into the backlog and pop as many as fit.
	 */
	n = rc;
	for (i = 0; i < n; i++) {
		struct mach_kev_reg *r = NULL;

		/* Out of eventlist room: the rest are dropped, as before, but a
		 * notify-mode knote must be re-enabled to be seen again. */
		if (filled >= nevents) {
			if (scratch_ev[i].filter == EVFILT_MACHPORT_NATIVE)
				machport_reenable_undelivered(kq,
				    &scratch_ev[i]);
			continue;
		}

		/*
		 * On a KEVENT_FLAG_ERROR_EVENTS call, emit receipts ONLY --
		 * never drain a port set. This is the other half of the race
		 * described above, and it destroys live messages.
		 *
		 * libdispatch issues every registration and every EV_DISPATCH
		 * re-arm through _dispatch_kq_update_one(), which passes
		 * KEVENT_FLAG_IMMEDIATE | KEVENT_FLAG_ERROR_EVENTS
		 * (event_kevent.c) and an eventlist of 16 -- it wants receipts
		 * for the changes it submitted, nothing more. Its own
		 * EV_RECEIPT emulation is compiled out here because
		 * DISPATCH_USE_KEVENT_QOS is 1, so honouring "do not dequeue"
		 * is entirely on this wrapper.
		 *
		 * Phase 1 already honours it (see the guard at the drain
		 * above). Phase 3 did not: a readiness event arriving in the
		 * same call was drained with mach_msg -- taking the message
		 * OUT of the port set -- and handed up. libdispatch's
		 * ERROR_EVENTS loop keeps only entries with EV_ERROR set and
		 * silently discards the rest, so the message was destroyed,
		 * its buffer and any ports inside it leaked, and no retry was
		 * possible: the kernel had already dequeued it, so
		 * ipc_pset_signal() will never fire for it again.
		 *
		 * Two paths make that hot rather than rare. libdispatch sends
		 * before it registers the reply port (mach.c), so a fast local
		 * server's reply can be eaten by the very call registering
		 * interest in it -- client blocked in ipc_mqueue_receive,
		 * server healthy and idle, which is #78's signature exactly.
		 * And because every re-arm is also an ERROR_EVENTS call, this
		 * fires after every delivered message, not just at setup.
		 *
		 * Skipping the entry leaves the message queued, so the next
		 * ordinary poll delivers it -- but ONLY if the readiness knote is
		 * level-triggered, which is why it is now registered EV_ADD with
		 * no EV_CLEAR.
		 *
		 * It was EV_ADD | EV_CLEAR, and that made this skip silently lossy.
		 * With EV_CLEAR the kernel clears KN_ACTIVE | KN_QUEUED when it
		 * hands the event over (kern_event.c:2270), so the readiness is
		 * consumed by the very call that skips it. Nothing re-arms the
		 * knote -- ipc_pset_signal() only fires on a NEW enqueue -- so the
		 * message sits on the port until unrelated traffic signals the set
		 * again, and any client waiting for a reply hangs until then.
		 *
		 * Measured on the kernel side before this change: over four runs of
		 * 150 sends, userland was handed 919/901/906/901 "message waiting"
		 * notifications and performed 909/889/895/895 actual receives -- a
		 * persistent gap of 10/12/11/6, about 1% of notifications, never
		 * negative. aslmanager's service port holds an undelivered message
		 * from boot on every boot for the same reason: skipped once during
		 * registration, never signalled again. See nextbsd-userland#135 and
		 * nextbsd-kernel#145.
		 *
		 * Level-triggered is what this code always assumed. filt_machport
		 * returns non-zero while any member port holds a message, so kqueue
		 * re-queues the knote (kern_event.c:2273) and the next ordinary
		 * poll does deliver it. It cannot spin: ERROR_EVENTS calls are
		 * KEVENT_FLAG_IMMEDIATE and return at once anyway, and ordinary
		 * polls drain in Phase 3 below.
		 *
		 * A notify-mode knote is EV_DISPATCH (#420), which the kernel
		 * has just disabled by handing this event over; re-enable it so
		 * the skip stays lossless.
		 */
		if ((flags & KEVENT_FLAG_ERROR_EVENTS) &&
		    !(scratch_ev[i].flags & EV_ERROR)) {
			if (scratch_ev[i].filter == EVFILT_MACHPORT_NATIVE)
				machport_reenable_undelivered(kq,
				    &scratch_ev[i]);
			continue;
		}

		/*
		 * A native EVFILT_MACHPORT event is OURS. Its udata is the
		 * struct mach_kev_reg * we registered the knote with (see the
		 * EV_SET calls above), not a caller cookie -- so it must never
		 * reach the caller. Only backlog_pop_locked() produces events
		 * fit to hand out; it stamps the caller's own filter and udata.
		 *
		 * The lookup failing is not a reason to pass the event
		 * through. It means the registration is already gone, which is
		 * exactly when udata is a dangling pointer: reg_destroy() frees
		 * r, and releasing the wrap pset makes the kernel fire EV_EOF
		 * on the knote and mark it EV_ONESHOT (ipc_pset_destroy ->
		 * knlist_clear). kqueue_scan then copies that kevent out
		 * verbatim -- udata included -- without calling f_event.
		 *
		 * Falling through to kev_to_qos() here handed libdispatch a
		 * kevent with filter -16 and udata pointing at freed memory.
		 * _dispatch_kevent_get_unote() casts udata to a unote with no
		 * validation and _dispatch_kevent_merge() dereferences it,
		 * which crashed the dispatch manager thread roughly a third of
		 * the time on teardown of any Mach recv source. Confirmed from
		 * a core: filter -16, flags EV_EOF|EV_ONESHOT, and the "unote"
		 * was a freed mach_kev_reg whose first word (the unlinked list
		 * pointer) was NULL, so dux_type(du)->dst_action read 0x0+9.
		 *
		 * See nextbsd-userland#158. The trigger was #106, which turned
		 * reg_release_pset() from a no-op -- mach_port_deallocate()
		 * returns KERN_INVALID_RIGHT on a port set -- into a real
		 * release, so this teardown path started running at all.
		 */
		if (scratch_ev[i].filter == EVFILT_MACHPORT_NATIVE) {
			pthread_mutex_lock(&g_reg_mtx);
			r = reg_find_by_wrap_pset_locked(
			    (mach_port_name_t)scratch_ev[i].ident);
			if (r != NULL) {
				backlog_drain_locked(r);
				while (r->backlog_head != NULL &&
				    filled < nevents) {
					if (backlog_pop_locked(r,
					    &eventlist[filled]))
						filled++;
				}
			}
			pthread_mutex_unlock(&g_reg_mtx);
			continue;
		}
		kev_to_qos(&scratch_ev[i], &eventlist[filled]);
		filled++;
	}

	if (ev_budget > KEVENT_STACK_SLOTS)
		free(scratch_ev);

	filled = emit_failed_changes(eventlist, nevents, filled,
	    failed_ch, failed_err, nfailed);

	if (saved_errno != 0)
		errno = saved_errno;
	return (filled);
}
