/*
 * msbc.c — the mSBC encoder. See msbc.h.
 *
 * The analysis follows the SBC encoder of A2DP 1.3, §12.5.1 (the filter
 * bank) and §12.6 (scale factors, bit allocation, quantisation), with the
 * one configuration mSBC allows. The prototype filter and the bit
 * allocation are sbc_decode.c's, so both directions share one copy of each.
 */
#include "msbc.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#define	BANDS		8

/* The analysis matrix M[k][i] = cos((k + 0.5) (i - 4) pi / 8). */
static float		g_m[BANDS][16];
static float		g_c[80];
static pthread_once_t	g_once = PTHREAD_ONCE_INIT;

static void
tables_init(void)
{
	int i, k;

	for (k = 0; k < BANDS; k++) {
		for (i = 0; i < 16; i++)
			g_m[k][i] = cosf((float)((k + 0.5) * (i - 4) *
			    (M_PI / 8.0)));
	}
	for (i = 0; i < 80; i++)
		g_c[i] = (float)sbc_proto8[i];
}

void
msbc_enc_reset(struct msbc_enc *e)
{
	(void)pthread_once(&g_once, tables_init);
	(void)memset(e, 0, sizeof(*e));
	e->alloc.freq = 0;		/* 16 kHz: the loudness offsets */
	e->alloc.rate = 16000;
	e->alloc.blocks = MSBC_BLOCKS;
	e->alloc.bands = BANDS;
	e->alloc.mode = SBC_MODE_MONO;
	e->alloc.alloc = 0;		/* loudness */
	e->alloc.channels = 1;
	e->alloc.bitpool = MSBC_BITPOOL;
}

/* One block of 8 input samples into 8 subband samples (§12.5.1). */
static void
analyse(struct msbc_enc *e, const int16_t *in, float *s)
{
	float y[16];
	int i, j, k;

	for (i = 79; i >= BANDS; i--)
		e->x[i] = e->x[i - BANDS];
	for (i = 0; i < BANDS; i++)
		e->x[BANDS - 1 - i] = (float)in[i];
	for (i = 0; i < 16; i++) {
		y[i] = 0;
		for (j = 0; j < 5; j++)
			y[i] += g_c[i + 16 * j] * e->x[i + 16 * j];
	}
	for (k = 0; k < BANDS; k++) {
		s[k] = 0;
		for (i = 0; i < 16; i++)
			s[k] += g_m[k][i] * y[i];
	}
}

/* CRC-8 of the SBC frame (A2DP 1.3, §12.6.1.1): x^8+x^4+x^3+x^2+1. */
static uint8_t
crc_bits(uint8_t crc, uint32_t val, int nbits)
{
	int i;

	for (i = nbits - 1; i >= 0; i--) {
		if ((((val >> i) & 1) ^ ((crc >> 7) & 1)) != 0)
			crc = (uint8_t)((crc << 1) ^ 0x1d);
		else
			crc = (uint8_t)(crc << 1);
	}
	return (crc);
}

struct bitw {
	uint8_t		*p;
	uint32_t	off;
};

static void
put_bits(struct bitw *w, uint32_t val, int nbits)
{
	int i;

	for (i = nbits - 1; i >= 0; i--) {
		if (((val >> i) & 1) != 0)
			w->p[w->off / 8] |= (uint8_t)(0x80 >> (w->off & 7));
		w->off++;
	}
}

size_t
msbc_encode(struct msbc_enc *e, const int16_t *pcm, uint8_t *out)
{
	float sb[MSBC_BLOCKS][BANDS];
	float maxv[BANDS];
	struct bitw w;
	uint8_t crc;
	int blk, k, sf, bits;
	float delta, levels, v;
	long q;

	for (k = 0; k < BANDS; k++)
		maxv[k] = 0;
	for (blk = 0; blk < MSBC_BLOCKS; blk++) {
		analyse(e, pcm + blk * BANDS, sb[blk]);
		for (k = 0; k < BANDS; k++) {
			v = fabsf(sb[blk][k]);
			if (v > maxv[k])
				maxv[k] = v;
		}
	}

	/* Scale factors: the smallest n with |sample| < 2^(n+1). */
	for (k = 0; k < BANDS; k++) {
		sf = 0;
		while (sf < 15 && maxv[k] >= (float)(1 << (sf + 1)))
			sf++;
		e->alloc.scalefactor[0][k] = (uint8_t)sf;
	}
	sbc_bit_alloc(&e->alloc);

	(void)memset(out, 0, MSBC_FRAME_LEN);
	out[0] = MSBC_SYNCWORD;
	out[1] = 0;			/* reserved */
	out[2] = 0;			/* reserved */
	crc = crc_bits(0x0f, out[1], 8);
	crc = crc_bits(crc, out[2], 8);
	w.p = out;
	w.off = 32;
	for (k = 0; k < BANDS; k++) {
		put_bits(&w, e->alloc.scalefactor[0][k], 4);
		crc = crc_bits(crc, e->alloc.scalefactor[0][k], 4);
	}
	out[3] = crc;

	/* Quantise (§12.6.4) and pack, block by block. */
	for (blk = 0; blk < MSBC_BLOCKS; blk++) {
		for (k = 0; k < BANDS; k++) {
			bits = e->alloc.bits[0][k];
			if (bits == 0)
				continue;
			levels = (float)((1 << bits) - 1);
			delta = (float)(1 << (e->alloc.scalefactor[0][k] + 1));
			q = (long)floorf((sb[blk][k] / delta + 1.0f) *
			    levels / 2.0f);
			if (q < 0)
				q = 0;
			else if (q > (long)levels)
				q = (long)levels;
			put_bits(&w, (uint32_t)q, bits);
		}
	}
	return (MSBC_FRAME_LEN);
}
