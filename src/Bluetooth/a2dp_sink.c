/*
 * a2dp_sink.c — the A2DP sink. See a2dp_sink.h.
 *
 * The acceptor state machine follows virtual_oss's avdtpACPHandlePacket()
 * (lib/virtual_oss/bt/avdtp.c) and the playback loop virtual_bt_speaker's
 * process_connection(), both BSD-licensed; neither program is run.
 */
#include "a2dp_sink.h"
#include "adapter.h"
#include "audio.h"
#include "avdtp.h"
#include "bluetoothd.h"
#include "btstate.h"
#include "btstore.h"
#include "hci_compat.h"
#include "sbc_decode.h"
#include "sdp_server.h"

#include <sys/soundcard.h>

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

#define	MEDIA_BUF		4096	/* above any L2CAP MTU we accept */
#define	IDLE_POLL_MS		500
#define	PREFILL_MS		60	/* silence ahead of the first frame */

typedef enum {
	SS_IDLE = 0,		/* signalling up, nothing configured */
	SS_CONFIGURED,		/* SET_CONFIGURATION accepted */
	SS_OPEN,		/* OPEN accepted; media channel due or up */
	SS_STREAMING		/* START accepted */
} ss_state_t;

/* What the playback thread shares with the session. */
struct player {
	pthread_mutex_t	lock;
	int		refs;		/* the session's and the thread's */
	int		media;		/* the thread's; closed by it */
	int		stop[2];	/* the session writes [1] to stop it */
	bool		suspended;
	char		addr[BT_ADDR_STRLEN];
};

struct sink_sess {
	bool			used;
	struct bt_addr		peer;
	char			addr[BT_ADDR_STRLEN];
	uint8_t			local[BT_ADDR_LEN];
	int			sig;
	dispatch_source_t	sig_src;
	ss_state_t		state;
	uint8_t			int_seid;
	uint8_t			sbc[4];		/* the configuration */
	struct avdtp_msg	msg;
	struct player		*pl;
};

static int		g_listen = -1;
static dispatch_source_t g_listen_src;
static bool		g_enabled;
static struct sink_sess	g_sess;		/* one stream at a time */

/* What a2dp_sink_publish() shows; under g_pub_lock. */
static pthread_mutex_t	g_pub_lock = PTHREAD_MUTEX_INITIALIZER;
static bool		g_pub_dirty = true;
static bool		g_pub_enabled;
static bool		g_pub_have_peer;
static struct bt_addr	g_pub_peer;
static uint8_t		g_pub_local[BT_ADDR_LEN];

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[a2dp-sink] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void
pub_update(void)
{
	(void)pthread_mutex_lock(&g_pub_lock);
	g_pub_enabled = g_enabled;
	g_pub_have_peer = g_sess.used && g_sess.state >= SS_OPEN;
	if (g_pub_have_peer) {
		g_pub_peer = g_sess.peer;
		(void)memcpy(g_pub_local, g_sess.local, BT_ADDR_LEN);
	}
	g_pub_dirty = true;
	(void)pthread_mutex_unlock(&g_pub_lock);
	bluetoothd_wake();
}

void
a2dp_sink_publish(void)
{
	struct {
		char		node[32];
		uint8_t		addr[BT_ADDR_LEN];
		bool		have_addr;
	} ad[BT_MAX_ADAPTERS];
	struct bt_addr peer;
	uint8_t local[BT_ADDR_LEN];
	bool enabled, have_peer;
	int i, n;

	(void)pthread_mutex_lock(&g_pub_lock);
	if (!g_pub_dirty) {
		(void)pthread_mutex_unlock(&g_pub_lock);
		return;
	}
	g_pub_dirty = false;
	enabled = g_pub_enabled;
	have_peer = g_pub_have_peer;
	peer = g_pub_peer;
	(void)memcpy(local, g_pub_local, sizeof(local));
	(void)pthread_mutex_unlock(&g_pub_lock);

	bluetoothd_lock();
	n = bluetoothd_adapter_count_locked();
	if (n > BT_MAX_ADAPTERS)
		n = BT_MAX_ADAPTERS;
	for (i = 0; i < n; i++) {
		struct bt_adapter *a = bluetoothd_adapter_at_locked(i);

		(void)strlcpy(ad[i].node, a->node, sizeof(ad[i].node));
		ad[i].have_addr = a->have_addr;
		(void)memcpy(ad[i].addr, a->addr.b, BT_ADDR_LEN);
	}
	bluetoothd_unlock();

	for (i = 0; i < n; i++) {
		bool here = have_peer && ad[i].have_addr &&
		    memcmp(ad[i].addr, local, BT_ADDR_LEN) == 0;

		(void)bt_state_publish_a2dp_sink(ad[i].node, enabled,
		    here ? &peer : NULL);
	}
	bt_state_notify(BT_NOTIFY_AUDIO);
}

/* ---- playback (its own thread) ------------------------------------------- */

static void
player_put(struct player *pl)
{
	int refs;

	(void)pthread_mutex_lock(&pl->lock);
	refs = --pl->refs;
	(void)pthread_mutex_unlock(&pl->lock);
	if (refs > 0)
		return;
	if (pl->stop[0] >= 0)
		(void)close(pl->stop[0]);
	if (pl->stop[1] >= 0)
		(void)close(pl->stop[1]);
	(void)pthread_mutex_destroy(&pl->lock);
	free(pl);
}

/* Open the system output for `d`'s format; -1 if it cannot be had now. */
static int
dsp_open(const struct sbc_dec *d, const char *addr)
{
	int fd, v;

	fd = open(A2DP_SINK_OUTPUT, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		xlog("%s: cannot open %s: %s", addr, A2DP_SINK_OUTPUT,
		    strerror(errno));
		return (-1);
	}
	v = AFMT_S16_NE;
	if (ioctl(fd, SNDCTL_DSP_SETFMT, &v) != 0 || v != AFMT_S16_NE)
		goto bad;
	v = d->channels;
	if (ioctl(fd, SNDCTL_DSP_CHANNELS, &v) != 0 || v != d->channels)
		goto bad;
	v = d->rate;
	if (ioctl(fd, SNDCTL_DSP_SPEED, &v) != 0)
		goto bad;
	v = (8 << 16) | 11;	/* 8 fragments of 2 KiB */
	(void)ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &v);
	xlog("%s: playing %d Hz %s to %s", addr, d->rate,
	    d->channels == 2 ? "stereo" : "mono", A2DP_SINK_OUTPUT);
	return (fd);
bad:
	xlog("%s: %s does not take %d Hz, %d channel(s), 16 bits", addr,
	    A2DP_SINK_OUTPUT, d->rate, d->channels);
	(void)close(fd);
	return (-1);
}

static void
write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	ssize_t n;

	while (len > 0) {
		n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		p += n;
		len -= (size_t)n;
	}
}

static void *
player_thread(void *arg)
{
	struct player *pl = arg;
	struct sbc_dec *dec;
	struct pollfd pfd[2];
	uint8_t *buf;
	int16_t pcm[2 * SBC_MAX_SAMPLES];
	time_t last_try = 0;
	int dsp = -1, rate = 0, channels = 0;
	bool suspended;

	dec = malloc(sizeof(*dec));
	buf = malloc(MEDIA_BUF);
	if (dec == NULL || buf == NULL)
		goto out;
	sbc_dec_reset(dec);

	for (;;) {
		const uint8_t *pay;
		size_t plen, used;
		ssize_t n;
		int frames, nf, rc;

		pfd[0].fd = pl->media;
		pfd[0].events = POLLIN;
		pfd[1].fd = pl->stop[0];
		pfd[1].events = POLLIN;
		pfd[0].revents = pfd[1].revents = 0;
		rc = poll(pfd, 2, IDLE_POLL_MS);
		if (rc < 0 && errno != EINTR)
			break;
		if (pfd[1].revents != 0)
			break;		/* the session ended */
		(void)pthread_mutex_lock(&pl->lock);
		suspended = pl->suspended;
		(void)pthread_mutex_unlock(&pl->lock);
		if (rc == 0) {
			/* Quiet: give the device back while suspended. */
			if (suspended && dsp >= 0) {
				(void)close(dsp);
				dsp = -1;
			}
			continue;
		}
		if ((pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
			continue;
		n = recv(pl->media, buf, MEDIA_BUF, 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
			break;		/* the peer closed the stream */
		if (n <= 0)
			continue;

		pay = avdtp_rtp_payload(buf, (size_t)n, &plen);
		if (pay == NULL || plen < 1 || (pay[0] & 0x80) != 0)
			continue;	/* not RTP, or a fragmented frame */
		frames = pay[0] & 0x0f;
		pay++;
		plen--;
		for (; frames > 0 && plen > 0; frames--) {
			used = sbc_dec_frame(dec, pay, plen, pcm, &nf);
			if (used == 0)
				break;
			pay += used;
			plen -= used;
			if (dsp >= 0 && (dec->rate != rate ||
			    dec->channels != channels)) {
				(void)close(dsp);
				dsp = -1;
			}
			if (dsp < 0 && time(NULL) != last_try) {
				last_try = time(NULL);
				dsp = dsp_open(dec, pl->addr);
				if (dsp >= 0) {
					int16_t zero[2 * 480];
					int ms;

					rate = dec->rate;
					channels = dec->channels;
					(void)memset(zero, 0, sizeof(zero));
					/* A little slack for radio jitter. */
					for (ms = 0; ms < PREFILL_MS; ms += 10)
						write_all(dsp, zero,
						    (size_t)(rate / 100) *
						    (size_t)channels * 2);
				}
			}
			if (dsp >= 0)
				write_all(dsp, pcm, (size_t)nf *
				    (size_t)dec->channels * sizeof(int16_t));
		}
	}
out:
	if (dsp >= 0)
		(void)close(dsp);
	(void)close(pl->media);
	free(buf);
	free(dec);
	player_put(pl);
	return (NULL);
}

static struct player *
player_start(int media, const char *addr)
{
	struct player *pl;
	pthread_attr_t attr;
	pthread_t t;
	int rc;

	pl = calloc(1, sizeof(*pl));
	if (pl == NULL)
		return (NULL);
	if (pipe2(pl->stop, O_CLOEXEC) != 0) {
		free(pl);
		return (NULL);
	}
	(void)pthread_mutex_init(&pl->lock, NULL);
	pl->media = media;
	pl->refs = 2;
	(void)strlcpy(pl->addr, addr, sizeof(pl->addr));
	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rc = pthread_create(&t, &attr, player_thread, pl);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		(void)close(pl->stop[0]);
		(void)close(pl->stop[1]);
		(void)pthread_mutex_destroy(&pl->lock);
		free(pl);
		return (NULL);
	}
	return (pl);
}

static void
player_stop(struct player *pl)
{
	if (pl == NULL)
		return;
	(void)write(pl->stop[1], "", 1);
	player_put(pl);
}

static void
player_suspend(struct player *pl, bool on)
{
	if (pl == NULL)
		return;
	(void)pthread_mutex_lock(&pl->lock);
	pl->suspended = on;
	(void)pthread_mutex_unlock(&pl->lock);
}

/* ---- the session (audio queue) ------------------------------------------- */

/* A source's descriptor is closed once the source is cancelled. */
static void
fd_cancelled(void *arg)
{
	(void)close((int)(intptr_t)arg);
}

static void
sess_end(const char *why)
{
	struct sink_sess *s = &g_sess;
	bool was_up;

	if (!s->used)
		return;
	xlog("%s: session ended: %s", s->addr, why);
	was_up = s->pl != NULL;
	player_stop(s->pl);
	s->pl = NULL;
	if (s->sig_src != NULL) {
		dispatch_source_cancel(s->sig_src);	/* closes s->sig */
		dispatch_release(s->sig_src);
		s->sig_src = NULL;
	} else if (s->sig >= 0)
		(void)close(s->sig);
	s->sig = -1;
	s->used = false;
	if (was_up)
		audio_sink_changed(&s->peer, false, NULL);
	pub_update();
}

/* Our one endpoint's capabilities: any SBC configuration. */
static size_t
put_caps(uint8_t *p)
{
	p[0] = AVDTP_CAT_MEDIA_TRANSPORT;
	p[1] = 0;
	p[2] = AVDTP_CAT_MEDIA_CODEC;
	p[3] = 6;
	p[4] = AVDTP_MEDIA_AUDIO << 4;
	p[5] = A2DP_CODEC_SBC;
	p[6] = SBC_FREQ_16000 | SBC_FREQ_32000 | SBC_FREQ_44100 |
	    SBC_FREQ_48000 | SBC_CH_MONO | SBC_CH_DUAL | SBC_CH_STEREO |
	    SBC_CH_JOINT;
	p[7] = SBC_BLOCKS_4 | SBC_BLOCKS_8 | SBC_BLOCKS_12 | SBC_BLOCKS_16 |
	    SBC_SUBBANDS_4 | SBC_SUBBANDS_8 | SBC_ALLOC_SNR |
	    SBC_ALLOC_LOUDNESS;
	p[8] = SBC_BITPOOL_MIN;
	p[9] = SBC_BITPOOL_MAX;
	return (10);
}

/* Exactly one bit of `mask` set in `v`. */
static bool
one_of(uint8_t v, uint8_t mask)
{
	v &= mask;
	return (v != 0 && (v & (v - 1)) == 0);
}

/*
 * Check a SET_CONFIGURATION's capabilities. Returns 0, or the error code
 * with the offending category in *cat.
 */
static uint8_t
check_config(struct sink_sess *s, const uint8_t *p, size_t len, uint8_t *cat)
{
	bool codec = false;
	size_t i = 0;

	while (i + 2 <= len) {
		uint8_t c = p[i], l = p[i + 1];
		const uint8_t *v = p + i + 2;

		*cat = c;
		if (i + 2 + l > len)
			return (AVDTP_ERR_BAD_LENGTH);
		switch (c) {
		case AVDTP_CAT_MEDIA_TRANSPORT:
			break;
		case AVDTP_CAT_MEDIA_CODEC:
			if (l < 6 || (v[0] >> 4) != AVDTP_MEDIA_AUDIO ||
			    v[1] != A2DP_CODEC_SBC)
				return (AVDTP_ERR_UNSUPPORTED_CONFIG);
			if (!one_of(v[2], 0xf0) || !one_of(v[2], 0x0f) ||
			    !one_of(v[3], 0xf0) || !one_of(v[3], 0x0c) ||
			    !one_of(v[3], 0x03) || v[4] < SBC_BITPOOL_MIN ||
			    v[5] > SBC_BITPOOL_MAX || v[4] > v[5])
				return (AVDTP_ERR_INVALID_CAPABILITIES);
			(void)memcpy(s->sbc, v + 2, 4);
			codec = true;
			break;
		default:
			return (AVDTP_ERR_BAD_SERV_CATEGORY);
		}
		i += 2 + (size_t)l;
	}
	if (!codec) {
		*cat = AVDTP_CAT_MEDIA_CODEC;
		return (AVDTP_ERR_INVALID_CAPABILITIES);
	}
	return (0);
}

/* One command from the peer. */
static void
handle(struct sink_sess *s, const struct avdtp_msg *m)
{
	uint8_t out[16], err[2], cat = 0, e;
	size_t n;
	int fd = s->sig;

	if (m->type != AVDTP_MSG_COMMAND)
		return;		/* we send no commands: a stray response */

	switch (m->signal) {
	case AVDTP_DISCOVER:
		out[0] = (uint8_t)((A2DP_SINK_SEID << 2) |
		    (s->state != SS_IDLE ? 0x02 : 0));
		out[1] = (AVDTP_MEDIA_AUDIO << 4) | 0x08;	/* TSEP sink */
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    out, 2);
		return;

	case AVDTP_GET_CAPABILITIES:
	case AVDTP_GET_ALL_CAPABILITIES:
		if (m->len < 1 || (m->params[0] >> 2) != A2DP_SINK_SEID) {
			err[0] = AVDTP_ERR_BAD_ACP_SEID;
			(void)avdtp_reject(fd, m, err, 1);
			return;
		}
		n = put_caps(out);
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    out, n);
		return;

	case AVDTP_SET_CONFIGURATION:
		if (m->len < 2 || (m->params[0] >> 2) != A2DP_SINK_SEID) {
			err[0] = 0;
			err[1] = AVDTP_ERR_BAD_ACP_SEID;
			(void)avdtp_reject(fd, m, err, 2);
			return;
		}
		if (s->state != SS_IDLE) {
			err[0] = 0;
			err[1] = AVDTP_ERR_SEP_IN_USE;
			(void)avdtp_reject(fd, m, err, 2);
			return;
		}
		e = check_config(s, m->params + 2, m->len - 2, &cat);
		if (e != 0) {
			err[0] = cat;
			err[1] = e;
			(void)avdtp_reject(fd, m, err, 2);
			return;
		}
		s->int_seid = m->params[1] >> 2;
		s->state = SS_CONFIGURED;
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    NULL, 0);
		xlog("%s: configured SBC %02x %02x, bitpool %u..%u", s->addr,
		    s->sbc[0], s->sbc[1], s->sbc[2], s->sbc[3]);
		return;

	case AVDTP_GET_CONFIGURATION:
		if (s->state == SS_IDLE) {
			err[0] = AVDTP_ERR_SEP_NOT_IN_USE;
			(void)avdtp_reject(fd, m, err, 1);
			return;
		}
		out[0] = AVDTP_CAT_MEDIA_TRANSPORT;
		out[1] = 0;
		out[2] = AVDTP_CAT_MEDIA_CODEC;
		out[3] = 6;
		out[4] = AVDTP_MEDIA_AUDIO << 4;
		out[5] = A2DP_CODEC_SBC;
		(void)memcpy(out + 6, s->sbc, 4);
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    out, 10);
		return;

	case AVDTP_OPEN:
		if (s->state != SS_CONFIGURED) {
			err[0] = AVDTP_ERR_BAD_STATE;
			(void)avdtp_reject(fd, m, err, 1);
			return;
		}
		s->state = SS_OPEN;
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    NULL, 0);
		return;

	case AVDTP_START:
		if (s->state != SS_OPEN || s->pl == NULL) {
			err[0] = A2DP_SINK_SEID << 2;
			err[1] = AVDTP_ERR_BAD_STATE;
			(void)avdtp_reject(fd, m, err, 2);
			return;
		}
		s->state = SS_STREAMING;
		player_suspend(s->pl, false);
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    NULL, 0);
		return;

	case AVDTP_SUSPEND:
		if (s->state != SS_STREAMING) {
			err[0] = A2DP_SINK_SEID << 2;
			err[1] = AVDTP_ERR_BAD_STATE;
			(void)avdtp_reject(fd, m, err, 2);
			return;
		}
		s->state = SS_OPEN;
		player_suspend(s->pl, true);
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    NULL, 0);
		return;

	case AVDTP_CLOSE:
	case AVDTP_ABORT:
		if (m->signal == AVDTP_CLOSE && s->state < SS_OPEN) {
			err[0] = AVDTP_ERR_BAD_STATE;
			(void)avdtp_reject(fd, m, err, 1);
			return;
		}
		(void)avdtp_send(fd, m->label, AVDTP_MSG_ACCEPT, m->signal,
		    NULL, 0);
		if (s->pl != NULL) {
			player_stop(s->pl);
			s->pl = NULL;
			audio_sink_changed(&s->peer, false, NULL);
		}
		s->state = SS_IDLE;
		pub_update();
		return;

	case AVDTP_RECONFIGURE:
		err[0] = AVDTP_CAT_MEDIA_CODEC;
		err[1] = AVDTP_ERR_NOT_SUPPORTED_COMMAND;
		(void)avdtp_reject(fd, m, err, 2);
		return;

	default:
		/* Security control, delay report, anything newer. */
		err[0] = AVDTP_ERR_NOT_SUPPORTED_COMMAND;
		(void)avdtp_reject(fd, m, err, 1);
		return;
	}
}

static void
sig_readable(void *arg)
{
	struct sink_sess *s = &g_sess;
	int rc;

	if (!s->used || s->sig != (int)(intptr_t)arg)
		return;
	for (;;) {
		rc = avdtp_recv(s->sig, &s->msg);
		if (rc < 0) {
			sess_end("the signalling channel closed");
			return;
		}
		if (rc == 0)
			return;
		handle(s, &s->msg);
		if (!s->used)
			return;
	}
}

/* The media channel arrived (after OPEN). */
static void
media_accepted(struct sink_sess *s, int fd)
{
	int v;

	v = MEDIA_BUF * 16;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
	s->pl = player_start(fd, s->addr);
	if (s->pl == NULL) {
		(void)close(fd);
		sess_end("cannot start playback");
		return;
	}
	xlog("%s: media channel up", s->addr);
	audio_sink_changed(&s->peer, true, "SBC");
	pub_update();
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	return (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0);
}

static void
listen_readable(void *arg)
{
	struct sockaddr_l2cap sa, la;
	struct sink_sess *s = &g_sess;
	struct bt_addr peer;
	char addr[BT_ADDR_STRLEN];
	socklen_t len = sizeof(sa);
	int fd;

	if (g_listen != (int)(intptr_t)arg)
		return;
	fd = accept(g_listen, (struct sockaddr *)&sa, &len);
	if (fd < 0)
		return;
	(void)memset(&peer, 0, sizeof(peer));
	(void)memcpy(peer.b, &sa.l2cap_bdaddr, BT_ADDR_LEN);
	peer.type = BT_ADDR_PUBLIC;
	(void)bt_addr_ntoa(&peer, BT_ADDR_FMT_DISPLAY, addr, sizeof(addr));

	/* The second channel of a session that has been opened: media. */
	if (s->used && memcmp(s->peer.b, peer.b, BT_ADDR_LEN) == 0) {
		if (s->state == SS_OPEN && s->pl == NULL)
			media_accepted(s, fd);
		else {
			xlog("%s: unexpected AVDTP channel; closed", addr);
			(void)close(fd);
		}
		return;
	}

	/* PSM 0x19's role rule: a headset is ours to play to. */
	if (audio_peer_is_sink(&peer)) {
		xlog("%s: a sink opened AVDTP; we connect to it instead",
		    addr);
		(void)close(fd);
		return;
	}
	if (s->used) {
		xlog("%s: %s is already playing to us; refused", addr,
		    s->addr);
		(void)close(fd);
		return;
	}

	(void)memset(s, 0, sizeof(*s));
	s->used = true;
	s->peer = peer;
	(void)strlcpy(s->addr, addr, sizeof(s->addr));
	len = sizeof(la);
	if (getsockname(fd, (struct sockaddr *)&la, &len) == 0)
		(void)memcpy(s->local, &la.l2cap_bdaddr, BT_ADDR_LEN);
	s->sig = fd;
	s->state = SS_IDLE;
	if (set_nonblock(fd) != 0) {
		sess_end("cannot set up the signalling channel");
		return;
	}
	s->sig_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, audio_queue());
	if (s->sig_src == NULL) {
		sess_end("no dispatch source");
		return;
	}
	dispatch_set_context(s->sig_src, (void *)(intptr_t)fd);
	dispatch_source_set_event_handler_f(s->sig_src, sig_readable);
	dispatch_source_set_cancel_handler_f(s->sig_src, fd_cancelled);
	dispatch_resume(s->sig_src);
	xlog("%s: AVDTP session (we are the sink)", addr);
}

/* ---- the preference (audio queue) ---------------------------------------- */

static void
listen_close(void)
{
	if (g_listen_src != NULL) {
		dispatch_source_cancel(g_listen_src);	/* closes g_listen */
		dispatch_release(g_listen_src);
		g_listen_src = NULL;
	}
	g_listen = -1;
}

static int
listen_open(void)
{
	struct sockaddr_l2cap sa;
	int fd;

	if (g_listen >= 0)
		return (0);
	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		return (-1);	/* the L2CAP sockets are not loaded yet */
	(void)memset(&sa, 0, sizeof(sa));
	sa.l2cap_len = sizeof(sa);
	sa.l2cap_family = AF_BLUETOOTH;
	sa.l2cap_psm = htole16(AVDTP_PSM);
	(void)memcpy(&sa.l2cap_bdaddr, NG_HCI_BDADDR_ANY, sizeof(bdaddr_t));
	sa.l2cap_bdaddr_type = BDADDR_BREDR;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(fd, 4) != 0 || set_nonblock(fd) != 0) {
		xlog("cannot listen on PSM 0x%02x: %s", AVDTP_PSM,
		    strerror(errno));
		(void)close(fd);
		return (-1);
	}
	g_listen_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, audio_queue());
	if (g_listen_src == NULL) {
		(void)close(fd);
		return (-1);
	}
	g_listen = fd;
	dispatch_set_context(g_listen_src, (void *)(intptr_t)fd);
	dispatch_source_set_event_handler_f(g_listen_src, listen_readable);
	dispatch_source_set_cancel_handler_f(g_listen_src, fd_cancelled);
	dispatch_resume(g_listen_src);
	xlog("listening on PSM 0x%02x (A2DP sink)", AVDTP_PSM);
	return (0);
}

static void
apply_q(void *arg)
{
	struct bt_prefs prefs;
	bool on;

	(void)arg;
	bluetoothd_lock();
	bluetoothd_prefs_locked(&prefs);
	on = prefs.a2dp_sink;
	adapter_set_cod_services_locked(ADAPTER_COD_A2DP_SINK,
	    on ? A2DP_COD_SERVICES : 0);
	bluetoothd_unlock();

	(void)sdp_server_set_service(SDP_LOCAL_A2DP_SINK, on, 0);
	if (on) {
		if (listen_open() == 0 && !g_enabled)
			xlog("A2DP sink on: this machine is a speaker");
	} else {
		if (g_enabled)
			xlog("A2DP sink off");
		sess_end("the A2DP sink was turned off");
		listen_close();
	}
	g_enabled = on && g_listen >= 0;
	pub_update();
}

void
a2dp_sink_prefs_changed(void)
{
	dispatch_async_f(audio_queue(), NULL, apply_q);
}

void
a2dp_sink_link_down(const struct bt_addr *peer)
{
	if (g_sess.used && memcmp(g_sess.peer.b, peer->b, BT_ADDR_LEN) == 0)
		sess_end("the link is down");
}

void
a2dp_sink_shutdown(void)
{
	sess_end("bluetoothd is stopping");
	listen_close();
}
