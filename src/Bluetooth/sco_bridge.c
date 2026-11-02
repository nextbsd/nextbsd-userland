/*
 * sco_bridge.c — SCO <-> /dev/btscoN for the headset's mixer. See
 * sco_bridge.h.
 *
 * Per bridge:
 *
 *   rx ring   48 kHz stereo frames for the mixer to read, filled by the SCO
 *             thread (decode, upsample); at most RX_MAX_MS, oldest dropped,
 *             so a stalled reader never builds up delay.
 *   tx ring   8 or 16 kHz mono samples for the headset, filled by the
 *             mixer's writes (downmix, downsample) and drained by the SCO
 *             thread one packet at a time; it waits for TX_PRIME_MS before
 *             it drains, and again after running dry, so the mixer's
 *             16 ms writes do not leave a gap in every packet.
 *
 * The resamplers are windowed-sinc FIR filters at 48 kHz, cut at 0.425 of
 * the SCO rate (3.4 kHz for CVSD, 6.8 kHz for mSBC), the telephone and
 * wide-band speech pass bands.
 */
#include "sco_bridge.h"
#include "msbc.h"
#include "sbc_decode.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/linker.h>
#include <sys/soundcard.h>

#include <cuse.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	DEV_PREFIX	"btsco"
#define	CUSE_THREADS	(2 * SCO_BRIDGE_MAX + 2)

#define	RX_MAX_MS	120
#define	RX_MAX_FRAMES	(SCO_BRIDGE_RATE * RX_MAX_MS / 1000)
#define	TX_MAX_MS	200
#define	TX_MAX		(16000 * TX_MAX_MS / 1000)
#define	TX_PRIME_MS	24
#define	STALL_MS	40	/* no packet this long: give silence */
#define	SILENCE_MS	8	/* pace of the silence with no link */
#define	CHUNK_FRAMES	1024	/* frames per copy to or from the mixer */

#define	FIR_TAPS	192	/* a multiple of 6 and of 3 */
#define	UP_MAX		6
#define	HIST_MAX	(FIR_TAPS / 2)	/* low-rate history, L = 3 */

#define	MSBC_UNIT	60	/* H2 + frame + padding */
#define	STREAM_MAX	512

/* 48 kHz <-> the SCO rate: L = 6 (CVSD) or 3 (mSBC). */
struct rs {
	int		l;
	float		h[FIR_TAPS];
	/* upsampler: the last FIR_TAPS / l low-rate samples, newest first */
	float		hist[HIST_MAX];
	/* decimator: a doubled ring so every window is contiguous */
	float		ring[2 * FIR_TAPS];
	int		pos;
	int		phase;
};

struct bridge {
	int			slot;
	char			path[32];
	struct cuse_dev		*cdev;

	pthread_mutex_t		mtx;
	pthread_cond_t		cv;

	/* Shared with the mixer's side, under mtx. */
	bool			active;
	int			low_rate;	/* 8000 or 16000 */
	int16_t			rx[RX_MAX_FRAMES * SCO_BRIDGE_CHANNELS];
	int			rx_head, rx_count;	/* in frames */
	int16_t			tx[TX_MAX];
	int			tx_head, tx_count;
	bool			tx_primed;
	struct rs		down;		/* the mixer's writes */

	/* The SCO thread's own. */
	pthread_t		thread;
	bool			running;
	volatile bool		stop;
	uint32_t		gen;
	int			fd;
	sco_codec_t		codec;
	struct rs		up;
	struct sbc_dec		dec;
	struct msbc_enc		enc;
	uint8_t			rxs[STREAM_MAX];	/* mSBC in */
	int			rxs_len;
	uint8_t			txs[STREAM_MAX];	/* mSBC out */
	int			txs_len;
	unsigned		seq;

	dispatch_queue_t	q;
	void			(*ended)(void *, int);
	void			*ctx;
};

static struct bridge	*g_br[SCO_BRIDGE_MAX];
static pthread_mutex_t	g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int		g_cuse = 0;	/* 0 not tried, 1 up, -1 failed */

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[sco] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* ---- resampling ------------------------------------------------------- */

static void
rs_init(struct rs *r, int low_rate)
{
	double fc, x, w, sum = 0;
	int i, n = FIR_TAPS;

	(void)memset(r, 0, sizeof(*r));
	r->l = SCO_BRIDGE_RATE / low_rate;
	fc = 0.425 * low_rate / SCO_BRIDGE_RATE;	/* cycles per sample */
	for (i = 0; i < n; i++) {
		x = i - (n - 1) / 2.0;
		w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) +
		    0.08 * cos(4 * M_PI * i / (n - 1));		/* Blackman */
		r->h[i] = (float)(w * (x == 0 ? 2 * fc :
		    sin(2 * M_PI * fc * x) / (M_PI * x)));
		sum += r->h[i];
	}
	for (i = 0; i < n; i++)
		r->h[i] = (float)(r->h[i] / sum);	/* unity DC gain */
}

/* One low-rate sample in, r->l high-rate samples out. */
static void
rs_up(struct rs *r, float in, float *out)
{
	int taps = FIR_TAPS / r->l, p, j;
	float y;

	(void)memmove(&r->hist[1], &r->hist[0],
	    (size_t)(taps - 1) * sizeof(float));
	r->hist[0] = in;
	for (p = 0; p < r->l; p++) {
		y = 0;
		for (j = 0; j < taps; j++)
			y += r->h[j * r->l + p] * r->hist[j];
		out[p] = y * (float)r->l;
	}
}

/* One high-rate sample in; true with a low-rate sample in *out. */
static bool
rs_down(struct rs *r, float in, float *out)
{
	const float *win;
	float y = 0;
	int i;

	r->ring[r->pos] = in;
	r->ring[r->pos + FIR_TAPS] = in;
	r->pos = (r->pos + 1) % FIR_TAPS;
	if (++r->phase < r->l)
		return (false);
	r->phase = 0;
	/* ring[pos .. pos + TAPS - 1] runs oldest to newest. */
	win = &r->ring[r->pos];
	for (i = 0; i < FIR_TAPS; i++)
		y += r->h[FIR_TAPS - 1 - i] * win[i];
	*out = y;
	return (true);
}

static int16_t
clip16(float v)
{
	if (v > 32767.0f)
		return (32767);
	if (v < -32768.0f)
		return (-32768);
	return ((int16_t)lrintf(v));
}

/* ---- the rings (mtx held) ---------------------------------------------- */

static void
rx_push(struct bridge *b, int16_t v)
{
	int at;

	if (b->rx_count == RX_MAX_FRAMES) {		/* drop the oldest */
		b->rx_head = (b->rx_head + 1) % RX_MAX_FRAMES;
		b->rx_count--;
	}
	at = (b->rx_head + b->rx_count) % RX_MAX_FRAMES;
	b->rx[at * 2] = v;
	b->rx[at * 2 + 1] = v;
	b->rx_count++;
}

static int
rx_pop(struct bridge *b, int16_t *out, int frames)
{
	int i;

	if (frames > b->rx_count)
		frames = b->rx_count;
	for (i = 0; i < frames; i++) {
		out[i * 2] = b->rx[b->rx_head * 2];
		out[i * 2 + 1] = b->rx[b->rx_head * 2 + 1];
		b->rx_head = (b->rx_head + 1) % RX_MAX_FRAMES;
	}
	b->rx_count -= frames;
	return (frames);
}

static void
tx_push(struct bridge *b, int16_t v)
{
	if (b->tx_count == TX_MAX) {
		b->tx_head = (b->tx_head + 1) % TX_MAX;
		b->tx_count--;
	}
	b->tx[(b->tx_head + b->tx_count) % TX_MAX] = v;
	b->tx_count++;
}

/* `n` samples for the headset; silence while the ring is not primed. */
static void
tx_pop(struct bridge *b, int16_t *out, int n)
{
	int i, prime = b->low_rate * TX_PRIME_MS / 1000;

	if (!b->tx_primed && b->tx_count >= prime)
		b->tx_primed = true;
	if (!b->tx_primed || b->tx_count < n) {
		b->tx_primed = false;
		(void)memset(out, 0, (size_t)n * sizeof(*out));
		return;
	}
	for (i = 0; i < n; i++) {
		out[i] = b->tx[b->tx_head];
		b->tx_head = (b->tx_head + 1) % TX_MAX;
	}
	b->tx_count -= n;
}

/* ---- the SCO thread ----------------------------------------------------- */

/* Headset -> mixer: decode and upsample one packet's worth. */
static void
rx_samples(struct bridge *b, const int16_t *pcm, int n)
{
	float out[UP_MAX];
	int i, p;

	(void)pthread_mutex_lock(&b->mtx);
	for (i = 0; i < n; i++) {
		rs_up(&b->up, (float)pcm[i], out);
		for (p = 0; p < b->up.l; p++)
			rx_push(b, clip16(out[p]));
	}
	(void)pthread_cond_broadcast(&b->cv);
	(void)pthread_mutex_unlock(&b->mtx);
}

static void
rx_packet(struct bridge *b, const uint8_t *buf, int n)
{
	int16_t pcm[256];
	int i, frames, take;

	if (b->codec == SCO_CODEC_CVSD) {
		take = n / 2;
		if (take > 256)
			take = 256;
		for (i = 0; i < take; i++)
			pcm[i] = (int16_t)(buf[2 * i] | (buf[2 * i + 1] << 8));
		rx_samples(b, pcm, take);
		return;
	}

	/*
	 * mSBC: a byte stream. A frame starts at its syncword with the two
	 * zero bytes after it; the H2 header in front and the padding byte
	 * behind are skipped by the search.
	 */
	if (n > STREAM_MAX - b->rxs_len)
		b->rxs_len = 0;			/* lost: start over */
	(void)memcpy(b->rxs + b->rxs_len, buf, (size_t)n);
	b->rxs_len += n;
	i = 0;
	while (b->rxs_len - i >= MSBC_FRAME_LEN) {
		if (b->rxs[i] != MSBC_SYNCWORD || b->rxs[i + 1] != 0 ||
		    b->rxs[i + 2] != 0 ||
		    sbc_dec_msbc(&b->dec, b->rxs + i, MSBC_FRAME_LEN, pcm,
		    &frames) == 0) {
			i++;
			continue;
		}
		rx_samples(b, pcm, frames);
		i += MSBC_FRAME_LEN;
	}
	(void)memmove(b->rxs, b->rxs + i, (size_t)(b->rxs_len - i));
	b->rxs_len -= i;
}

/* Mixer -> headset: one packet of `n` bytes. */
static void
tx_packet(struct bridge *b, uint8_t *out, int n)
{
	static const uint8_t h2[4] = { 0x08, 0x38, 0xc8, 0xf8 };
	int16_t pcm[256];
	int i;

	if (b->codec == SCO_CODEC_CVSD) {
		(void)pthread_mutex_lock(&b->mtx);
		tx_pop(b, pcm, n / 2);
		(void)pthread_mutex_unlock(&b->mtx);
		for (i = 0; i < n / 2; i++) {
			out[2 * i] = (uint8_t)(pcm[i] & 0xff);
			out[2 * i + 1] = (uint8_t)((pcm[i] >> 8) & 0xff);
		}
		return;
	}
	while (b->txs_len < n && b->txs_len + MSBC_UNIT <= STREAM_MAX) {
		(void)pthread_mutex_lock(&b->mtx);
		tx_pop(b, pcm, MSBC_SAMPLES);
		(void)pthread_mutex_unlock(&b->mtx);
		b->txs[b->txs_len] = 0x01;
		b->txs[b->txs_len + 1] = h2[b->seq++ & 3];
		(void)msbc_encode(&b->enc, pcm, b->txs + b->txs_len + 2);
		b->txs[b->txs_len + 2 + MSBC_FRAME_LEN] = 0;
		b->txs_len += MSBC_UNIT;
	}
	if (b->txs_len < n) {		/* a packet larger than any SCO's */
		(void)memset(b->txs + b->txs_len, 0, (size_t)(n - b->txs_len));
		b->txs_len = n;
	}
	(void)memcpy(out, b->txs, (size_t)n);
	(void)memmove(b->txs, b->txs + n, (size_t)(b->txs_len - n));
	b->txs_len -= n;
}

struct ended_job {
	struct bridge	*b;
	uint32_t	gen;
};

static void
reap(struct bridge *b)
{
	(void)pthread_join(b->thread, NULL);
	b->running = false;
	(void)close(b->fd);
	b->fd = -1;
	(void)pthread_mutex_lock(&b->mtx);
	b->active = false;
	b->rx_count = 0;
	b->tx_count = 0;
	b->tx_primed = false;
	(void)pthread_cond_broadcast(&b->cv);
	(void)pthread_mutex_unlock(&b->mtx);
}

/* On the caller's queue: the link ended by itself. */
static void
ended_q(void *arg)
{
	struct ended_job *j = arg;
	struct bridge *b = j->b;

	if (b->running && b->gen == j->gen) {
		reap(b);
		xlog("%s: the SCO link closed", b->path);
		if (b->ended != NULL)
			b->ended(b->ctx, b->slot);
	}
	free(j);
}

static void *
pump(void *arg)
{
	struct bridge *b = arg;
	struct ended_job *j;
	struct pollfd pfd;
	uint8_t in[STREAM_MAX], out[STREAM_MAX];
	ssize_t n;
	int r;

	while (!b->stop) {
		pfd.fd = b->fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		r = poll(&pfd, 1, 200);
		if (r < 0 && errno != EINTR)
			break;
		if (r <= 0)
			continue;
		n = read(b->fd, in, sizeof(in));
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n <= 0)
			break;
		rx_packet(b, in, (int)n);
		tx_packet(b, out, (int)n);
		/* Same size back, now; a full queue drops it, never blocks. */
		(void)write(b->fd, out, (size_t)n);
	}
	if (!b->stop && (j = malloc(sizeof(*j))) != NULL) {
		j->b = b;
		j->gen = b->gen;
		dispatch_async_f(b->q, j, ended_q);
	}
	return (NULL);
}

/* ---- the device (cuse workers) ------------------------------------------ */

static int
dev_open(struct cuse_dev *cdev, int fflags)
{
	(void)cdev;
	(void)fflags;
	return (CUSE_ERR_NONE);
}

static int
dev_close(struct cuse_dev *cdev, int fflags)
{
	(void)cdev;
	(void)fflags;
	return (CUSE_ERR_NONE);
}

static int
dev_read(struct cuse_dev *cdev, int fflags, void *peer, int len)
{
	struct bridge *b = cuse_dev_get_priv0(cdev);
	int16_t buf[CHUNK_FRAMES * SCO_BRIDGE_CHANNELS];
	struct timespec ts;
	int frames, want = len / 4;

	if (want <= 0)
		return (CUSE_ERR_INVALID);
	if (want > CHUNK_FRAMES)
		want = CHUNK_FRAMES;

	(void)pthread_mutex_lock(&b->mtx);
	if (!b->active) {
		/* No link: silence at the mixer's pace. */
		(void)pthread_mutex_unlock(&b->mtx);
		(void)usleep(SILENCE_MS * 1000);
		frames = SCO_BRIDGE_RATE * SILENCE_MS / 1000;
		if (frames > want)
			frames = want;
		(void)memset(buf, 0, (size_t)frames * 4);
	} else {
		if (b->rx_count == 0 && (fflags & CUSE_FFLAG_NONBLOCK) != 0) {
			(void)pthread_mutex_unlock(&b->mtx);
			return (CUSE_ERR_WOULDBLOCK);
		}
		if (b->rx_count == 0) {
			(void)clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += STALL_MS * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			(void)pthread_cond_timedwait(&b->cv, &b->mtx, &ts);
		}
		frames = rx_pop(b, buf, want);
		(void)pthread_mutex_unlock(&b->mtx);
		if (frames == 0) {
			/* No packet for a while: keep the mixer fed. */
			frames = SCO_BRIDGE_RATE * STALL_MS / 1000;
			if (frames > want)
				frames = want;
			(void)memset(buf, 0, (size_t)frames * 4);
		}
	}
	if (cuse_got_peer_signal() == 0)
		return (CUSE_ERR_SIGNAL);
	if (cuse_copy_out(buf, peer, frames * 4) != 0)
		return (CUSE_ERR_FAULT);
	return (frames * 4);
}

static int
dev_write(struct cuse_dev *cdev, int fflags, const void *peer, int len)
{
	struct bridge *b = cuse_dev_get_priv0(cdev);
	int16_t buf[CHUNK_FRAMES * SCO_BRIDGE_CHANNELS];
	const uint8_t *p = peer;
	int done = 0, n, i;
	float v;

	(void)fflags;
	len &= ~3;
	while (done < len) {
		n = len - done;
		if (n > (int)sizeof(buf))
			n = (int)sizeof(buf);
		if (cuse_copy_in(p + done, buf, n) != 0)
			return (done > 0 ? done : CUSE_ERR_FAULT);
		(void)pthread_mutex_lock(&b->mtx);
		if (b->active) {
			for (i = 0; i < n / 4; i++) {
				if (rs_down(&b->down, ((float)buf[2 * i] +
				    (float)buf[2 * i + 1]) / 2.0f, &v))
					tx_push(b, clip16(v));
			}
		}
		(void)pthread_mutex_unlock(&b->mtx);
		done += n;
	}
	return (len);
}

static int
dev_ioctl(struct cuse_dev *cdev, int fflags, unsigned long cmd, void *peer)
{
	struct bridge *b = cuse_dev_get_priv0(cdev);
	union {
		int		val;
		audio_buf_info	bi;
	} data;
	int len = (int)IOCPARM_LEN(cmd);

	(void)fflags;
	if (len < 0 || len > (int)sizeof(data))
		return (CUSE_ERR_INVALID);
	(void)memset(&data, 0, sizeof(data));
	if ((cmd & IOC_IN) != 0 && cuse_copy_in(peer, &data, len) != 0)
		return (CUSE_ERR_FAULT);

	switch (cmd) {
	case FIONBIO:
	case FIOASYNC:
	case SNDCTL_DSP_NONBLOCK:
	case SNDCTL_DSP_RESET:
	case SNDCTL_DSP_SYNC:
		break;
	case SNDCTL_DSP_SETFMT:
	case SNDCTL_DSP_GETFMTS:
		data.val = AFMT_S16_LE;
		break;
	case SNDCTL_DSP_CHANNELS:
	case SOUND_PCM_READ_CHANNELS:
		data.val = SCO_BRIDGE_CHANNELS;
		break;
	case SNDCTL_DSP_SPEED:
	case SOUND_PCM_READ_RATE:
		data.val = SCO_BRIDGE_RATE;
		break;
	case SNDCTL_DSP_SETBLKSIZE:
		break;
	case SNDCTL_DSP_GETBLKSIZE:
		data.val = SCO_BRIDGE_RATE * 4 * 16 / 1000;	/* 16 ms */
		break;
	case FIONREAD:
		(void)pthread_mutex_lock(&b->mtx);
		data.val = b->rx_count * 4;
		(void)pthread_mutex_unlock(&b->mtx);
		break;
	case SNDCTL_DSP_GETODELAY:
		data.val = 0;
		break;
	case SNDCTL_DSP_GETISPACE:
	case SNDCTL_DSP_GETOSPACE:
		data.bi.fragsize = SCO_BRIDGE_RATE * 4 * 16 / 1000;
		data.bi.fragstotal = 8;
		data.bi.fragments = 8;
		data.bi.bytes = data.bi.fragsize * 8;
		break;
	default:
		return (CUSE_ERR_INVALID);
	}
	if ((cmd & IOC_OUT) != 0 && cuse_copy_out(&data, peer, len) != 0)
		return (CUSE_ERR_FAULT);
	return (CUSE_ERR_NONE);
}

static int
dev_poll(struct cuse_dev *cdev, int fflags, int events)
{
	struct bridge *b = cuse_dev_get_priv0(cdev);
	int ret = CUSE_POLL_NONE;

	(void)pthread_mutex_lock(&b->mtx);
	if ((events & CUSE_POLL_READ) != 0 && (fflags & CUSE_FFLAG_READ) != 0 &&
	    (!b->active || b->rx_count > 0))
		ret |= CUSE_POLL_READ;
	(void)pthread_mutex_unlock(&b->mtx);
	if ((events & CUSE_POLL_WRITE) != 0 && (fflags & CUSE_FFLAG_WRITE) != 0)
		ret |= CUSE_POLL_WRITE;
	return (ret);
}

static const struct cuse_methods g_methods = {
	.cm_open = dev_open,
	.cm_close = dev_close,
	.cm_read = dev_read,
	.cm_write = dev_write,
	.cm_ioctl = dev_ioctl,
	.cm_poll = dev_poll,
};

static void *
cuse_worker(void *arg)
{
	(void)arg;
	for (;;) {
		if (cuse_wait_and_process() != 0)
			break;
	}
	return (NULL);
}

/* Start a thread with every signal blocked (see sco_bridge.h). */
static int
spawn(pthread_t *t, void *(*fn)(void *), void *arg)
{
	sigset_t all, old;
	int rc;

	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &old);
	rc = pthread_create(t, NULL, fn, arg);
	(void)pthread_sigmask(SIG_SETMASK, &old, NULL);
	return (rc);
}

/* libcuse and its workers, once. g_init_lock held. */
static int
cuse_up(void)
{
	pthread_t t;
	int i;

	if (g_cuse != 0)
		return (g_cuse > 0 ? 0 : -1);
	/* Compiled into the NextBSD kernel; a module elsewhere. */
	(void)kldload("cuse");
	if (cuse_init() != 0) {
		xlog("cuse is not available: no headset microphone");
		g_cuse = -1;
		return (-1);
	}
	for (i = 0; i < CUSE_THREADS; i++) {
		if (spawn(&t, cuse_worker, NULL) == 0)
			(void)pthread_detach(t);
	}
	g_cuse = 1;
	return (0);
}

int
sco_bridge_device(int slot, char *path, size_t len)
{
	struct bridge *b;
	int rc = 0;

	if (slot < 0 || slot >= SCO_BRIDGE_MAX)
		return (-1);
	(void)pthread_mutex_lock(&g_init_lock);
	if (g_br[slot] == NULL) {
		if (cuse_up() != 0)
			rc = -1;
		else if ((b = calloc(1, sizeof(*b))) == NULL)
			rc = -1;
		else {
			b->slot = slot;
			b->fd = -1;
			b->low_rate = 8000;
			(void)pthread_mutex_init(&b->mtx, NULL);
			(void)pthread_cond_init(&b->cv, NULL);
			(void)snprintf(b->path, sizeof(b->path), "/dev/%s%d",
			    DEV_PREFIX, slot);
			/* root only: the mixer (run by bluetoothd) reads it. */
			b->cdev = cuse_dev_create(&g_methods, b, NULL, 0, 0,
			    0600, DEV_PREFIX "%d", slot);
			if (b->cdev == NULL) {
				xlog("cannot create %s", b->path);
				free(b);
				rc = -1;
			} else
				g_br[slot] = b;
		}
	}
	if (rc == 0)
		(void)strlcpy(path, g_br[slot]->path, len);
	(void)pthread_mutex_unlock(&g_init_lock);
	return (rc);
}

int
sco_bridge_start(int slot, int fd, sco_codec_t codec, dispatch_queue_t q,
    void (*ended)(void *, int), void *ctx)
{
	char path[32];
	struct bridge *b;
	int fl;

	if (sco_bridge_device(slot, path, sizeof(path)) != 0) {
		(void)close(fd);
		return (-1);
	}
	b = g_br[slot];
	if (b->running)
		sco_bridge_stop(slot);
	fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	b->fd = fd;
	b->codec = codec;
	b->q = q;
	b->ended = ended;
	b->ctx = ctx;
	b->stop = false;
	b->gen++;
	b->rxs_len = b->txs_len = 0;
	b->seq = 0;
	sbc_dec_reset(&b->dec);
	msbc_enc_reset(&b->enc);
	rs_init(&b->up, codec == SCO_CODEC_MSBC ? 16000 : 8000);

	(void)pthread_mutex_lock(&b->mtx);
	b->low_rate = codec == SCO_CODEC_MSBC ? 16000 : 8000;
	rs_init(&b->down, b->low_rate);
	b->rx_head = b->rx_count = 0;
	b->tx_head = b->tx_count = 0;
	b->tx_primed = false;
	b->active = true;
	(void)pthread_mutex_unlock(&b->mtx);

	if (spawn(&b->thread, pump, b) != 0) {
		(void)pthread_mutex_lock(&b->mtx);
		b->active = false;
		(void)pthread_mutex_unlock(&b->mtx);
		(void)close(fd);
		b->fd = -1;
		return (-1);
	}
	b->running = true;
	cuse_poll_wakeup();
	xlog("%s: carrying a %s link", b->path,
	    codec == SCO_CODEC_MSBC ? "mSBC" : "CVSD");
	return (0);
}

void
sco_bridge_stop(int slot)
{
	struct bridge *b;

	if (slot < 0 || slot >= SCO_BRIDGE_MAX || (b = g_br[slot]) == NULL ||
	    !b->running)
		return;
	b->stop = true;
	reap(b);
	cuse_poll_wakeup();
}

bool
sco_bridge_active(int slot)
{
	return (slot >= 0 && slot < SCO_BRIDGE_MAX && g_br[slot] != NULL &&
	    g_br[slot]->running);
}
