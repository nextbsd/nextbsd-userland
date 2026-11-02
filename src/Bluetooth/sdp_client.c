/*
 * sdp_client.c — bluetoothd's SDP client, and the `bluetooth sdp` browse.
 * See sdp_client.h.
 *
 * Written from the Core Specification (Vol 3 Part B) over the kernel's L2CAP
 * socket, in place of libsdp's sdp_open()/sdp_search(): libsdp connects with no
 * timeout, and a device that is out of range would hold the calling thread for
 * the kernel's full L2CAP timeout. Here the connect and every response wait
 * have their own bound.
 *
 * Replaces `bthidcontrol query` (the HID record into the store) and
 * `sdpcontrol browse` (bluetooth sdp <addr>).
 */
#include "sdp_client.h"
#include "sdp_proto.h"
#include "bluetoothd.h"
#include "btstore.h"
#include "bluetooth_mig_types.h"

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/socket.h>

#include <bitstring.h>		/* ng_btsocket.h uses bitstr_t */

#define	L2CAP_SOCKET_CHECKED	/* every sockaddr_l2cap field is set below */
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/include/ng_l2cap.h>
#include <netgraph/bluetooth/include/ng_btsocket.h>

#include <mach/mach.h>
#include "bluetoothServer.h"		/* MIG-emitted prototypes */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Timeouts. Connecting may page the device (5.12 s by default) and then set
 * up L2CAP; a response is normally milliseconds once connected.
 */
#define	SDP_CONNECT_TIMEOUT_MS	20000
#define	SDP_RESPONSE_TIMEOUT_MS	5000

/* A peer's whole answer to one query, all continuations joined. */
#define	SDP_ANSWER_MAX		(128 * 1024)
#define	SDP_MAX_CONTINUATIONS	512

/* Lines one browse may produce. */
#define	SDP_BROWSE_MAX_LINES	4096

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[sdp] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* The session: one L2CAP channel to the peer's PSM 1                  */
/* ------------------------------------------------------------------ */

struct sdp_session {
	int		fd;
	uint16_t	imtu;		/* the largest PDU the peer may send */
	uint16_t	tid;
	uint8_t		*rbuf;
};

/* An attribute range for a request: [lo, hi]. */
struct sdp_qrange {
	uint16_t	lo;
	uint16_t	hi;
};

static int
wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pfd;
	struct timespec t0, t1;
	int rc, left = timeout_ms;

	(void)clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		pfd.fd = fd;
		pfd.events = events;
		pfd.revents = 0;
		rc = poll(&pfd, 1, left);
		if (rc > 0)
			return (0);
		if (rc == 0)
			return (ETIMEDOUT);
		if (errno != EINTR)
			return (errno);
		(void)clock_gettime(CLOCK_MONOTONIC, &t1);
		left = timeout_ms - (int)((t1.tv_sec - t0.tv_sec) * 1000 +
		    (t1.tv_nsec - t0.tv_nsec) / 1000000);
		if (left <= 0)
			return (ETIMEDOUT);
	}
}

static void
session_close(struct sdp_session *s)
{
	if (s == NULL)
		return;
	if (s->fd >= 0)
		(void)close(s->fd);
	free(s->rbuf);
	free(s);
}

static void
fill_sockaddr(struct sockaddr_l2cap *sa, const bdaddr_t *addr, uint16_t psm)
{
	(void)memset(sa, 0, sizeof(*sa));
	sa->l2cap_len = sizeof(*sa);
	sa->l2cap_family = AF_BLUETOOTH;
	sa->l2cap_psm = htole16(psm);
	(void)memcpy(&sa->l2cap_bdaddr, addr, sizeof(bdaddr_t));
	sa->l2cap_cid = 0;
	sa->l2cap_bdaddr_type = BDADDR_BREDR;
}

/* Open a channel to `peer`'s SDP server. Returns 0 or an errno. */
static int
session_open(const struct bt_addr *peer, struct sdp_session **out)
{
	struct sdp_session *s;
	struct sockaddr_l2cap sa;
	bdaddr_t ba;
	socklen_t len;
	int err, flags;

	*out = NULL;
	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return (ENOMEM);
	s->fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (s->fd < 0) {
		err = errno;
		goto fail;
	}

	/* Any local controller: the kernel routes by the peer's address. */
	fill_sockaddr(&sa, NG_HCI_BDADDR_ANY, 0);
	if (bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		err = errno;
		goto fail;
	}

	flags = fcntl(s->fd, F_GETFL);
	if (flags < 0 || fcntl(s->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		err = errno;
		goto fail;
	}
	(void)memcpy(&ba, peer->b, sizeof(ba));
	fill_sockaddr(&sa, &ba, SDP_PSM);
	if (connect(s->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		if (errno != EINPROGRESS) {
			err = errno;
			goto fail;
		}
		if ((err = wait_fd(s->fd, POLLOUT, SDP_CONNECT_TIMEOUT_MS)) != 0)
			goto fail;
		len = sizeof(err);
		if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
			err = errno;
		if (err != 0)
			goto fail;
	}

	len = sizeof(s->imtu);
	if (getsockopt(s->fd, SOL_L2CAP, SO_L2CAP_IMTU, &s->imtu, &len) != 0 ||
	    s->imtu < SDP_MIN_MTU)
		s->imtu = SDP_DEFAULT_MTU;
	s->rbuf = malloc(s->imtu);
	if (s->rbuf == NULL) {
		err = ENOMEM;
		goto fail;
	}
	s->tid = (uint16_t)arc4random();
	*out = s;
	return (0);

fail:
	session_close(s);
	return (err != 0 ? err : EIO);
}

static uint16_t
rd16(const uint8_t *p)
{
	return ((uint16_t)(p[0] << 8 | p[1]));
}

static void
put_ranges(struct sdp_wbuf *w, const struct sdp_qrange *r, int n)
{
	size_t m;
	int i;

	m = sdp_seq_begin(w);
	for (i = 0; i < n; i++) {
		if (r[i].lo == r[i].hi)
			sdp_put_uint16(w, r[i].lo);
		else
			sdp_put_uint32(w, (uint32_t)r[i].lo << 16 | r[i].hi);
	}
	sdp_seq_end(w, m);
}

/*
 * ServiceSearchAttribute for `uuids` (16-bit), `ranges`, following every
 * continuation. On success `out` holds the AttributeLists: a DES of one DES
 * per matching record. Returns 0 or an errno.
 */
static int
session_ssa(struct sdp_session *s, const uint16_t *uuids, int nuuids,
    const struct sdp_qrange *ranges, int nranges, struct sdp_wbuf *out)
{
	struct sdp_wbuf req;
	uint8_t cont[1 + SDP_CONT_MAX];
	uint16_t maxbytes;
	ssize_t n;
	size_t m;
	int i, iter, err = 0;

	(void)memset(&req, 0, sizeof(req));
	cont[0] = 0;
	/* What fits our incoming MTU with the header and a full continuation. */
	maxbytes = (uint16_t)(s->imtu - SDP_PDU_HDR_LEN - 2 - 1 -
	    SDP_CONT_MAX);

	for (iter = 0; iter < SDP_MAX_CONTINUATIONS; iter++) {
		const uint8_t *p, *end;
		uint16_t tid, cnt;
		uint8_t clen;

		tid = ++s->tid;
		req.len = 0;
		req.err = false;
		sdp_put_raw(&req, (const uint8_t[]){ SDP_PDU_SSA_REQUEST }, 1);
		sdp_put_be16(&req, tid);
		sdp_put_be16(&req, 0);
		m = sdp_seq_begin(&req);
		for (i = 0; i < nuuids; i++)
			sdp_put_uuid16(&req, uuids[i]);
		sdp_seq_end(&req, m);
		sdp_put_be16(&req, maxbytes);
		put_ranges(&req, ranges, nranges);
		sdp_put_raw(&req, cont, 1u + cont[0]);
		if (req.err) {
			err = ENOMEM;
			break;
		}
		req.b[3] = (uint8_t)((req.len - SDP_PDU_HDR_LEN) >> 8);
		req.b[4] = (uint8_t)(req.len - SDP_PDU_HDR_LEN);

		if (send(s->fd, req.b, req.len, 0) < 0) {
			err = errno;
			break;
		}

		/* Wait for the response to this transaction. */
		for (;;) {
			if ((err = wait_fd(s->fd, POLLIN,
			    SDP_RESPONSE_TIMEOUT_MS)) != 0)
				goto done;
			n = recv(s->fd, s->rbuf, s->imtu, 0);
			if (n < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				err = errno;
				goto done;
			}
			if (n == 0) {
				err = ECONNRESET;
				goto done;
			}
			if (n >= SDP_PDU_HDR_LEN && rd16(s->rbuf + 1) == tid)
				break;
			/* A stale or garbled PDU: keep waiting. */
		}

		p = s->rbuf;
		end = s->rbuf + n;
		if (rd16(p + 3) != (size_t)n - SDP_PDU_HDR_LEN) {
			err = EPROTO;
			break;
		}
		if (p[0] == SDP_PDU_ERROR_RESPONSE) {
			xlog("peer answered SDP error 0x%04x",
			    n >= 7 ? rd16(p + 5) : 0);
			err = EPROTO;
			break;
		}
		if (p[0] != SDP_PDU_SSA_RESPONSE || n < SDP_PDU_HDR_LEN + 3) {
			err = EPROTO;
			break;
		}
		p += SDP_PDU_HDR_LEN;
		cnt = rd16(p);
		p += 2;
		if ((size_t)(end - p) < (size_t)cnt + 1) {
			err = EPROTO;
			break;
		}
		sdp_put_raw(out, p, cnt);
		p += cnt;
		clen = p[0];
		if (clen > SDP_CONT_MAX || (size_t)(end - p) != 1u + clen) {
			err = EPROTO;
			break;
		}
		if (out->err || out->len > SDP_ANSWER_MAX) {
			err = E2BIG;
			break;
		}
		if (clen == 0)
			break;			/* complete */
		(void)memcpy(cont, p, 1u + clen);
	}
	if (err == 0 && iter == SDP_MAX_CONTINUATIONS)
		err = E2BIG;
done:
	sdp_wbuf_free(&req);
	return (err);
}

/*
 * Run one SSA on a fresh session. The answer must parse as a DES; `outer` is
 * then that DES, pointing into `ans`.
 */
static int
query(const struct bt_addr *peer, const uint16_t *uuids, int nuuids,
    const struct sdp_qrange *ranges, int nranges, struct sdp_wbuf *ans,
    struct sdp_de *outer)
{
	struct sdp_session *s;
	int err;

	if (peer->type != BT_ADDR_PUBLIC)
		return (EINVAL);		/* SDP is BR/EDR only */
	if ((err = session_open(peer, &s)) != 0)
		return (err);
	err = session_ssa(s, uuids, nuuids, ranges, nranges, ans);
	session_close(s);
	if (err != 0)
		return (err);
	if (ans->len == 0 ||
	    sdp_de_get(ans->b, ans->b + ans->len, outer) != ans->len ||
	    outer->type != SDP_DE_SEQ)
		return (EPROTO);
	return (0);
}

/* ------------------------------------------------------------------ */
/* Reading records                                                     */
/* ------------------------------------------------------------------ */

/* Find attribute `want` in one record (a DES of pairs). */
static bool
rec_attr(const struct sdp_de *rec, uint16_t want, struct sdp_de *val)
{
	struct sdp_iter it;
	uint16_t id;

	if (!sdp_iter_init(&it, rec))
		return (false);
	while (sdp_iter_attr(&it, &id, val)) {
		if (id == want)
			return (true);
	}
	return (false);
}

static bool
rec_uint(const struct sdp_de *rec, uint16_t id, uint64_t *v)
{
	struct sdp_de val;

	return (rec_attr(rec, id, &val) && sdp_de_uint(&val, v));
}

/* The record's ServiceClassIDList as short UUIDs; returns how many. */
static int
rec_classes(const struct sdp_de *rec, uint32_t *out, int max)
{
	struct sdp_de list, u;
	struct sdp_iter it;
	int n = 0;

	if (!rec_attr(rec, SDP_ATTR_SERVICE_CLASS_ID_LIST, &list) ||
	    !sdp_iter_init(&it, &list))
		return (0);
	while (n < max && sdp_iter_next(&it, &u)) {
		if (sdp_de_uuid32(&u, &out[n]))
			n++;
	}
	return (n);
}

/*
 * The L2CAP PSM in a protocol descriptor list { { L2CAP, psm }, ... }, or 0.
 * For AdditionalProtocolDescriptorLists (a list of such lists) `nested` looks
 * one level deeper.
 */
static uint16_t
proto_psm(const struct sdp_de *list, bool nested)
{
	struct sdp_iter it, it2;
	struct sdp_de stack, d, u, psm;
	uint32_t uuid;
	uint64_t v;

	if (!sdp_iter_init(&it, list))
		return (0);
	if (nested) {
		if (!sdp_iter_next(&it, &stack) || !sdp_iter_init(&it, &stack))
			return (0);
	}
	while (sdp_iter_next(&it, &d)) {
		if (!sdp_iter_init(&it2, &d) || !sdp_iter_next(&it2, &u))
			continue;
		if (!sdp_de_uuid32(&u, &uuid) || uuid != SDP_UUID_L2CAP)
			continue;
		if (sdp_iter_next(&it2, &psm) && sdp_de_uint(&psm, &v))
			return ((uint16_t)v);
	}
	return (0);
}

static void
read_hid(const struct sdp_de *rec, struct sdp_hid_info *h)
{
	struct sdp_de val, d, type, bytes;
	struct sdp_iter it, it2;
	uint64_t v;

	h->present = true;
	h->control_psm = 0x0011;
	h->interrupt_psm = 0x0013;
	if (rec_attr(rec, SDP_ATTR_PROTOCOL_DESC_LIST, &val) &&
	    (v = proto_psm(&val, false)) != 0)
		h->control_psm = (uint16_t)v;
	if (rec_attr(rec, SDP_ATTR_ADD_PROTOCOL_DESC_LIST, &val) &&
	    (v = proto_psm(&val, true)) != 0)
		h->interrupt_psm = (uint16_t)v;

	if (rec_uint(rec, SDP_ATTR_HID_DEVICE_RELEASE, &v))
		h->device_release = (uint16_t)v;
	if (rec_uint(rec, SDP_ATTR_HID_PARSER_VERSION, &v))
		h->parser_version = (uint16_t)v;
	if (rec_uint(rec, SDP_ATTR_HID_DEVICE_SUBCLASS, &v))
		h->subclass = (uint8_t)v;
	if (rec_uint(rec, SDP_ATTR_HID_COUNTRY_CODE, &v))
		h->country_code = (uint8_t)v;
	if (rec_uint(rec, SDP_ATTR_HID_VIRTUAL_CABLE, &v))
		h->virtual_cable = v != 0;
	if (rec_uint(rec, SDP_ATTR_HID_RECONNECT_INITIATE, &v))
		h->reconnect_initiate = v != 0;
	if (rec_uint(rec, SDP_ATTR_HID_BATTERY_POWER, &v))
		h->battery_power = v != 0;
	if (rec_uint(rec, SDP_ATTR_HID_REMOTE_WAKE, &v))
		h->remote_wake = v != 0;
	if (rec_uint(rec, SDP_ATTR_HID_PROFILE_VERSION, &v))
		h->profile_version = (uint16_t)v;
	if (rec_uint(rec, SDP_ATTR_HID_SUPERVISION_TIMEOUT, &v))
		h->supervision_timeout = (uint16_t)v;
	if (rec_uint(rec, SDP_ATTR_HID_NORMALLY_CONNECTABLE, &v))
		h->normally_connectable = v != 0;
	if (rec_uint(rec, SDP_ATTR_HID_BOOT_DEVICE, &v))
		h->boot_device = v != 0;

	/* HIDDescriptorList: { { uint8 type, text bytes }, ... }. */
	if (!rec_attr(rec, SDP_ATTR_HID_DESCRIPTOR_LIST, &val) ||
	    !sdp_iter_init(&it, &val))
		return;
	while (sdp_iter_next(&it, &d)) {
		if (!sdp_iter_init(&it2, &d) || !sdp_iter_next(&it2, &type) ||
		    !sdp_de_uint(&type, &v) || v != SDP_HID_DESC_TYPE_REPORT ||
		    !sdp_iter_next(&it2, &bytes) || bytes.type != SDP_DE_TEXT)
			continue;
		if (bytes.len == 0 || bytes.len > BT_HID_DESC_MAX) {
			xlog("HID report descriptor of %u bytes: not cached",
			    (unsigned)bytes.len);
			return;
		}
		(void)memcpy(h->desc, bytes.val, bytes.len);
		h->desc_len = (uint16_t)bytes.len;
		return;
	}
}

/* PnP Information; returns true if this record is the primary one. */
static bool
read_pnp(const struct sdp_de *rec, struct bt_pnp_id *p)
{
	uint64_t v;

	(void)memset(p, 0, sizeof(*p));
	p->vendor_id_source = 1;	/* the default when absent */
	if (rec_uint(rec, SDP_ATTR_PNP_VENDOR_ID_SOURCE, &v))
		p->vendor_id_source = (uint8_t)v;
	if (rec_uint(rec, SDP_ATTR_PNP_VENDOR_ID, &v))
		p->vendor_id = (uint16_t)v;
	if (rec_uint(rec, SDP_ATTR_PNP_PRODUCT_ID, &v))
		p->product_id = (uint16_t)v;
	if (rec_uint(rec, SDP_ATTR_PNP_VERSION, &v))
		p->version = (uint16_t)v;
	return (rec_uint(rec, SDP_ATTR_PNP_PRIMARY_RECORD, &v) && v != 0);
}

static uint32_t
class_profile(uint32_t c)
{
	switch (c) {
	case SDP_UUID_HID:			return (BT_PROFILE_HID);
	case SDP_UUID_AUDIO_SINK:		return (BT_PROFILE_A2DP_SINK);
	case SDP_UUID_AUDIO_SOURCE:		return (BT_PROFILE_A2DP_SOURCE);
	case SDP_UUID_AV_REMOTE:
	case SDP_UUID_AV_REMOTE_TARGET:
	case SDP_UUID_AV_REMOTE_CONTROLLER:	return (BT_PROFILE_AVRCP);
	case SDP_UUID_HANDSFREE:		return (BT_PROFILE_HFP);
	default:				return (0);
	}
}

/* Fold every record of an SSA answer into `out`. */
static void
read_records(const struct sdp_de *outer, struct sdp_device_info *out)
{
	struct sdp_iter it;
	struct sdp_de rec;
	uint32_t classes[8];
	bool have_primary = false;
	int i, n;

	(void)sdp_iter_init(&it, outer);
	while (sdp_iter_next(&it, &rec)) {
		if (rec.type != SDP_DE_SEQ)
			continue;
		out->nrecords++;
		n = rec_classes(&rec, classes, 8);
		for (i = 0; i < n; i++) {
			out->profiles |= class_profile(classes[i]);
			if (classes[i] == SDP_UUID_HID && !out->hid.present)
				read_hid(&rec, &out->hid);
			if (classes[i] == SDP_UUID_PNP_INFORMATION &&
			    !have_primary) {
				have_primary = read_pnp(&rec, &out->pnp);
				out->have_pnp = true;
			}
		}
	}
}

int
sdp_client_query(const struct bt_addr *peer, struct sdp_device_info *out)
{
	/*
	 * Every record that uses L2CAP, with only what bluetoothd keeps:
	 * classes, protocol lists (the HID PSMs) and the profile-specific
	 * 0x0200-0x0210 block, which holds both the HID and the PnP
	 * attributes.
	 */
	static const uint16_t all[] = { SDP_UUID_L2CAP };
	static const struct sdp_qrange want[] = {
		{ SDP_ATTR_SERVICE_CLASS_ID_LIST, SDP_ATTR_SERVICE_CLASS_ID_LIST },
		{ SDP_ATTR_PROTOCOL_DESC_LIST, SDP_ATTR_PROTOCOL_DESC_LIST },
		{ SDP_ATTR_ADD_PROTOCOL_DESC_LIST,
		  SDP_ATTR_ADD_PROTOCOL_DESC_LIST },
		{ 0x0200, 0x0210 },
	};
	static const struct sdp_qrange every[] = { { 0x0000, 0xffff } };
	static const uint16_t hid[] = { SDP_UUID_HID };
	static const uint16_t pnp[] = { SDP_UUID_PNP_INFORMATION };
	struct sdp_wbuf ans;
	struct sdp_de outer;
	char disp[BT_ADDR_STRLEN];
	int err;

	(void)memset(out, 0, sizeof(*out));
	(void)memset(&ans, 0, sizeof(ans));
	(void)bt_addr_ntoa(peer, BT_ADDR_FMT_DISPLAY, disp, sizeof(disp));

	err = query(peer, all, 1, want, 4, &ans, &outer);
	if (err == 0) {
		read_records(&outer, out);
		goto done;
	}
	if (err != EPROTO)
		goto done;

	/*
	 * The peer's server choked on the combined query. Ask for the two
	 * records bluetoothd caches by class, as bthidcontrol did; Profiles
	 * then only knows HID.
	 */
	xlog("%s: combined SDP query failed; asking for HID and PnP alone",
	    disp);
	ans.len = 0;
	ans.err = false;
	err = query(peer, hid, 1, every, 1, &ans, &outer);
	if (err == 0)
		read_records(&outer, out);
	else if (err != EPROTO)
		goto done;
	ans.len = 0;
	ans.err = false;
	if (query(peer, pnp, 1, every, 1, &ans, &outer) == 0)
		read_records(&outer, out);
	err = (out->nrecords > 0) ? 0 : EPROTO;

done:
	sdp_wbuf_free(&ans);
	if (err != 0) {
		xlog("%s: SDP query failed: %s", disp, strerror(err));
		return (err);
	}
	{
		char prof[128];

		bt_profiles_format(out->profiles, prof, sizeof(prof));
		xlog("%s: SDP: %d record(s), profiles [%s]%s%s", disp,
		    out->nrecords, prof, out->hid.desc_len != 0 ?
		    ", HID descriptor" : "", out->have_pnp ? ", PnP ID" : "");
	}
	return (0);
}

int
sdp_client_fetch_hid(const struct bt_addr *peer, struct sdp_hid_info *out)
{
	struct sdp_device_info *info;
	int err;

	if ((info = malloc(sizeof(*info))) == NULL)
		return (ENOMEM);
	err = sdp_client_query(peer, info);
	if (err == 0)
		*out = info->hid;
	free(info);
	return (err);
}

int
sdp_client_fetch_pnp(const struct bt_addr *peer, struct bt_pnp_id *out)
{
	struct sdp_device_info *info;
	int err;

	if ((info = malloc(sizeof(*info))) == NULL)
		return (ENOMEM);
	err = sdp_client_query(peer, info);
	if (err == 0 && !info->have_pnp)
		err = ENOENT;
	if (err == 0)
		*out = info->pnp;
	free(info);
	return (err);
}

int
sdp_client_detect_profiles(const struct bt_addr *peer, uint32_t *profiles)
{
	struct sdp_device_info *info;
	int err;

	if ((info = malloc(sizeof(*info))) == NULL)
		return (ENOMEM);
	err = sdp_client_query(peer, info);
	if (err == 0)
		*profiles = info->profiles;
	free(info);
	return (err);
}

void
sdp_client_apply(const struct sdp_device_info *info, struct bt_bond *b)
{
	const uint32_t classic = BT_PROFILE_HID | BT_PROFILE_A2DP_SINK |
	    BT_PROFILE_A2DP_SOURCE | BT_PROFILE_AVRCP | BT_PROFILE_HFP;

	b->profiles = (b->profiles & ~classic) | (info->profiles & classic);
	if (info->have_pnp) {
		b->pnp = info->pnp;
		b->flags |= BT_BOND_PNP_ID;
	}
	if (info->hid.desc_len != 0) {
		(void)memcpy(b->hid_desc, info->hid.desc, info->hid.desc_len);
		b->hid_desc_len = info->hid.desc_len;
		b->flags |= BT_BOND_HID_DESC;
	}
}

int
sdp_client_update_bond(const struct bt_addr *peer, struct bt_bond *b)
{
	struct sdp_device_info *info;
	int err;

	if ((info = malloc(sizeof(*info))) == NULL)
		return (ENOMEM);
	err = sdp_client_query(peer, info);
	if (err == 0)
		sdp_client_apply(info, b);
	free(info);
	return (err);
}

static void *
refresh_thread(void *arg)
{
	struct bt_addr *peer = arg;
	struct sdp_device_info *info;
	struct bt_bond *b;
	char disp[BT_ADDR_STRLEN];

	(void)bt_addr_ntoa(peer, BT_ADDR_FMT_DISPLAY, disp, sizeof(disp));
	info = malloc(sizeof(*info));
	b = malloc(sizeof(*b));
	if (info == NULL || b == NULL)
		goto out;
	if (sdp_client_query(peer, info) != 0)
		goto out;

	/* Serialised with the main loop, which saves bonds under the lock. */
	bluetoothd_lock();
	if (bt_store_bond_find(peer, true, b)) {
		sdp_client_apply(info, b);
		if (bt_store_bond_save(b) != 0)
			xlog("%s: could not save the SDP results", disp);
	} else
		xlog("%s: no longer paired; SDP results dropped", disp);
	bluetoothd_unlock();
out:
	if (b != NULL)
		(void)memset(b, 0, sizeof(*b));	/* it held keys */
	free(b);
	free(info);
	free(peer);
	return (NULL);
}

int
sdp_client_refresh_bond(const struct bt_addr *peer)
{
	pthread_attr_t attr;
	pthread_t t;
	struct bt_addr *copy;
	int rc;

	if ((copy = malloc(sizeof(*copy))) == NULL)
		return (ENOMEM);
	*copy = *peer;
	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rc = pthread_create(&t, &attr, refresh_thread, copy);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0)
		free(copy);
	return (rc);
}

/* ------------------------------------------------------------------ */
/* Browse: every record, as text                                       */
/* ------------------------------------------------------------------ */

static pthread_mutex_t	g_browse_lock = PTHREAD_MUTEX_INITIALIZER;
static BTSDPLine	*g_lines;
static int		g_nlines;
static int		g_lines_cap;

/* A 2-space indent per level, then the text. */
static void
emit(int indent, const char *fmt, ...)
{
	char text[BT_SDP_LINE_MAX];
	va_list ap;

	if (g_nlines == SDP_BROWSE_MAX_LINES)
		return;
	if (g_nlines == g_lines_cap) {
		int ncap = g_lines_cap != 0 ? g_lines_cap * 2 : 128;
		BTSDPLine *nl = realloc(g_lines, sizeof(BTSDPLine) * ncap);

		if (nl == NULL)
			return;
		g_lines = nl;
		g_lines_cap = ncap;
	}
	va_start(ap, fmt);
	(void)vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	(void)snprintf(g_lines[g_nlines++], BT_SDP_LINE_MAX, "%*s%s",
	    indent * 2, "", text);
}

static bool
cat(char *buf, size_t len, size_t *off, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*off >= len)
		return (false);
	va_start(ap, fmt);
	n = vsnprintf(buf + *off, len - *off, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= len - *off)
		return (false);
	*off += (size_t)n;
	return (true);
}

static bool
printable(const struct sdp_de *de)
{
	uint32_t i;

	for (i = 0; i < de->len; i++) {
		/* A trailing NUL is common in device names. */
		if (de->val[i] == '\0' && i == de->len - 1)
			break;
		if (!isprint(de->val[i]))
			return (false);
	}
	return (true);
}

static uint32_t
text_len(const struct sdp_de *de)
{
	if (de->len > 0 && de->val[de->len - 1] == '\0')
		return (de->len - 1);
	return (de->len);
}

/*
 * Render `de` on one line into buf. False when it does not fit or holds
 * something too big to inline (long binary).
 */
static bool
compact(const struct sdp_de *de, char *buf, size_t len, size_t *off)
{
	struct sdp_iter it;
	struct sdp_de m;
	uint64_t v;
	uint32_t u, i;
	uint8_t u128[16];
	const char *name;
	bool first = true;

	switch (de->type) {
	case SDP_DE_NIL:
		return (cat(buf, len, off, "nil"));
	case SDP_DE_BOOL:
		return (cat(buf, len, off, "%s",
		    de->len == 1 && de->val[0] ? "true" : "false"));
	case SDP_DE_UINT:
		if (sdp_de_uint(de, &v))
			return (cat(buf, len, off, "0x%0*llx", (int)de->len * 2,
			    (unsigned long long)v));
		goto hex;
	case SDP_DE_INT:
		if (de->len <= 8) {
			int64_t s = 0;

			for (i = 0; i < de->len; i++)
				s = (int64_t)((uint64_t)s << 8 | de->val[i]);
			if (de->len < 8 && (de->val[0] & 0x80) != 0)
				s -= (int64_t)1 << (de->len * 8);
			return (cat(buf, len, off, "%lld", (long long)s));
		}
		goto hex;
	case SDP_DE_UUID:
		if (sdp_de_uuid32(de, &u)) {
			name = sdp_uuid_name(u);
			return (cat(buf, len, off, u > 0xffff ? "0x%08x%s%s%s" :
			    "0x%04x%s%s%s", u, name ? " (" : "",
			    name ? name : "", name ? ")" : ""));
		}
		if (!sdp_de_uuid128(de, u128))
			return (false);
		return (cat(buf, len, off,
		    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		    "%02x%02x%02x%02x%02x%02x", u128[0], u128[1], u128[2],
		    u128[3], u128[4], u128[5], u128[6], u128[7], u128[8],
		    u128[9], u128[10], u128[11], u128[12], u128[13], u128[14],
		    u128[15]));
	case SDP_DE_TEXT:
	case SDP_DE_URL:
		if (printable(de))
			return (de->len <= 120 && cat(buf, len, off, "\"%.*s\"",
			    (int)text_len(de), (const char *)de->val));
		if (de->len > 24)
			return (false);
		goto hex;
	case SDP_DE_SEQ:
	case SDP_DE_ALT:
		if (!cat(buf, len, off, de->type == SDP_DE_SEQ ? "{" : "alt{"))
			return (false);
		(void)sdp_iter_init(&it, de);
		while (sdp_iter_next(&it, &m)) {
			if (!cat(buf, len, off, first ? " " : ", ") ||
			    !compact(&m, buf, len, off))
				return (false);
			first = false;
		}
		return (cat(buf, len, off, first ? "}" : " }"));
	default:
		return (false);
	}
hex:
	if (!cat(buf, len, off, "<"))
		return (false);
	for (i = 0; i < de->len; i++) {
		if (!cat(buf, len, off, "%02x", de->val[i]))
			return (false);
	}
	return (cat(buf, len, off, ">"));
}

/* Render `de` over as many lines as it needs. */
static void
render(const struct sdp_de *de, int indent)
{
	char line[BT_SDP_LINE_MAX];
	struct sdp_iter it;
	struct sdp_de m;
	size_t off = 0;
	uint32_t i;

	if (compact(de, line, sizeof(line) - (size_t)indent * 2 - 1, &off)) {
		emit(indent, "%s", line);
		return;
	}
	switch (de->type) {
	case SDP_DE_SEQ:
	case SDP_DE_ALT:
		emit(indent, de->type == SDP_DE_SEQ ? "{" : "alt{");
		(void)sdp_iter_init(&it, de);
		while (sdp_iter_next(&it, &m))
			render(&m, indent + 1);
		emit(indent, "}");
		return;
	case SDP_DE_TEXT:
	case SDP_DE_URL:
		if (printable(de)) {
			uint32_t tl = text_len(de);

			for (i = 0; i < tl; i += 64)
				emit(indent, "\"%.*s\"", (int)(tl - i < 64 ?
				    tl - i : 64), (const char *)de->val + i);
			return;
		}
		/* FALLTHROUGH */
	default:
		emit(indent, "%u bytes:", (unsigned)de->len);
		for (i = 0; i < de->len; i += 32) {
			uint32_t j;

			off = 0;
			for (j = i; j < de->len && j < i + 32; j++)
				(void)cat(line, sizeof(line), &off, "%02x ",
				    de->val[j]);
			if (off > 0)
				line[off - 1] = '\0';
			emit(indent + 1, "%s", line);
		}
		return;
	}
}

static void
render_record(const struct sdp_de *rec)
{
	char line[BT_SDP_LINE_MAX];
	struct sdp_iter it;
	struct sdp_de val;
	uint32_t classes[4];
	uint64_t handle = 0;
	uint16_t id;
	const char *cname, *aname;
	size_t off;
	int n;

	n = rec_classes(rec, classes, 4);
	(void)rec_uint(rec, SDP_ATTR_RECORD_HANDLE, &handle);
	cname = n > 0 ? sdp_uuid_name(classes[0]) : NULL;
	emit(0, "record 0x%08llx%s%s", (unsigned long long)handle,
	    cname != NULL ? ": " : "", cname != NULL ? cname : "");

	(void)sdp_iter_init(&it, rec);
	while (sdp_iter_attr(&it, &id, &val)) {
		aname = sdp_attr_name(id, n > 0 ? classes[0] : 0);
		off = 0;
		(void)cat(line, sizeof(line), &off, "0x%04x%s%s: ", id,
		    aname != NULL ? " " : "", aname != NULL ? aname : "");
		if (compact(&val, line, sizeof(line) - 2, &off)) {
			emit(1, "%s", line);
			continue;
		}
		emit(1, "0x%04x%s%s:", id, aname != NULL ? " " : "",
		    aname != NULL ? aname : "");
		render(&val, 2);
	}
	if (it.bad)
		emit(1, "(malformed attribute; record truncated)");
}

int
sdp_client_browse(const struct bt_addr *peer, int *nrecords, int *nlines)
{
	static const uint16_t l2cap[] = { SDP_UUID_L2CAP };
	static const uint16_t root[] = { SDP_UUID_PUBLIC_BROWSE_ROOT };
	static const struct sdp_qrange every[] = { { 0x0000, 0xffff } };
	struct sdp_wbuf ans;
	struct sdp_de outer, rec;
	struct sdp_iter it;
	int err;

	*nrecords = 0;
	*nlines = 0;
	(void)memset(&ans, 0, sizeof(ans));

	/* Every L2CAP service; a device whose records omit it, by browse root. */
	err = query(peer, l2cap, 1, every, 1, &ans, &outer);
	if (err == 0 && outer.len == 0) {
		ans.len = 0;
		ans.err = false;
		err = query(peer, root, 1, every, 1, &ans, &outer);
	}

	(void)pthread_mutex_lock(&g_browse_lock);
	g_nlines = 0;
	if (err == 0) {
		(void)sdp_iter_init(&it, &outer);
		while (sdp_iter_next(&it, &rec)) {
			if (rec.type != SDP_DE_SEQ)
				continue;
			if (*nrecords > 0)
				emit(0, "%s", "");
			render_record(&rec);
			(*nrecords)++;
		}
		if (g_nlines == SDP_BROWSE_MAX_LINES)
			emit(0, "(output truncated)");
	}
	*nlines = g_nlines;
	(void)pthread_mutex_unlock(&g_browse_lock);
	sdp_wbuf_free(&ans);
	return (err);
}

/* ---- MIG 31050-31051: bluetooth sdp <addr> ----------------------------- */

/*
 * 31050 bt_sdp_browse. Runs the remote browse on the Mach service thread: a
 * diagnostic, bounded by the connect and response timeouts. `errnum` is the
 * errno behind a failure, for the CLI's message.
 */
kern_return_t
_bt_sdp_browse(mach_port_t server, BTAddressString address, int *records,
    int *lines, int *errnum, bt_status_t *status)
{
	struct bt_addr peer;
	int nadapters, err;

	(void)server;
	*records = 0;
	*lines = 0;
	*errnum = 0;

	address[sizeof(BTAddressString) - 1] = '\0';
	if (!bt_addr_aton(address, &peer) || peer.type != BT_ADDR_PUBLIC) {
		*status = bt_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}
	bluetoothd_lock();
	nadapters = bluetoothd_adapter_count_locked();
	bluetoothd_unlock();
	if (nadapters == 0) {
		*status = bt_status_no_adapter_e;
		return (KERN_SUCCESS);
	}

	err = sdp_client_browse(&peer, records, lines);
	*errnum = err;
	switch (err) {
	case 0:
		*status = bt_status_success_e;
		break;
	case EPROTONOSUPPORT:
	case EAFNOSUPPORT:
		*status = bt_status_no_adapter_e;	/* no L2CAP stack yet */
		break;
	case EHOSTUNREACH:
	case EHOSTDOWN:
	case ETIMEDOUT:
	case ECONNREFUSED:
	case ECONNRESET:
	case ENETDOWN:
		*status = bt_status_not_connected_e;
		break;
	default:
		*status = bt_status_internal_error_e;
		break;
	}
	return (KERN_SUCCESS);
}

/* 31051 bt_sdp_browse_line — one line of the last browse, by index. */
kern_return_t
_bt_sdp_browse_line(mach_port_t server, int index, BTSDPLine line,
    bt_status_t *status)
{
	(void)server;
	(void)memset(line, 0, sizeof(BTSDPLine));

	(void)pthread_mutex_lock(&g_browse_lock);
	if (index < 0 || index >= g_nlines) {
		*status = bt_status_index_out_of_range_e;
	} else {
		(void)strlcpy(line, g_lines[index], sizeof(BTSDPLine));
		*status = bt_status_success_e;
	}
	(void)pthread_mutex_unlock(&g_browse_lock);
	return (KERN_SUCCESS);
}
