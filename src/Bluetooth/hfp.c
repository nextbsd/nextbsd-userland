/*
 * hfp.c — the Hands-Free Profile Audio Gateway. See hfp.h.
 *
 * Section numbers are HFP 1.8's. The AG side of the service level
 * connection (4.2), codec connection (4.11.3) and audio connection (4.11)
 * procedures, with no calls: a computer is not a phone, the headset is
 * only asked for its microphone and earpiece.
 */
#include "hfp.h"
#include "adapter.h"
#include "bluetoothd.h"
#include "btstore.h"
#include "hci_compat.h"
#include "ng_btsocket_sco_sync.h"
#include "sco_bridge.h"
#include "sdp_client.h"
#include "sdp_proto.h"
#include "sdp_server.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	HFP_MAX		SCO_BRIDGE_MAX	/* session i uses bridge i */
#define	LINE_MAX_AT	256

/* AT+BRSF: the headset's features (4.34.2) and ours (4.34.1). */
#define	HF_FEAT_3WAY		(1u << 1)
#define	HF_FEAT_CODEC_NEG	(1u << 7)
#define	HF_FEAT_ESCO_S4		(1u << 9)
#define	AG_FEAT_CODEC_NEG	(1u << 9)
#define	AG_FEAT_ESCO_S4		(1u << 11)
#define	AG_FEATURES		(AG_FEAT_CODEC_NEG | AG_FEAT_ESCO_S4)

/* The AG record's SupportedFeatures (5.3): wide band speech. */
#define	SDP_AG_WIDE_BAND	0x0020

/*
 * The indicators (4.34.2, +CIND): in service, no call, full signal, at
 * home, full battery. The order is the one AT+CIND=? reports.
 */
#define	CIND_TEST	"+CIND: (\"service\",(0,1)),(\"call\",(0,1))," \
			"(\"callsetup\",(0-3)),(\"callheld\",(0-2))," \
			"(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))"
#define	CIND_READ	"+CIND: 1,0,0,0,5,0,5"

struct hfp_sess {
	bool			used;
	int			slot;		/* the SCO bridge */
	struct bt_addr		peer;
	char			addr[BT_ADDR_STRLEN];
	uint32_t		gen;		/* matches its timers */
	bool			link;		/* ACL up, as audio.c says */

	/* The RFCOMM channel. */
	int			fd;
	dispatch_source_t	src;
	bool			up;
	bool			connecting;	/* our connect in flight */
	bool			cancelled;	/* ...and no longer wanted */
	char			in[LINE_MAX_AT];
	int			in_len;

	/* The service level connection. */
	uint32_t		hf_features;
	bool			slc;
	bool			msbc_ok;	/* AT+BAC listed mSBC */
	bool			msbc_failed;	/* ...and it did not work */
	bool			cvsd_only;	/* asked for: CVSD this session */

	/* Codec and audio. */
	int			codec_pending;	/* +BCS sent, awaiting AT+BCS */
	uint32_t		codec_gen;
	sco_codec_t		codec;
	bool			want_audio;
	bool			sco_connecting;
	uint32_t		sco_gen;
	bool			sco_up;
	bool			routed;		/* the mixer points at us */
	char			dev[32];	/* the bridge */

	int			vgs;
	int			vgm;
	int			battery;
};

static dispatch_queue_t		g_q;
static pthread_once_t		g_q_once = PTHREAD_ONCE_INIT;
static struct hfp_sess		g_sess[HFP_MAX];
static int			g_rfcomm = -1;
static dispatch_source_t	g_rfcomm_src;
static int			g_sco = -1;
static dispatch_source_t	g_sco_src;
static uint32_t			g_gen;
static bool			g_down;

static void	codec_select(struct hfp_sess *s);
static void	sco_open(struct hfp_sess *s);
static void	audio_down(struct hfp_sess *s, const char *why);

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[hfp] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void
q_init(void)
{
	g_q = dispatch_queue_create("org.nextbsd.bluetoothd.hfp", NULL);
}

dispatch_queue_t
hfp_queue(void)
{
	(void)pthread_once(&g_q_once, q_init);
	return (g_q);
}

static bool
same(const struct bt_addr *a, const struct bt_addr *b)
{
	return (memcmp(a->b, b->b, BT_ADDR_LEN) == 0);
}

static const char *
codec_name(sco_codec_t c)
{
	return (c == SCO_CODEC_MSBC ? "mSBC" : "CVSD");
}

/* ---- sessions (hfp queue) ------------------------------------------------ */

static struct hfp_sess *
find(const struct bt_addr *peer)
{
	int i;

	for (i = 0; i < HFP_MAX; i++) {
		if (g_sess[i].used && same(&g_sess[i].peer, peer))
			return (&g_sess[i]);
	}
	return (NULL);
}

static struct hfp_sess *
add(const struct bt_addr *peer)
{
	struct hfp_sess *s = find(peer);
	int i;

	for (i = 0; s == NULL && i < HFP_MAX; i++) {
		if (!g_sess[i].used)
			s = &g_sess[i];
	}
	if (s == NULL || s->used)
		return (s);
	i = (int)(s - g_sess);
	(void)memset(s, 0, sizeof(*s));
	s->used = true;
	s->slot = i;
	s->fd = -1;
	s->peer = *peer;
	s->peer.type = BT_ADDR_PUBLIC;
	(void)bt_addr_ntoa(&s->peer, BT_ADDR_FMT_DISPLAY, s->addr,
	    sizeof(s->addr));
	s->gen = ++g_gen;
	s->vgs = s->vgm = s->battery = -1;
	return (s);
}

static void
gc(struct hfp_sess *s)
{
	if (s->link || s->up || s->connecting || s->sco_connecting)
		return;
	s->used = false;
}

/*
 * Is `peer` a headset we serve: paired, its Audio switch on, and (unless
 * the headset is the one connecting) its bond lists Handsfree.
 */
static bool
wanted(const struct bt_addr *peer, bool need_profile)
{
	struct bt_bond *b = calloc(1, sizeof(*b));
	struct bt_addr p = *peer;
	bool yes;

	if (b == NULL)
		return (false);
	p.type = BT_ADDR_PUBLIC;
	yes = bt_store_bond_find(&p, false, b) && !b->no_audio &&
	    (!need_profile || (b->profiles & BT_PROFILE_HFP) != 0);
	free(b);
	return (yes);
}

/* What audio.c publishes (Device/<addr>/Audio). */
static void
publish(struct hfp_sess *s)
{
	audio_hfp_changed(&s->peer, s->slc, s->sco_up, s->battery);
}

struct tick {
	struct bt_addr	peer;
	uint32_t	gen;
};

static void
tick_after(struct hfp_sess *s, uint32_t gen, int ms, dispatch_function_t fn)
{
	struct tick *t = malloc(sizeof(*t));

	if (t == NULL)
		return;
	t->peer = s->peer;
	t->gen = gen;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)ms * 1000000), hfp_queue(), t, fn);
}

/* ---- the AT channel ---------------------------------------------------- */

static void
send_line(struct hfp_sess *s, const char *fmt, ...)
{
	char buf[LINE_MAX_AT + 8];
	va_list ap;
	int n;

	if (s->fd < 0)
		return;
	buf[0] = '\r';
	buf[1] = '\n';
	va_start(ap, fmt);
	n = vsnprintf(buf + 2, sizeof(buf) - 4, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	n += 2;
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\r';
	buf[n++] = '\n';
	/* A few bytes on an RFCOMM channel with credits: no partial writes. */
	(void)write(s->fd, buf, (size_t)n);
}

static void
ok(struct hfp_sess *s)
{
	send_line(s, "OK");
}

static void
error(struct hfp_sess *s)
{
	send_line(s, "ERROR");
}

static void
slc_up(struct hfp_sess *s)
{
	if (s->slc)
		return;
	s->slc = true;
	xlog("%s: hands-free connected (headset features 0x%x%s)", s->addr,
	    s->hf_features, s->msbc_ok ? ", wide band speech" : "");
	publish(s);
}

/* Parse "n" or "n,m,..." into up to max ints; returns how many. */
static int
parse_ints(const char *p, int *v, int max)
{
	char *end;
	long x;
	int n = 0;

	while (n < max && *p != '\0') {
		while (*p == ' ')
			p++;
		x = strtol(p, &end, 10);
		if (end == p)
			break;
		v[n++] = (int)x;
		p = end;
		while (*p == ' ')
			p++;
		if (*p != ',')
			break;
		p++;
	}
	return (n);
}

/* One command from the headset. `cmd` is upper case up to '=' or '?'. */
static void
at_command(struct hfp_sess *s, char *line)
{
	char *arg, *p;
	int v[16], n, i;
	bool listed;

	for (p = line; *p != '\0' && *p != '=' && *p != '?'; p++)
		*p = (char)toupper((unsigned char)*p);
	arg = (*p == '=') ? p + 1 : p;

	if (strncmp(line, "AT+BRSF=", 8) == 0) {
		s->hf_features = (uint32_t)strtoul(arg, NULL, 10);
		send_line(s, "+BRSF: %u", AG_FEATURES);
		ok(s);
	} else if (strncmp(line, "AT+BAC=", 7) == 0) {
		n = parse_ints(arg, v, 16);
		for (i = 0, listed = false; i < n; i++)
			listed |= (v[i] == SCO_CODEC_MSBC);
		s->msbc_ok = listed;
		ok(s);
		/* A new list during a codec connection: choose again. */
		if (s->codec_pending != 0) {
			s->codec_pending = 0;
			codec_select(s);
		}
	} else if (strcmp(line, "AT+CIND=?") == 0) {
		send_line(s, "%s", CIND_TEST);
		ok(s);
	} else if (strcmp(line, "AT+CIND?") == 0) {
		send_line(s, "%s", CIND_READ);
		ok(s);
	} else if (strncmp(line, "AT+CMER=", 8) == 0) {
		ok(s);
		/* Without three-way calling on both sides, the SLC is up. */
		if ((s->hf_features & HF_FEAT_3WAY) == 0 ||
		    (AG_FEATURES & 1u) == 0)
			slc_up(s);
	} else if (strcmp(line, "AT+CHLD=?") == 0) {
		send_line(s, "+CHLD: (0,1,2,3)");
		ok(s);
		slc_up(s);
	} else if (strncmp(line, "AT+BCS=", 7) == 0) {
		n = parse_ints(arg, v, 1);
		if (s->codec_pending != 0 && n == 1 &&
		    v[0] == s->codec_pending) {
			ok(s);
			s->codec = (sco_codec_t)s->codec_pending;
			s->codec_pending = 0;
			s->codec_gen++;
			sco_open(s);		/* the OK has gone first */
		} else {
			error(s);
			if (s->codec_pending == SCO_CODEC_MSBC) {
				/* It will not take mSBC: CVSD. */
				s->msbc_failed = true;
				s->codec_pending = 0;
				codec_select(s);
			}
		}
	} else if (strcmp(line, "AT+BCC") == 0) {
		/* The headset asks for audio (4.11.2). */
		ok(s);
		if (!s->want_audio) {
			xlog("%s: the headset asked for audio", s->addr);
			s->want_audio = true;
			codec_select(s);
		}
	} else if (strncmp(line, "AT+VGS=", 7) == 0) {
		if (parse_ints(arg, v, 1) == 1)
			s->vgs = v[0];
		ok(s);
	} else if (strncmp(line, "AT+VGM=", 7) == 0) {
		if (parse_ints(arg, v, 1) == 1)
			s->vgm = v[0];
		ok(s);
	} else if (strncmp(line, "AT+XAPL=", 8) == 0) {
		/*
		 * Apple's accessory extension. The reply's first field is
		 * the token accessories look for, not a product claim; 2 asks
		 * the headset to report its battery (AT+IPHONEACCEV).
		 */
		send_line(s, "+XAPL=iPhone,2");
		ok(s);
	} else if (strncmp(line, "AT+IPHONEACCEV=", 15) == 0) {
		n = parse_ints(arg, v, 16);
		for (i = 1; i + 1 < n; i += 2) {
			if (v[i] == 1 && v[i + 1] >= 0 && v[i + 1] <= 9 &&
			    s->battery != (v[i + 1] + 1) * 10) {
				s->battery = (v[i + 1] + 1) * 10;
				publish(s);
			}
		}
		ok(s);
	} else if (strcmp(line, "ATA") == 0 ||
	    strncmp(line, "AT+CKPD=", 8) == 0) {
		ok(s);
		if (s->sco_up && line[2] == '+')	/* the HSP button */
			audio_down(s, "the headset's button");
		else if (!s->want_audio) {
			s->want_audio = true;
			codec_select(s);
		}
	} else if (strcmp(line, "AT+CHUP") == 0) {
		ok(s);
		if (s->want_audio)
			audio_down(s, "the headset hung up");
	} else if (strcmp(line, "AT+COPS?") == 0) {
		send_line(s, "+COPS: 0,0,\"NextBSD\"");
		ok(s);
	} else if (strncmp(line, "AT+NREC=", 8) == 0 ||
	    strncmp(line, "AT+CLIP=", 8) == 0 ||
	    strncmp(line, "AT+CCWA=", 8) == 0 ||
	    strncmp(line, "AT+CMEE=", 8) == 0 ||
	    strncmp(line, "AT+BIA=", 7) == 0 ||
	    strncmp(line, "AT+COPS=", 8) == 0 ||
	    strncmp(line, "AT+XEVENT=", 10) == 0 ||
	    strncmp(line, "AT+BIEV=", 8) == 0 ||
	    strncmp(line, "AT+BIND", 7) == 0 ||
	    strcmp(line, "AT+CLCC") == 0 ||
	    strcmp(line, "AT+CNUM") == 0 ||
	    strcmp(line, "AT+BTRH?") == 0) {
		/* Accepted; nothing to report: there are no calls. */
		ok(s);
	} else {
		xlog("%s: not supported: %s", s->addr, line);
		error(s);
	}
}

/* ---- audio: codec, SCO, the mixer ---------------------------------------- */

static bool
codec_negotiation(const struct hfp_sess *s)
{
	return ((s->hf_features & HF_FEAT_CODEC_NEG) != 0 &&
	    (AG_FEATURES & AG_FEAT_CODEC_NEG) != 0);
}

static void
codec_timeout(void *arg)
{
	struct tick *t = arg;
	struct hfp_sess *s = find(&t->peer);

	if (s != NULL && s->codec_pending != 0 && s->codec_gen == t->gen) {
		xlog("%s: no answer to +BCS:%d", s->addr, s->codec_pending);
		if (s->codec_pending == SCO_CODEC_MSBC) {
			s->msbc_failed = true;
			s->codec_pending = 0;
			codec_select(s);
		} else {
			s->codec_pending = 0;
			audio_down(s, "the headset did not confirm a codec");
		}
	}
	free(t);
}

/* Start the codec connection (4.11.3), or go straight to SCO without one. */
static void
codec_select(struct hfp_sess *s)
{
	int c;

	if (!s->slc || !s->want_audio || s->sco_up || s->sco_connecting)
		return;
	if (!codec_negotiation(s)) {
		s->codec = SCO_CODEC_CVSD;
		sco_open(s);
		return;
	}
	c = (s->msbc_ok && !s->msbc_failed && !s->cvsd_only) ?
	    SCO_CODEC_MSBC : SCO_CODEC_CVSD;
	s->codec_pending = c;
	s->codec_gen++;
	send_line(s, "+BCS: %d", c);
	tick_after(s, s->codec_gen, HFP_CODEC_WAIT_MS, codec_timeout);
}

/* The parameter sets to try for `codec` (ng_btsocket_sco_sync.h). */
struct sco_set {
	const char		*name;
	struct sco_sync_params	p;
};

static int
sco_sets(sco_codec_t codec, uint32_t hf_features, struct sco_set *out)
{
	static const struct sco_set t2 = { "T2",
	    { 8000, 8000, 0x000d, SCO_VOICE_TRANSPARENT, 0x0380, 0x02, 0 } };
	static const struct sco_set t1 = { "T1",
	    { 8000, 8000, 0x0008, SCO_VOICE_TRANSPARENT, 0x03c8, 0x02, 0 } };
	static const struct sco_set s4 = { "S4",
	    { 8000, 8000, 0x000c, SCO_VOICE_CVSD, 0x0380, 0x02, 0 } };
	static const struct sco_set s3 = { "S3",
	    { 8000, 8000, 0x000a, SCO_VOICE_CVSD, 0x0380, 0x01, 0 } };
	static const struct sco_set s1 = { "S1",
	    { 8000, 8000, 0x0007, SCO_VOICE_CVSD, 0x03c8, 0x01, 0 } };
	static const struct sco_set d1 = { "D1",
	    { 8000, 8000, 0xffff, SCO_VOICE_CVSD, 0x03c4, 0xff, 0 } };
	bool s4ok = (hf_features & HF_FEAT_ESCO_S4) != 0;
	int n = 0;

	if (codec == SCO_CODEC_MSBC) {
		if (s4ok)
			out[n++] = t2;
		out[n++] = t1;
	} else {
		if (s4ok)
			out[n++] = s4;
		out[n++] = s3;
		out[n++] = s1;
		out[n++] = d1;
	}
	return (n);
}

struct sco_job {
	struct bt_addr	peer;
	uint32_t	gen;
	sco_codec_t	codec;
	uint32_t	hf_features;
	int		fd;
	int		err;
	const char	*set;
};

static void
sco_fill(struct sockaddr_sco *sa, const void *bdaddr)
{
	(void)memset(sa, 0, sizeof(*sa));
	sa->sco_len = sizeof(*sa);
	sa->sco_family = AF_BLUETOOTH;
	(void)memcpy(&sa->sco_bdaddr, bdaddr, sizeof(bdaddr_t));
}

static void	sco_connected(void *arg);

/* A global queue: connect() waits for the controller. */
static void
sco_connect_job(void *arg)
{
	struct sco_job *j = arg;
	struct sco_set sets[4];
	struct sockaddr_sco sa;
	int i, n, fd;
	bool legacy = false;

	j->fd = -1;
	j->err = EHOSTUNREACH;
	n = sco_sets(j->codec, j->hf_features, sets);
	for (i = 0; i < n || legacy; i++) {
		fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
		    BLUETOOTH_PROTO_SCO);
		if (fd < 0) {
			j->err = errno;
			break;
		}
		sco_fill(&sa, NG_HCI_BDADDR_ANY);
		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
			j->err = errno;
			(void)close(fd);
			break;
		}
		if (!legacy && setsockopt(fd, SOL_SCO, SO_SCO_SYNC_PARAMS,
		    &sets[i].p, sizeof(sets[i].p)) != 0) {
			/*
			 * A kernel without eSCO (before NgBtsocket's
			 * nextbsd-kernel-extensions#31): CVSD can still go
			 * over a legacy SCO link; mSBC cannot.
			 */
			j->err = errno;
			(void)close(fd);
			if (j->codec != SCO_CODEC_CVSD)
				break;
			legacy = true;
			i = -1;
			n = 0;
			continue;
		}
		sco_fill(&sa, j->peer.b);
		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
			j->fd = fd;
			j->set = legacy ? "SCO" : sets[i].name;
			break;
		}
		j->err = errno;
		(void)close(fd);
		if (legacy || j->err != ECONNREFUSED)
			break;		/* only a refused set has a next one */
	}
	dispatch_async_f(hfp_queue(), j, sco_connected);
}

/* The bridge says the headset closed the SCO link. */
static void
sco_ended(void *ctx, int slot)
{
	struct hfp_sess *s = ctx;

	if (!s->used || s->slot != slot || !s->sco_up)
		return;
	s->sco_up = false;
	audio_down(s, "the headset closed the audio link");
}

/* Point the headset's mixer at our bridge. */
static bool
route_mixer(struct hfp_sess *s)
{
	bt_status_t st;

	if (sco_bridge_device(s->slot, s->dev, sizeof(s->dev)) != 0) {
		xlog("%s: no audio bridge (cuse): the microphone stays off",
		    s->addr);
		return (false);
	}
	st = audio_route_to(&s->peer, AUDIO_ROUTE_HFP, s->dev,
	    codec_name(s->codec), false);
	if (st != bt_status_success_e) {
		xlog("%s: no mixer runs for it (an A2DP headset with its "
		    "Audio switch on has one): the microphone stays off",
		    s->addr);
		return (false);
	}
	s->routed = true;
	return (true);
}

static void
sco_open(struct hfp_sess *s)
{
	struct sco_job *j;

	if (!route_mixer(s)) {
		audio_down(s, "no mixer");
		return;
	}
	j = calloc(1, sizeof(*j));
	if (j == NULL) {
		audio_down(s, "out of memory");
		return;
	}
	j->peer = s->peer;
	j->gen = ++s->sco_gen;
	j->codec = s->codec;
	j->hf_features = s->hf_features;
	s->sco_connecting = true;
	xlog("%s: opening the audio link (%s)", s->addr, codec_name(s->codec));
	dispatch_async_f(dispatch_get_global_queue(
	    DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), j, sco_connect_job);
}

static void
log_info(struct hfp_sess *s, int fd, const char *set)
{
	struct sco_sync_info si;
	socklen_t len = sizeof(si);

	if (getsockopt(fd, SOL_SCO, SO_SCO_SYNC_INFO, &si, &len) == 0)
		xlog("%s: %s link with %s (air mode %u, %u/%u-byte packets)",
		    s->addr, si.link_type == SCO_LINK_ESCO ? "eSCO" : "SCO",
		    set, si.air_mode, si.rx_pkt_len, si.tx_pkt_len);
	else
		xlog("%s: SCO link with %s", s->addr, set);
}

/* Carry SCO socket `fd` (connected) to the mixer. */
static void
sco_adopt(struct hfp_sess *s, int fd, const char *set)
{
	log_info(s, fd, set);
	if (sco_bridge_start(s->slot, fd, s->codec, hfp_queue(), sco_ended,
	    s) != 0) {
		audio_down(s, "the audio bridge did not start");
		return;
	}
	s->sco_up = true;
	xlog("%s: microphone live (%s) through %s", s->addr,
	    codec_name(s->codec), s->dev);
	publish(s);
}

static void
sco_connected(void *arg)
{
	struct sco_job *j = arg;
	struct hfp_sess *s = find(&j->peer);

	if (s == NULL || j->gen != s->sco_gen) {
		if (j->fd >= 0)
			(void)close(j->fd);
		free(j);
		return;
	}
	s->sco_connecting = false;
	if (!s->want_audio || !s->slc) {
		if (j->fd >= 0)
			(void)close(j->fd);
		free(j);
		gc(s);
		return;
	}
	if (j->fd < 0) {
		xlog("%s: the %s audio link failed: %s", s->addr,
		    codec_name(j->codec), strerror(j->err));
		if (j->codec == SCO_CODEC_MSBC) {
			s->msbc_failed = true;
			if (codec_negotiation(s))
				codec_select(s);	/* CVSD now */
			else
				audio_down(s, "no audio link");
		} else
			audio_down(s, "no audio link");
		free(j);
		return;
	}
	sco_adopt(s, j->fd, j->set);
	free(j);
}

/* Close the microphone and give the headset back to A2DP. */
static void
audio_down(struct hfp_sess *s, const char *why)
{
	bool was = s->sco_up || s->sco_connecting || s->codec_pending != 0;

	s->want_audio = false;
	s->codec_pending = 0;
	s->codec_gen++;
	if (s->sco_connecting) {
		s->sco_gen++;		/* its result is closed on arrival */
		s->sco_connecting = false;
	}
	if (s->sco_up) {
		sco_bridge_stop(s->slot);
		s->sco_up = false;
	}
	if (s->routed) {
		s->routed = false;
		(void)audio_route(&s->peer, AUDIO_ROUTE_A2DP);
	}
	if (was)
		xlog("%s: microphone off: %s", s->addr, why);
	publish(s);
}

static bt_status_t
audio_up(struct hfp_sess *s, bool cvsd_only)
{
	if (!s->slc)
		return (bt_status_not_connected_e);
	if (cvsd_only && !s->cvsd_only) {
		s->cvsd_only = true;
		if (s->sco_up && s->codec == SCO_CODEC_MSBC)
			audio_down(s, "switching to CVSD");
	}
	if (s->sco_up || s->sco_connecting || s->codec_pending != 0)
		return (bt_status_success_e);
	s->want_audio = true;
	codec_select(s);
	return (bt_status_success_e);
}

/* ---- the RFCOMM channel ------------------------------------------------ */

static void
sess_close(struct hfp_sess *s, const char *why)
{
	bool was = s->slc;

	if (s->sco_up || s->sco_connecting || s->routed ||
	    s->codec_pending != 0)
		audio_down(s, why);
	if (s->src != NULL) {
		dispatch_source_cancel(s->src);		/* closes the fd */
		dispatch_release(s->src);
		s->src = NULL;
	}
	s->fd = -1;
	if (s->up)
		xlog("%s: hands-free channel closed: %s", s->addr, why);
	s->up = false;
	s->slc = false;
	s->hf_features = 0;
	s->msbc_ok = s->msbc_failed = s->cvsd_only = false;
	s->in_len = 0;
	s->battery = s->vgs = s->vgm = -1;
	if (s->connecting)
		s->cancelled = true;
	if (was)
		publish(s);
	gc(s);
}

static void
readable(void *arg)
{
	struct hfp_sess *s = arg;
	char buf[256];
	ssize_t n;
	int i;

	if (!s->up)
		return;
	n = read(s->fd, buf, sizeof(buf));
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		sess_close(s, n == 0 ? "the headset closed it" :
		    strerror(errno));
		return;
	}
	for (i = 0; i < n && s->up; i++) {
		if (buf[i] == '\r' || buf[i] == '\n') {
			if (s->in_len == 0)
				continue;
			s->in[s->in_len] = '\0';
			s->in_len = 0;
			at_command(s, s->in);
		} else if (s->in_len < LINE_MAX_AT - 1)
			s->in[s->in_len++] = buf[i];
	}
}

static void
fd_cancelled(void *arg)
{
	(void)close((int)(intptr_t)arg);
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	return (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0);
}

/* Take RFCOMM channel `fd` as the session with `peer`. */
static void
adopt(const struct bt_addr *peer, int fd, bool incoming)
{
	struct hfp_sess *s = add(peer);

	if (s == NULL || s->up || set_nonblock(fd) != 0) {
		(void)close(fd);		/* none free, or both opened */
		return;
	}
	s->src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, hfp_queue());
	if (s->src == NULL) {
		(void)close(fd);
		gc(s);
		return;
	}
	s->fd = fd;
	s->up = true;
	s->in_len = 0;
	dispatch_set_context(s->src, s);
	dispatch_source_set_event_handler_f(s->src, readable);
	/* The cancel handler keeps the fd, not the session: it may be reused. */
	dispatch_source_set_cancel_handler(s->src, ^{
		(void)close(fd);
	});
	dispatch_resume(s->src);
	xlog("%s: hands-free channel up (%s)", s->addr,
	    incoming ? "the headset opened it" : "we opened it");
}

static void
rfcomm_fill(struct sockaddr_rfcomm *sa, const void *bdaddr, uint8_t ch)
{
	(void)memset(sa, 0, sizeof(*sa));
	sa->rfcomm_len = sizeof(*sa);
	sa->rfcomm_family = AF_BLUETOOTH;
	sa->rfcomm_channel = ch;
	(void)memcpy(&sa->rfcomm_bdaddr, bdaddr, sizeof(bdaddr_t));
}

struct conn_job {
	struct bt_addr	peer;
	int		fd;
	int		err;
	uint8_t		channel;
};

static void
connected(void *arg)
{
	struct conn_job *j = arg;
	struct hfp_sess *s = find(&j->peer);

	if (s != NULL) {
		s->connecting = false;
		if (s->cancelled || !s->link) {
			s->cancelled = false;
			if (j->fd >= 0)
				(void)close(j->fd);
			gc(s);
			free(j);
			return;
		}
	}
	if (j->fd < 0) {
		if (s != NULL) {
			xlog("%s: cannot open the hands-free channel: %s",
			    s->addr, strerror(j->err));
			gc(s);
		}
	} else if (s == NULL)
		(void)close(j->fd);
	else
		adopt(&j->peer, j->fd, false);
	free(j);
}

/* A global queue: the SDP query and connect() block. */
static void
connect_job(void *arg)
{
	struct conn_job *j = arg;
	struct sockaddr_rfcomm sa;
	uint16_t features;
	int fd;

	j->fd = -1;
	j->err = sdp_client_rfcomm_service(&j->peer, SDP_UUID_HANDSFREE,
	    &j->channel, &features);
	if (j->err != 0)
		goto done;
	fd = socket(PF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_RFCOMM);
	if (fd < 0) {
		j->err = errno;
		goto done;
	}
	rfcomm_fill(&sa, NG_HCI_BDADDR_ANY, 0);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		j->err = errno;
		(void)close(fd);
		goto done;
	}
	rfcomm_fill(&sa, j->peer.b, j->channel);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		j->err = errno;
		(void)close(fd);
		goto done;
	}
	j->fd = fd;
done:
	dispatch_async_f(hfp_queue(), j, connected);
}

/* The headset had its turn: open the channel ourselves. */
static void
connect_check(void *arg)
{
	struct tick *t = arg;
	struct hfp_sess *s = find(&t->peer);
	struct conn_job *j;

	if (s == NULL || s->gen != t->gen || !s->link || s->up ||
	    s->connecting || g_down || !wanted(&s->peer, true)) {
		free(t);
		return;
	}
	free(t);
	j = calloc(1, sizeof(*j));
	if (j == NULL)
		return;
	j->peer = s->peer;
	s->connecting = true;
	s->cancelled = false;
	xlog("%s: opening the hands-free channel", s->addr);
	dispatch_async_f(dispatch_get_global_queue(
	    DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), j, connect_job);
}

/* ---- the listeners ------------------------------------------------------ */

static void
rfcomm_readable(void *arg)
{
	struct sockaddr_rfcomm sa;
	struct bt_addr peer;
	char addr[BT_ADDR_STRLEN];
	socklen_t len = sizeof(sa);
	int fd;

	if (g_rfcomm != (int)(intptr_t)arg)
		return;
	fd = accept(g_rfcomm, (struct sockaddr *)&sa, &len);
	if (fd < 0)
		return;
	(void)memset(&peer, 0, sizeof(peer));
	(void)memcpy(peer.b, &sa.rfcomm_bdaddr, BT_ADDR_LEN);
	peer.type = BT_ADDR_PUBLIC;
	if (g_down || !wanted(&peer, false)) {
		xlog("%s: hands-free from a device that is not paired, or "
		    "has audio off; refused", bt_addr_ntoa(&peer,
		    BT_ADDR_FMT_DISPLAY, addr, sizeof(addr)));
		(void)close(fd);
		return;
	}
	adopt(&peer, fd, true);
}

static void
sco_readable(void *arg)
{
	struct sockaddr_sco sa;
	struct bt_addr peer;
	struct hfp_sess *s;
	socklen_t len = sizeof(sa);
	int fd;

	if (g_sco != (int)(intptr_t)arg)
		return;
	fd = accept(g_sco, (struct sockaddr *)&sa, &len);
	if (fd < 0)
		return;
	(void)memset(&peer, 0, sizeof(peer));
	(void)memcpy(peer.b, &sa.sco_bdaddr, BT_ADDR_LEN);
	s = find(&peer);
	if (s == NULL || !s->slc || s->sco_up || s->sco_connecting) {
		(void)close(fd);	/* not ours, or ours crossed it */
		return;
	}
	/* The listener sets no parameters: the kernel accepted CVSD. */
	xlog("%s: the headset opened the audio link", s->addr);
	s->codec_pending = 0;
	s->codec = SCO_CODEC_CVSD;
	s->want_audio = true;
	if (!route_mixer(s)) {
		(void)close(fd);
		audio_down(s, "no mixer");
		return;
	}
	sco_adopt(s, fd, "the headset's");
}

/* Open one listener; returns its fd or -1. */
static int
listen_on(int proto, int type, struct sockaddr *sa, socklen_t salen,
    dispatch_source_t *srcp, dispatch_function_t fn, const char *what)
{
	dispatch_source_t src;
	int fd;

	fd = socket(PF_BLUETOOTH, type | SOCK_CLOEXEC, proto);
	if (fd < 0)
		return (-1);		/* the sockets are not loaded yet */
	if (bind(fd, sa, salen) != 0 || listen(fd, 4) != 0 ||
	    set_nonblock(fd) != 0) {
		xlog("cannot listen for %s: %s", what, strerror(errno));
		(void)close(fd);
		return (-1);
	}
	src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, hfp_queue());
	if (src == NULL) {
		(void)close(fd);
		return (-1);
	}
	dispatch_set_context(src, (void *)(intptr_t)fd);
	dispatch_source_set_event_handler_f(src, fn);
	dispatch_source_set_cancel_handler_f(src, fd_cancelled);
	dispatch_resume(src);
	*srcp = src;
	xlog("listening for %s", what);
	return (fd);
}

static void
start_q(void *arg)
{
	struct sockaddr_rfcomm ra;
	struct sockaddr_sco sa;

	(void)arg;
	if (g_down)
		return;
	(void)sdp_server_set_hfp_channel(HFP_RFCOMM_CHANNEL);
	(void)sdp_server_set_service(SDP_LOCAL_HFP_AG, true, SDP_AG_WIDE_BAND);
	bluetoothd_lock();
	adapter_set_cod_services_locked(ADAPTER_COD_HFP, HFP_COD_SERVICES);
	bluetoothd_unlock();

	if (g_rfcomm < 0) {
		rfcomm_fill(&ra, NG_HCI_BDADDR_ANY, HFP_RFCOMM_CHANNEL);
		g_rfcomm = listen_on(BLUETOOTH_PROTO_RFCOMM, SOCK_STREAM,
		    (struct sockaddr *)&ra, sizeof(ra), &g_rfcomm_src,
		    rfcomm_readable, "hands-free on RFCOMM channel 13");
	}
	if (g_sco < 0) {
		sco_fill(&sa, NG_HCI_BDADDR_ANY);
		g_sco = listen_on(BLUETOOTH_PROTO_SCO, SOCK_SEQPACKET,
		    (struct sockaddr *)&sa, sizeof(sa), &g_sco_src,
		    sco_readable, "SCO links");
	}
}

void
hfp_start(void)
{
	dispatch_async_f(hfp_queue(), NULL, start_q);
}

static void
shutdown_q(void *arg)
{
	int i;

	(void)arg;
	g_down = true;
	for (i = 0; i < HFP_MAX; i++) {
		if (g_sess[i].used) {
			g_sess[i].link = false;
			sess_close(&g_sess[i], "bluetoothd is stopping");
		}
	}
	if (g_rfcomm_src != NULL) {
		dispatch_source_cancel(g_rfcomm_src);
		g_rfcomm_src = NULL;
		g_rfcomm = -1;
	}
	if (g_sco_src != NULL) {
		dispatch_source_cancel(g_sco_src);
		g_sco_src = NULL;
		g_sco = -1;
	}
}

void
hfp_shutdown(void)
{
	dispatch_sync_f(hfp_queue(), NULL, shutdown_q);
}

/* ---- from audio.c ------------------------------------------------------- */

struct ev {
	struct bt_addr	peer;
	int		what;
};

enum { EV_UP, EV_DOWN, EV_BOND, EV_MIXER };

static void
ev_q(void *arg)
{
	struct ev *e = arg;
	struct hfp_sess *s;

	switch (e->what) {
	case EV_UP:
		if (g_down || !wanted(&e->peer, true))
			break;
		s = add(&e->peer);
		if (s == NULL)
			break;
		s->link = true;
		if (!s->up && !s->connecting)
			tick_after(s, s->gen, HFP_CONNECT_WAIT_MS,
			    connect_check);
		break;
	case EV_DOWN:
		s = find(&e->peer);
		if (s == NULL)
			break;
		s->link = false;
		s->gen = ++g_gen;
		sess_close(s, "the link is down");
		break;
	case EV_BOND:
		s = find(&e->peer);
		if (s != NULL && s->up && !wanted(&e->peer, false))
			sess_close(s, "audio is off for it");
		break;
	case EV_MIXER:
		s = find(&e->peer);
		if (s != NULL && s->sco_up && s->routed)
			(void)audio_route_to(&s->peer, AUDIO_ROUTE_HFP, s->dev,
			    codec_name(s->codec), true);
		break;
	}
	free(e);
}

static void
post(const struct bt_addr *peer, int what)
{
	struct ev *e = calloc(1, sizeof(*e));

	if (e == NULL)
		return;
	e->peer = *peer;
	e->peer.type = BT_ADDR_PUBLIC;
	e->what = what;
	dispatch_async_f(hfp_queue(), e, ev_q);
}

void
hfp_link_up(const struct bt_addr *peer)
{
	post(peer, EV_UP);
}

void
hfp_link_down(const struct bt_addr *peer)
{
	post(peer, EV_DOWN);
}

void
hfp_bond_updated(const struct bt_addr *peer)
{
	post(peer, EV_BOND);
}

void
hfp_mixer_started(const struct bt_addr *peer)
{
	post(peer, EV_MIXER);
}

/* ---- the RPCs ---------------------------------------------------------- */

struct call {
	struct bt_addr		peer;
	int			route;
	bt_status_t		st;
	bool			found;
	struct hfp_status	*out;
};

static void
route_q(void *arg)
{
	struct call *c = arg;
	struct hfp_sess *s = find(&c->peer);

	if (s == NULL || !s->slc) {
		c->st = bt_status_not_connected_e;
		return;
	}
	if (c->route == AUDIO_ROUTE_A2DP) {
		if (s->want_audio || s->sco_up || s->routed)
			audio_down(s, "asked for A2DP");
		c->st = bt_status_success_e;
	} else
		c->st = audio_up(s, c->route == HFP_ROUTE_HFP_CVSD);
}

bt_status_t
hfp_set_route(const struct bt_addr *peer, int route)
{
	struct call c;

	if (route != AUDIO_ROUTE_A2DP && route != AUDIO_ROUTE_HFP &&
	    route != HFP_ROUTE_HFP_CVSD)
		return (bt_status_invalid_parameter_e);
	(void)memset(&c, 0, sizeof(c));
	c.peer = *peer;
	c.peer.type = BT_ADDR_PUBLIC;
	c.route = route;
	dispatch_sync_f(hfp_queue(), &c, route_q);
	return (c.st);
}

static void
status_q(void *arg)
{
	struct call *c = arg;
	struct hfp_sess *s = find(&c->peer);
	struct hfp_status *o = c->out;

	if (s == NULL || !s->up)
		return;
	c->found = true;
	o->slc = s->slc;
	o->audio = s->sco_up;
	o->pending = s->sco_connecting || s->codec_pending != 0;
	if (s->sco_up) {
		(void)strlcpy(o->codec, codec_name(s->codec),
		    sizeof(o->codec));
		(void)strlcpy(o->device, s->dev, sizeof(o->device));
	}
	o->hf_features = s->hf_features;
	o->speaker_gain = s->vgs;
	o->mic_gain = s->vgm;
	o->battery = s->battery;
}

bool
hfp_status(const struct bt_addr *peer, struct hfp_status *out)
{
	struct call c;

	(void)memset(&c, 0, sizeof(c));
	(void)memset(out, 0, sizeof(*out));
	out->speaker_gain = out->mic_gain = out->battery = -1;
	c.peer = *peer;
	c.peer.type = BT_ADDR_PUBLIC;
	c.out = out;
	dispatch_sync_f(hfp_queue(), &c, status_q);
	return (c.found);
}
