/*
 * sdp_server.c — bluetoothd's SDP server. See sdp_server.h.
 *
 * Written from the Core Specification (Vol 3 Part B §4); sdpd is not used or
 * supervised. The three request PDUs are served statelessly: each response is
 * built in full from the database, and a continuation is just an offset into
 * that response, tagged with the database state so a record change between
 * two continued requests is caught (InvalidContinuationState) instead of
 * splicing two different answers together. The database is a handful of
 * records, so rebuilding per request costs nothing.
 *
 * Our continuation state is 4 bytes: database state (low 16 bits) and offset,
 * both big-endian. Peers treat it as opaque.
 */
#include "sdp_server.h"
#include "sdp_proto.h"

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/socket.h>

#include <bitstring.h>		/* ng_btsocket.h uses bitstr_t */

#define	L2CAP_SOCKET_CHECKED	/* every sockaddr_l2cap field is set below */
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/include/ng_l2cap.h>
#include <netgraph/bluetooth/include/ng_btsocket.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Concurrent SDP clients. A peer opens one channel, asks, and closes. */
#define	SDP_MAX_CLIENTS		16

/* Request buffer: our incoming MTU is the L2CAP default, 672. */
#define	SDP_REQ_MAX		1024

/* At most this many attribute IDs or ranges in an AttributeIDList. */
#define	SDP_MAX_RANGES		64

/* Record handles. 0 is the SDP server's own record, by the spec. */
#define	SDP_HANDLE_SERVER	0x00000000u
#define	SDP_HANDLE_PNP		0x00010000u
#define	SDP_HANDLE_LOCAL(i)	(0x00010001u + (uint32_t)(i))

/* Database slots: the server record, PnP, then the optional services. */
#define	REC_SERVER		0
#define	REC_PNP			1
#define	REC_LOCAL(i)		(2 + (int)(i))
#define	NRECORDS		(2 + SDP_LOCAL_COUNT)

struct sdp_record {
	bool		on;
	uint32_t	handle;
	struct sdp_wbuf	attrs;		/* (id, value) pairs, ascending id */
};

/* ---- the database (g_db_lock) ------------------------------------------ */

static pthread_mutex_t	g_db_lock = PTHREAD_MUTEX_INITIALIZER;
static bool		g_db_ready;
static struct sdp_record g_rec[NRECORDS];
static uint32_t		g_db_state = 1;		/* ServiceDatabaseState */

static bool		g_local_on[SDP_LOCAL_COUNT];
static uint16_t		g_local_features[SDP_LOCAL_COUNT];
static uint8_t		g_hfp_channel = 13;

static struct {
	uint8_t		source;
	uint16_t	vendor;
	uint16_t	product;
	uint16_t	version;
} g_pnp = { 2, 0xffff, 0x0000, 0x0100 };

/* ---- the server thread (g_run_lock) ------------------------------------ */

static pthread_mutex_t	g_run_lock = PTHREAD_MUTEX_INITIALIZER;
static bool		g_running;
static pthread_t	g_thread;
static int		g_listen = -1;
static int		g_stop[2] = { -1, -1 };

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
/* Building the local records                                          */
/* ------------------------------------------------------------------ */

static void
put_handle(struct sdp_wbuf *w, uint32_t handle)
{
	sdp_put_uint16(w, SDP_ATTR_RECORD_HANDLE);
	sdp_put_uint32(w, handle);
}

static void
put_classes(struct sdp_wbuf *w, uint16_t c1, uint16_t c2)
{
	size_t m;

	sdp_put_uint16(w, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	m = sdp_seq_begin(w);
	sdp_put_uuid16(w, c1);
	if (c2 != 0)
		sdp_put_uuid16(w, c2);
	sdp_seq_end(w, m);
}

/*
 * ProtocolDescriptorList: { { L2CAP, psm }, { proto[, version] } }. For RFCOMM
 * the second member's parameter is the uint8 server channel; for AVDTP and
 * AVCTP it is the uint16 protocol version; SDP has none.
 */
static void
put_protocols(struct sdp_wbuf *w, uint16_t psm, uint16_t proto, int param)
{
	size_t m, m1;

	sdp_put_uint16(w, SDP_ATTR_PROTOCOL_DESC_LIST);
	m = sdp_seq_begin(w);
	m1 = sdp_seq_begin(w);
	sdp_put_uuid16(w, SDP_UUID_L2CAP);
	sdp_put_uint16(w, psm);
	sdp_seq_end(w, m1);
	m1 = sdp_seq_begin(w);
	sdp_put_uuid16(w, proto);
	if (proto == SDP_UUID_RFCOMM)
		sdp_put_uint8(w, (uint8_t)param);
	else if (param >= 0)
		sdp_put_uint16(w, (uint16_t)param);
	sdp_seq_end(w, m1);
	sdp_seq_end(w, m);
}

static void
put_browse_lang(struct sdp_wbuf *w)
{
	size_t m;

	sdp_put_uint16(w, SDP_ATTR_BROWSE_GROUP_LIST);
	m = sdp_seq_begin(w);
	sdp_put_uuid16(w, SDP_UUID_PUBLIC_BROWSE_ROOT);
	sdp_seq_end(w, m);

	/* "en", UTF-8 (MIBenum 106), attributes at 0x0100. */
	sdp_put_uint16(w, SDP_ATTR_LANG_BASE_LIST);
	m = sdp_seq_begin(w);
	sdp_put_uint16(w, 0x656e);
	sdp_put_uint16(w, 106);
	sdp_put_uint16(w, SDP_ATTR_PRIMARY_LANG_BASE);
	sdp_seq_end(w, m);
}

static void
put_profile(struct sdp_wbuf *w, uint16_t profile, uint16_t version)
{
	size_t m, m1;

	sdp_put_uint16(w, SDP_ATTR_PROFILE_DESC_LIST);
	m = sdp_seq_begin(w);
	m1 = sdp_seq_begin(w);
	sdp_put_uuid16(w, profile);
	sdp_put_uint16(w, version);
	sdp_seq_end(w, m1);
	sdp_seq_end(w, m);
}

static void
put_name(struct sdp_wbuf *w, const char *name)
{
	sdp_put_uint16(w, SDP_ATTR_PRIMARY_LANG_BASE + SDP_ATTR_SERVICE_NAME);
	sdp_put_text(w, name, strlen(name));
}

static void
put_features(struct sdp_wbuf *w, uint16_t features)
{
	sdp_put_uint16(w, SDP_ATTR_SUPPORTED_FEATURES);
	sdp_put_uint16(w, features);
}

static void
build_server(struct sdp_wbuf *w)
{
	size_t m;

	put_handle(w, SDP_HANDLE_SERVER);
	put_classes(w, SDP_UUID_SDP_SERVER, 0);
	put_protocols(w, SDP_PSM, SDP_UUID_SDP, -1);
	put_browse_lang(w);
	sdp_put_uint16(w, SDP_ATTR_VERSION_NUMBER_LIST);
	m = sdp_seq_begin(w);
	sdp_put_uint16(w, 0x0100);		/* SDP 1.0 */
	sdp_seq_end(w, m);
	sdp_put_uint16(w, SDP_ATTR_DATABASE_STATE);
	sdp_put_uint32(w, g_db_state);
}

static void
build_pnp(struct sdp_wbuf *w)
{
	put_handle(w, SDP_HANDLE_PNP);
	put_classes(w, SDP_UUID_PNP_INFORMATION, 0);
	put_protocols(w, SDP_PSM, SDP_UUID_SDP, -1);
	put_browse_lang(w);
	put_profile(w, SDP_UUID_PNP_INFORMATION, 0x0103);
	put_name(w, "Device ID");
	sdp_put_uint16(w, SDP_ATTR_PNP_SPECIFICATION_ID);
	sdp_put_uint16(w, 0x0103);		/* Device ID 1.3 */
	sdp_put_uint16(w, SDP_ATTR_PNP_VENDOR_ID);
	sdp_put_uint16(w, g_pnp.vendor);
	sdp_put_uint16(w, SDP_ATTR_PNP_PRODUCT_ID);
	sdp_put_uint16(w, g_pnp.product);
	sdp_put_uint16(w, SDP_ATTR_PNP_VERSION);
	sdp_put_uint16(w, g_pnp.version);
	sdp_put_uint16(w, SDP_ATTR_PNP_PRIMARY_RECORD);
	sdp_put_bool(w, true);
	sdp_put_uint16(w, SDP_ATTR_PNP_VENDOR_ID_SOURCE);
	sdp_put_uint16(w, g_pnp.source);
}

static uint16_t
local_features(sdp_local_service_t svc)
{
	static const uint16_t defaults[SDP_LOCAL_COUNT] = {
		[SDP_LOCAL_A2DP_SINK]		= 0x0001,	/* headphone */
		[SDP_LOCAL_A2DP_SOURCE]		= 0x0001,	/* player */
		[SDP_LOCAL_AVRCP_TARGET]	= 0x0001,	/* category 1 */
		[SDP_LOCAL_AVRCP_CONTROLLER]	= 0x0002,	/* category 2 */
		[SDP_LOCAL_HFP_AG]		= 0x0000,
	};

	return (g_local_features[svc] != 0 ? g_local_features[svc]
	                                   : defaults[svc]);
}

/* Attribute IDs ascend within each record, as SDP clients expect. */
static void
build_local(struct sdp_wbuf *w, sdp_local_service_t svc)
{
	uint16_t f = local_features(svc);

	put_handle(w, SDP_HANDLE_LOCAL(svc));
	switch (svc) {
	case SDP_LOCAL_A2DP_SINK:
	case SDP_LOCAL_A2DP_SOURCE:
		put_classes(w, svc == SDP_LOCAL_A2DP_SINK ?
		    SDP_UUID_AUDIO_SINK : SDP_UUID_AUDIO_SOURCE, 0);
		put_protocols(w, SDP_UUID_AVDTP, SDP_UUID_AVDTP, 0x0103);
		put_browse_lang(w);
		put_profile(w, SDP_UUID_ADVANCED_AUDIO, 0x0103);
		put_name(w, svc == SDP_LOCAL_A2DP_SINK ? "Audio Sink"
		                                       : "Audio Source");
		break;
	case SDP_LOCAL_AVRCP_TARGET:
		put_classes(w, SDP_UUID_AV_REMOTE_TARGET, 0);
		put_protocols(w, SDP_UUID_AVCTP, SDP_UUID_AVCTP, 0x0104);
		put_browse_lang(w);
		put_profile(w, SDP_UUID_AV_REMOTE, 0x0106);
		put_name(w, "AVRCP Target");
		break;
	case SDP_LOCAL_AVRCP_CONTROLLER:
		put_classes(w, SDP_UUID_AV_REMOTE,
		    SDP_UUID_AV_REMOTE_CONTROLLER);
		put_protocols(w, SDP_UUID_AVCTP, SDP_UUID_AVCTP, 0x0104);
		put_browse_lang(w);
		put_profile(w, SDP_UUID_AV_REMOTE, 0x0106);
		put_name(w, "AVRCP Controller");
		break;
	case SDP_LOCAL_HFP_AG:
		put_classes(w, SDP_UUID_HANDSFREE_AG, SDP_UUID_GENERIC_AUDIO);
		/* RFCOMM rides L2CAP PSM 3. */
		put_protocols(w, 0x0003, SDP_UUID_RFCOMM, g_hfp_channel);
		put_browse_lang(w);
		put_profile(w, SDP_UUID_HANDSFREE, 0x0108);
		put_name(w, "Hands-Free Audio Gateway");
		sdp_put_uint16(w, SDP_ATTR_NETWORK);
		sdp_put_uint8(w, 0x01);		/* can reject a call */
		break;
	default:
		return;
	}
	put_features(w, f);
}

/* Rebuild every record. Caller holds g_db_lock. */
static int
db_rebuild_locked(void)
{
	int i;
	bool err = false;

	for (i = 0; i < NRECORDS; i++) {
		sdp_wbuf_free(&g_rec[i].attrs);
		g_rec[i].on = false;
	}

	g_rec[REC_SERVER].on = true;
	g_rec[REC_SERVER].handle = SDP_HANDLE_SERVER;
	build_server(&g_rec[REC_SERVER].attrs);

	g_rec[REC_PNP].on = true;
	g_rec[REC_PNP].handle = SDP_HANDLE_PNP;
	build_pnp(&g_rec[REC_PNP].attrs);

	for (i = 0; i < SDP_LOCAL_COUNT; i++) {
		struct sdp_record *r = &g_rec[REC_LOCAL(i)];

		r->handle = SDP_HANDLE_LOCAL(i);
		if (!g_local_on[i])
			continue;
		r->on = true;
		build_local(&r->attrs, (sdp_local_service_t)i);
	}
	for (i = 0; i < NRECORDS; i++) {
		if (g_rec[i].attrs.err) {
			g_rec[i].on = false;
			err = true;
		}
	}
	g_db_ready = true;
	return (err ? -1 : 0);
}

/* A record changed: bump ServiceDatabaseState and rebuild. */
static int
db_changed_locked(void)
{
	g_db_state++;
	if ((g_db_state & 0xffff) == 0)
		g_db_state++;		/* 0 never tags a continuation */
	return (db_rebuild_locked());
}

int
sdp_server_set_service(sdp_local_service_t svc, bool enabled,
    uint16_t features)
{
	int rc;

	if ((int)svc < 0 || svc >= SDP_LOCAL_COUNT)
		return (-1);
	(void)pthread_mutex_lock(&g_db_lock);
	g_local_on[svc] = enabled;
	g_local_features[svc] = features;
	rc = db_changed_locked();
	(void)pthread_mutex_unlock(&g_db_lock);
	return (rc);
}

bool
sdp_server_service_enabled(sdp_local_service_t svc)
{
	bool on;

	if ((int)svc < 0 || svc >= SDP_LOCAL_COUNT)
		return (false);
	(void)pthread_mutex_lock(&g_db_lock);
	on = g_local_on[svc];
	(void)pthread_mutex_unlock(&g_db_lock);
	return (on);
}

int
sdp_server_set_hfp_channel(uint8_t channel)
{
	int rc;

	if (channel < 1 || channel > 30)
		return (-1);
	(void)pthread_mutex_lock(&g_db_lock);
	g_hfp_channel = channel;
	rc = db_changed_locked();
	(void)pthread_mutex_unlock(&g_db_lock);
	return (rc);
}

int
sdp_server_set_pnp(uint8_t vendor_id_source, uint16_t vendor_id,
    uint16_t product_id, uint16_t version)
{
	int rc;

	if (vendor_id_source != 1 && vendor_id_source != 2)
		return (-1);
	(void)pthread_mutex_lock(&g_db_lock);
	g_pnp.source = vendor_id_source;
	g_pnp.vendor = vendor_id;
	g_pnp.product = product_id;
	g_pnp.version = version;
	rc = db_changed_locked();
	(void)pthread_mutex_unlock(&g_db_lock);
	return (rc);
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                     */
/* ------------------------------------------------------------------ */

struct sdp_range {
	uint16_t	lo;
	uint16_t	hi;
};

/* A parsed request. */
struct sdp_req {
	uint8_t		pdu;
	uint16_t	tid;
	uint8_t		uuids[SDP_MAX_PATTERN][16];
	int		nuuids;
	uint32_t	handle;			/* SA */
	uint16_t	max;			/* record count or byte count */
	struct sdp_range ranges[SDP_MAX_RANGES];
	int		nranges;
	uint8_t		cont[SDP_CONT_MAX];
	uint8_t		cont_len;
};

/* ServiceSearchPattern: a DES of 1..12 UUIDs. Returns bytes used, or 0. */
static size_t
parse_pattern(const uint8_t *p, const uint8_t *end, struct sdp_req *r)
{
	struct sdp_de de, u;
	struct sdp_iter it;
	size_t n;

	n = sdp_de_get(p, end, &de);
	if (n == 0 || !sdp_iter_init(&it, &de))
		return (0);
	r->nuuids = 0;
	while (sdp_iter_next(&it, &u)) {
		if (r->nuuids == SDP_MAX_PATTERN ||
		    !sdp_de_uuid128(&u, r->uuids[r->nuuids]))
			return (0);
		r->nuuids++;
	}
	if (it.bad || r->nuuids == 0)
		return (0);
	return (n);
}

/*
 * AttributeIDList: a DES of uint16 IDs and uint32 ranges (high 16 bits the
 * first ID, low 16 the last). Returns bytes used, or 0.
 */
static size_t
parse_ranges(const uint8_t *p, const uint8_t *end, struct sdp_req *r)
{
	struct sdp_de de, a;
	struct sdp_iter it;
	uint64_t v;
	size_t n;

	n = sdp_de_get(p, end, &de);
	if (n == 0 || !sdp_iter_init(&it, &de))
		return (0);
	r->nranges = 0;
	while (sdp_iter_next(&it, &a)) {
		if (r->nranges == SDP_MAX_RANGES || a.type != SDP_DE_UINT ||
		    !sdp_de_uint(&a, &v))
			return (0);
		if (a.len == 2) {
			r->ranges[r->nranges].lo = (uint16_t)v;
			r->ranges[r->nranges].hi = (uint16_t)v;
		} else if (a.len == 4) {
			r->ranges[r->nranges].lo = (uint16_t)(v >> 16);
			r->ranges[r->nranges].hi = (uint16_t)v;
			if (r->ranges[r->nranges].lo >
			    r->ranges[r->nranges].hi)
				return (0);
		} else
			return (0);
		r->nranges++;
	}
	if (it.bad || r->nranges == 0)
		return (0);
	return (n);
}

/*
 * ContinuationState: InfoLength (0..16) then that many bytes, and nothing
 * after it. Returns false on a malformed one.
 */
static bool
parse_cont(const uint8_t *p, const uint8_t *end, struct sdp_req *r)
{
	if (p >= end || p[0] > SDP_CONT_MAX || (size_t)(end - p) != 1u + p[0])
		return (false);
	r->cont_len = p[0];
	(void)memcpy(r->cont, p + 1, p[0]);
	return (true);
}

static uint16_t
be16(const uint8_t *p)
{
	return ((uint16_t)(p[0] << 8 | p[1]));
}

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3]);
}

/* Parse a whole request. Returns 0, or the SDP error code to answer with. */
static uint16_t
parse_request(const uint8_t *pdu, size_t len, struct sdp_req *r)
{
	const uint8_t *p, *end;
	size_t n;

	(void)memset(r, 0, sizeof(*r));
	if (len < SDP_PDU_HDR_LEN)
		return (SDP_ERR_INVALID_PDU_SIZE);
	r->pdu = pdu[0];
	r->tid = be16(pdu + 1);
	if (be16(pdu + 3) != len - SDP_PDU_HDR_LEN)
		return (SDP_ERR_INVALID_PDU_SIZE);
	p = pdu + SDP_PDU_HDR_LEN;
	end = pdu + len;

	switch (r->pdu) {
	case SDP_PDU_SS_REQUEST:
		if ((n = parse_pattern(p, end, r)) == 0)
			return (SDP_ERR_INVALID_SYNTAX);
		p += n;
		if (end - p < 2)
			return (SDP_ERR_INVALID_SYNTAX);
		r->max = be16(p);
		p += 2;
		if (r->max == 0)
			return (SDP_ERR_INVALID_SYNTAX);
		break;
	case SDP_PDU_SA_REQUEST:
		if (end - p < 6)
			return (SDP_ERR_INVALID_SYNTAX);
		r->handle = be32(p);
		r->max = be16(p + 4);
		p += 6;
		if ((n = parse_ranges(p, end, r)) == 0)
			return (SDP_ERR_INVALID_SYNTAX);
		p += n;
		break;
	case SDP_PDU_SSA_REQUEST:
		if ((n = parse_pattern(p, end, r)) == 0)
			return (SDP_ERR_INVALID_SYNTAX);
		p += n;
		if (end - p < 2)
			return (SDP_ERR_INVALID_SYNTAX);
		r->max = be16(p);
		p += 2;
		if ((n = parse_ranges(p, end, r)) == 0)
			return (SDP_ERR_INVALID_SYNTAX);
		p += n;
		break;
	default:
		return (SDP_ERR_INVALID_SYNTAX);
	}
	if (r->pdu != SDP_PDU_SS_REQUEST && r->max < 7)
		return (SDP_ERR_INVALID_SYNTAX);
	if (!parse_cont(p, end, r))
		return (SDP_ERR_INVALID_SYNTAX);
	return (0);
}

/* ------------------------------------------------------------------ */
/* Matching and attribute selection                                    */
/* ------------------------------------------------------------------ */

/* Does `uuid` appear anywhere in the element list p..end? */
static bool
contains_uuid(const uint8_t *p, const uint8_t *end, const uint8_t uuid[16],
    int depth)
{
	struct sdp_de de;
	uint8_t u[16];
	size_t n;

	if (depth > 8)
		return (false);
	while (p < end && (n = sdp_de_get(p, end, &de)) != 0) {
		if (de.type == SDP_DE_UUID) {
			if (sdp_de_uuid128(&de, u) && memcmp(u, uuid, 16) == 0)
				return (true);
		} else if (de.type == SDP_DE_SEQ || de.type == SDP_DE_ALT) {
			if (contains_uuid(de.val, de.val + de.len, uuid,
			    depth + 1))
				return (true);
		}
		p += n;
	}
	return (false);
}

static bool
record_matches(const struct sdp_record *rec, const struct sdp_req *r)
{
	const uint8_t *b = rec->attrs.b, *e = rec->attrs.b + rec->attrs.len;
	int i;

	for (i = 0; i < r->nuuids; i++) {
		if (!contains_uuid(b, e, r->uuids[i], 0))
			return (false);
	}
	return (true);
}

static bool
id_wanted(const struct sdp_req *r, uint16_t id)
{
	int i;

	for (i = 0; i < r->nranges; i++) {
		if (id >= r->ranges[i].lo && id <= r->ranges[i].hi)
			return (true);
	}
	return (false);
}

/* Append DES { the record's wanted (id, value) pairs } to w. */
static void
put_record_attrs(struct sdp_wbuf *w, const struct sdp_record *rec,
    const struct sdp_req *r)
{
	struct sdp_de whole, val;
	struct sdp_iter it;
	uint16_t id;
	size_t m;

	whole.type = SDP_DE_SEQ;
	whole.val = rec->attrs.b;
	whole.len = (uint32_t)rec->attrs.len;
	(void)sdp_iter_init(&it, &whole);

	m = sdp_seq_begin(w);
	while (sdp_iter_attr(&it, &id, &val)) {
		if (!id_wanted(r, id))
			continue;
		sdp_put_uint16(w, id);
		sdp_put_de(w, &val);
	}
	sdp_seq_end(w, m);
}

/* ------------------------------------------------------------------ */
/* Responses                                                           */
/* ------------------------------------------------------------------ */

static void
put_pdu_hdr(struct sdp_wbuf *w, uint8_t pdu, uint16_t tid)
{
	sdp_put_raw(w, &pdu, 1);
	sdp_put_be16(w, tid);
	sdp_put_be16(w, 0);		/* patched by finish_pdu */
}

static void
finish_pdu(struct sdp_wbuf *w)
{
	size_t plen;

	if (w->err)
		return;
	plen = w->len - SDP_PDU_HDR_LEN;
	w->b[3] = (uint8_t)(plen >> 8);
	w->b[4] = (uint8_t)plen;
}

static void
error_pdu(struct sdp_wbuf *w, uint16_t tid, uint16_t code)
{
	w->len = 0;
	w->err = false;
	put_pdu_hdr(w, SDP_PDU_ERROR_RESPONSE, tid);
	sdp_put_be16(w, code);
	finish_pdu(w);
}

static void
put_cont(struct sdp_wbuf *w, uint32_t offset)
{
	uint8_t c[5];

	c[0] = 4;
	c[1] = (uint8_t)(g_db_state >> 8);
	c[2] = (uint8_t)g_db_state;
	c[3] = (uint8_t)(offset >> 8);
	c[4] = (uint8_t)offset;
	sdp_put_raw(w, c, sizeof(c));
}

/*
 * The offset a continuation asks to resume from. 0 for none. Returns false
 * for a continuation we did not issue or that a database change invalidated.
 */
static bool
get_cont(const struct sdp_req *r, uint32_t *offset)
{
	*offset = 0;
	if (r->cont_len == 0)
		return (true);
	if (r->cont_len != 4 || be16(r->cont) != (uint16_t)g_db_state)
		return (false);
	*offset = be16(r->cont + 2);
	return (*offset != 0);
}

/* ServiceSearch. Caller holds g_db_lock. */
static uint16_t
do_service_search(const struct sdp_req *r, uint16_t omtu, struct sdp_wbuf *w)
{
	uint32_t handles[NRECORDS];
	uint32_t start, per, cur, total = 0, i;

	for (i = 0; i < NRECORDS; i++) {
		if (g_rec[i].on && record_matches(&g_rec[i], r))
			handles[total++] = g_rec[i].handle;
	}
	if (total > r->max)
		total = r->max;
	if (!get_cont(r, &start) || (start != 0 && start >= total))
		return (SDP_ERR_INVALID_CONTINUATION);

	/* Header, two counts, the continuation: the rest is handles. */
	per = (uint32_t)(omtu - SDP_PDU_HDR_LEN - 4 - 5) / 4;
	cur = total - start;
	if (cur > per)
		cur = per;

	put_pdu_hdr(w, SDP_PDU_SS_RESPONSE, r->tid);
	sdp_put_be16(w, (uint16_t)total);
	sdp_put_be16(w, (uint16_t)cur);
	for (i = 0; i < cur; i++)
		sdp_put_be32(w, handles[start + i]);
	if (start + cur < total)
		put_cont(w, start + cur);
	else
		sdp_put_raw(w, "", 1);		/* InfoLength 0 */
	finish_pdu(w);
	return (0);
}

/*
 * ServiceAttribute and ServiceSearchAttribute. Both build the complete
 * attribute list(s), then send the slice the continuation asks for, no larger
 * than the peer's MaximumAttributeByteCount or MTU. Caller holds g_db_lock.
 */
static uint16_t
do_attributes(const struct sdp_req *r, uint16_t omtu, struct sdp_wbuf *w)
{
	struct sdp_wbuf full;
	uint32_t start, room, chunk;
	uint16_t err = 0;
	int i;

	(void)memset(&full, 0, sizeof(full));
	if (r->pdu == SDP_PDU_SA_REQUEST) {
		for (i = 0; i < NRECORDS; i++) {
			if (g_rec[i].on && g_rec[i].handle == r->handle)
				break;
		}
		if (i == NRECORDS)
			return (SDP_ERR_INVALID_HANDLE);
		put_record_attrs(&full, &g_rec[i], r);
	} else {
		size_t m = sdp_seq_begin(&full);

		for (i = 0; i < NRECORDS; i++) {
			if (g_rec[i].on && record_matches(&g_rec[i], r))
				put_record_attrs(&full, &g_rec[i], r);
		}
		sdp_seq_end(&full, m);
	}
	if (full.err || full.len > 0xffff) {
		err = SDP_ERR_INSUFFICIENT_RESOURCES;
		goto out;
	}
	if (!get_cont(r, &start) || (start != 0 && start >= full.len)) {
		err = SDP_ERR_INVALID_CONTINUATION;
		goto out;
	}

	/* Header, the byte count, and room for our 4-byte continuation. */
	room = (uint32_t)(omtu - SDP_PDU_HDR_LEN - 2 - 5);
	chunk = (uint32_t)full.len - start;
	if (chunk > r->max)
		chunk = r->max;
	if (chunk > room)
		chunk = room;

	put_pdu_hdr(w, r->pdu == SDP_PDU_SA_REQUEST ? SDP_PDU_SA_RESPONSE :
	    SDP_PDU_SSA_RESPONSE, r->tid);
	sdp_put_be16(w, (uint16_t)chunk);
	sdp_put_raw(w, full.b + start, chunk);
	if (start + chunk < full.len)
		put_cont(w, start + chunk);
	else
		sdp_put_raw(w, "", 1);
	finish_pdu(w);
out:
	sdp_wbuf_free(&full);
	return (err);
}

/* Answer one request PDU on `fd`. */
static void
serve_pdu(int fd, uint16_t omtu, const uint8_t *pdu, size_t len)
{
	struct sdp_req *r;
	struct sdp_wbuf w;
	uint16_t err;

	r = malloc(sizeof(*r));
	if (r == NULL)
		return;
	(void)memset(&w, 0, sizeof(w));

	err = parse_request(pdu, len, r);
	if (err == 0) {
		(void)pthread_mutex_lock(&g_db_lock);
		if (!g_db_ready)
			(void)db_rebuild_locked();
		if (r->pdu == SDP_PDU_SS_REQUEST)
			err = do_service_search(r, omtu, &w);
		else
			err = do_attributes(r, omtu, &w);
		(void)pthread_mutex_unlock(&g_db_lock);
	}
	if (err == 0 && w.err)
		err = SDP_ERR_INSUFFICIENT_RESOURCES;
	if (err != 0)
		error_pdu(&w, len >= 3 ? be16(pdu + 1) : 0, err);
	if (!w.err && send(fd, w.b, w.len, 0) < 0)
		xlog("send: %s", strerror(errno));
	sdp_wbuf_free(&w);
	free(r);
}

/* ------------------------------------------------------------------ */
/* The listener thread                                                 */
/* ------------------------------------------------------------------ */

struct sdp_client {
	int		fd;
	uint16_t	omtu;
};

static void
client_accept(int lfd, struct sdp_client *c, int *nc)
{
	struct sockaddr_l2cap sa;
	socklen_t salen = sizeof(sa);
	uint16_t omtu;
	socklen_t olen = sizeof(omtu);
	int fd;

	fd = accept(lfd, (struct sockaddr *)&sa, &salen);
	if (fd < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED)
			xlog("accept: %s", strerror(errno));
		return;
	}
	if (*nc == SDP_MAX_CLIENTS) {
		xlog("%d SDP clients already connected: refusing one",
		    SDP_MAX_CLIENTS);
		(void)close(fd);
		return;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (getsockopt(fd, SOL_L2CAP, SO_L2CAP_OMTU, &omtu, &olen) != 0 ||
	    omtu < SDP_MIN_MTU)
		omtu = SDP_DEFAULT_MTU;
	c[*nc].fd = fd;
	c[*nc].omtu = omtu;
	(*nc)++;
}

static void *
server_thread(void *arg)
{
	struct sdp_client c[SDP_MAX_CLIENTS];
	struct pollfd pfd[2 + SDP_MAX_CLIENTS];
	uint8_t buf[SDP_REQ_MAX];
	int nc = 0, i;

	(void)arg;
	for (;;) {
		int nfd = 0;

		pfd[nfd].fd = g_stop[0];
		pfd[nfd++].events = POLLIN;
		pfd[nfd].fd = g_listen;
		pfd[nfd++].events = POLLIN;
		for (i = 0; i < nc; i++) {
			pfd[nfd].fd = c[i].fd;
			pfd[nfd++].events = POLLIN;
		}
		for (i = 0; i < nfd; i++)
			pfd[i].revents = 0;

		if (poll(pfd, (nfds_t)nfd, -1) < 0) {
			if (errno == EINTR)
				continue;
			xlog("poll: %s", strerror(errno));
			break;
		}
		if (pfd[0].revents != 0)
			break;			/* sdp_server_stop */

		/* Clients first: accepting reorders the table. */
		for (i = nc - 1; i >= 0; i--) {
			ssize_t n;

			if (pfd[2 + i].revents == 0)
				continue;
			n = recv(c[i].fd, buf, sizeof(buf), 0);
			if (n > 0) {
				serve_pdu(c[i].fd, c[i].omtu, buf, (size_t)n);
				continue;
			}
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			/* The peer closed the channel, or it failed. */
			(void)close(c[i].fd);
			c[i] = c[--nc];
		}
		if (pfd[1].revents & POLLIN)
			client_accept(g_listen, c, &nc);
	}
	for (i = 0; i < nc; i++)
		(void)close(c[i].fd);
	return (NULL);
}

int
sdp_server_start(void)
{
	struct sockaddr_l2cap sa;
	int fd, saved;

	(void)pthread_mutex_lock(&g_run_lock);
	if (g_running) {
		(void)pthread_mutex_unlock(&g_run_lock);
		return (0);
	}

	(void)pthread_mutex_lock(&g_db_lock);
	if (!g_db_ready)
		(void)db_rebuild_locked();
	(void)pthread_mutex_unlock(&g_db_lock);

	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		goto fail;

	(void)memset(&sa, 0, sizeof(sa));
	sa.l2cap_len = sizeof(sa);
	sa.l2cap_family = AF_BLUETOOTH;
	sa.l2cap_psm = htole16(SDP_PSM);
	(void)memcpy(&sa.l2cap_bdaddr, NG_HCI_BDADDR_ANY, sizeof(bdaddr_t));
	sa.l2cap_cid = 0;
	sa.l2cap_bdaddr_type = BDADDR_BREDR;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		/* EADDRINUSE: something else holds PSM 1 (a stray sdpd). */
		xlog("bind PSM 1: %s", strerror(errno));
		goto fail;
	}
	if (listen(fd, 8) != 0) {
		xlog("listen: %s", strerror(errno));
		goto fail;
	}
	if (pipe2(g_stop, O_CLOEXEC) != 0)
		goto fail;

	g_listen = fd;
	if (pthread_create(&g_thread, NULL, server_thread, NULL) != 0) {
		g_listen = -1;
		(void)close(g_stop[0]);
		(void)close(g_stop[1]);
		g_stop[0] = g_stop[1] = -1;
		errno = EAGAIN;
		goto fail;
	}
	g_running = true;
	(void)pthread_mutex_unlock(&g_run_lock);
	xlog("SDP server listening on L2CAP PSM 1");
	return (0);

fail:
	saved = errno;
	if (fd >= 0)
		(void)close(fd);
	(void)pthread_mutex_unlock(&g_run_lock);
	errno = saved;
	return (-1);
}

void
sdp_server_stop(void)
{
	(void)pthread_mutex_lock(&g_run_lock);
	if (!g_running) {
		(void)pthread_mutex_unlock(&g_run_lock);
		return;
	}
	(void)write(g_stop[1], "", 1);
	(void)pthread_join(g_thread, NULL);
	(void)close(g_listen);
	(void)close(g_stop[0]);
	(void)close(g_stop[1]);
	g_listen = -1;
	g_stop[0] = g_stop[1] = -1;
	g_running = false;
	(void)pthread_mutex_unlock(&g_run_lock);
}

bool
sdp_server_running(void)
{
	bool r;

	(void)pthread_mutex_lock(&g_run_lock);
	r = g_running;
	(void)pthread_mutex_unlock(&g_run_lock);
	return (r);
}
