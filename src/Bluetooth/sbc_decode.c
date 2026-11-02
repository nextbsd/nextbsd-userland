/*-
 * Copyright (c) 2015 Nathanial Sloss <nathanialsloss@yahoo.com.au>
 *
 *		This software is dedicated to the memory of -
 *	   Baron James Anlezark (Barry) - 1 Jan 1949 - 13 May 2012.
 *
 *		Barry was a man who loved his music.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * sbc_decode.c — the SBC decoder. See sbc_decode.h.
 *
 * Ported from virtual_oss's lib/virtual_oss/bt/sbc_encode.c (the decode half,
 * the bit allocation it shares with the encoder, and the synthesis filter
 * tables from cosdata-gen/cosdata.c). The arithmetic is unchanged except for
 * the dequantiser's scale (see synthesise()); what changed is the shape: one self-contained decoder state, the configuration
 * read from each frame's header instead of a negotiated struct, and output
 * written straight into the caller's PCM buffer.
 */
#include "sbc_decode.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

/* The 80- and 40-tap prototype filters (A2DP 1.3, Tables 12.23, 12.24). */
static const double proto8[80] = {
	0.00000000e+00, 1.56575398e-04, 3.43256425e-04, 5.54620202e-04,
	8.23919506e-04, 1.13992507e-03, 1.47640169e-03, 1.78371725e-03,
	2.01182542e-03, 2.10371989e-03, 1.99454554e-03, 1.61656283e-03,
	9.02154502e-04, -1.78805361e-04, -1.64973098e-03, -3.49717454e-03,
	5.65949473e-03, 8.02941163e-03, 1.04584443e-02, 1.27472335e-02,
	1.46525263e-02, 1.59045603e-02, 1.62208471e-02, 1.53184106e-02,
	1.29371806e-02, 8.85757540e-03, 2.92408442e-03, -4.91578024e-03,
	-1.46404076e-02, -2.61098752e-02, -3.90751381e-02, -5.31873032e-02,
	6.79989431e-02, 8.29847578e-02, 9.75753918e-02, 1.11196689e-01,
	1.23264548e-01, 1.33264415e-01, 1.40753505e-01, 1.45389847e-01,
	1.46955068e-01, 1.45389847e-01, 1.40753505e-01, 1.33264415e-01,
	1.23264548e-01, 1.11196689e-01, 9.75753918e-02, 8.29847578e-02,
	-6.79989431e-02, -5.31873032e-02, -3.90751381e-02, -2.61098752e-02,
	-1.46404076e-02, -4.91578024e-03, 2.92408442e-03, 8.85757540e-03,
	1.29371806e-02, 1.53184106e-02, 1.62208471e-02, 1.59045603e-02,
	1.46525263e-02, 1.27472335e-02, 1.04584443e-02, 8.02941163e-03,
	-5.65949473e-03, -3.49717454e-03, -1.64973098e-03, -1.78805361e-04,
	9.02154502e-04, 1.61656283e-03, 1.99454554e-03, 2.10371989e-03,
	2.01182542e-03, 1.78371725e-03, 1.47640169e-03, 1.13992507e-03,
	8.23919506e-04, 5.54620202e-04, 3.43256425e-04, 1.56575398e-04,
};

static const double proto4[40] = {
	0.00000000e+00, 5.36548976e-04, 1.49188357e-03, 2.73370904e-03,
	3.83720193e-03, 3.89205149e-03, 1.86581691e-03, -3.06012286e-03,
	1.09137620e-02, 2.04385087e-02, 2.88757392e-02, 3.21939290e-02,
	2.58767811e-02, 6.13245186e-03, -2.88217274e-02, -7.76463494e-02,
	1.35593274e-01, 1.94987841e-01, 2.46636662e-01, 2.81828203e-01,
	2.94315332e-01, 2.81828203e-01, 2.46636662e-01, 1.94987841e-01,
	-1.35593274e-01, -7.76463494e-02, -2.88217274e-02, 6.13245186e-03,
	2.58767811e-02, 3.21939290e-02, 2.88757392e-02, 2.04385087e-02,
	-1.09137620e-02, -3.06012286e-03, 1.86581691e-03, 3.89205149e-03,
	3.83720193e-03, 2.73370904e-03, 1.49188357e-03, 5.36548976e-04,
};

/* Loudness offset allocations. */
static const int loudnessoffset8[4][8] = {
	{-2, 0, 0, 0, 0, 0, 0, 1},
	{-3, 0, 0, 0, 0, 0, 1, 2},
	{-4, 0, 0, 0, 0, 0, 1, 2},
	{-4, 0, 0, 0, 0, 0, 1, 2},
};

static const int loudnessoffset4[4][4] = {
	{-1, 0, 0, 0},
	{-2, 0, 0, 1},
	{-2, 0, 0, 1},
	{-2, 0, 0, 1},
};

static const int rates[4] = { 16000, 32000, 44100, 48000 };

/* The synthesis tables, built once (cosdata.c's formulas). */
static float		coeffs8[80], coeffs4[40];
static float		cosdec8[8][16], cosdec4[4][8];
static pthread_once_t	tables_once = PTHREAD_ONCE_INIT;

static void
tables_init(void)
{
	int i, k;

	for (i = 0; i < 80; i++)
		coeffs8[i] = (float)proto8[i];
	for (i = 0; i < 40; i++)
		coeffs4[i] = (float)proto4[i];
	for (i = 0; i < 8; i++) {
		for (k = 0; k < 16; k++)
			cosdec8[i][k] = cosf((float)((i + 0.5) * (k + 4) *
			    (M_PI / 8.0)));
	}
	for (i = 0; i < 4; i++) {
		for (k = 0; k < 8; k++)
			cosdec4[i][k] = cosf((float)((i + 0.5) * (k + 2) *
			    (M_PI / 4.0)));
	}
}

void
sbc_dec_reset(struct sbc_dec *d)
{
	(void)pthread_once(&tables_once, tables_init);
	(void)memset(d, 0, sizeof(*d));
}

static uint32_t
load_bits(struct sbc_dec *d, uint32_t numbits)
{
	uint32_t off = d->off;
	uint32_t value = 0;

	for (; numbits > 0; numbits--) {
		if (off >= d->max) {
			d->truncated = true;
			break;
		}
		if (d->p[off / 8] & (1 << ((7 - off) & 7)))
			value |= (1U << (numbits - 1));
		off++;
	}
	d->off = off;
	return (value);
}

/* The bit allocation (A2DP 1.3, §12.6.3): sbc_encode.c's calc_bitneed. */
static void
calc_bitneed(struct sbc_dec *d)
{
	int32_t bitneed[2][8];
	int32_t max_bitneed, bitcount;
	int32_t slicecount, bitslice;
	int32_t loudness;
	int ch, sb, start_chan = 0;
	int channels = d->channels;

	if (d->mode == SBC_MODE_DUAL)
		channels = 1;

next_chan:
	max_bitneed = 0;
	bitcount = 0;
	slicecount = 0;

	if (d->alloc == 1) {		/* SNR */
		for (ch = start_chan; ch < channels; ch++) {
			for (sb = 0; sb < d->bands; sb++) {
				bitneed[ch][sb] = d->scalefactor[ch][sb];
				if (bitneed[ch][sb] > max_bitneed)
					max_bitneed = bitneed[ch][sb];
			}
		}
	} else {			/* loudness */
		for (ch = start_chan; ch < channels; ch++) {
			for (sb = 0; sb < d->bands; sb++) {
				if (d->scalefactor[ch][sb] == 0) {
					bitneed[ch][sb] = -5;
				} else {
					if (d->bands == 8)
						loudness =
						    d->scalefactor[ch][sb] -
						    loudnessoffset8[d->freq][sb];
					else
						loudness =
						    d->scalefactor[ch][sb] -
						    loudnessoffset4[d->freq][sb];
					if (loudness > 0)
						bitneed[ch][sb] = loudness / 2;
					else
						bitneed[ch][sb] = loudness;
				}
				if (bitneed[ch][sb] > max_bitneed)
					max_bitneed = bitneed[ch][sb];
			}
		}
	}

	slicecount = bitcount = 0;
	bitslice = max_bitneed + 1;
	do {
		bitslice--;
		bitcount += slicecount;
		slicecount = 0;
		for (ch = start_chan; ch < channels; ch++) {
			for (sb = 0; sb < d->bands; sb++) {
				if ((bitneed[ch][sb] > bitslice + 1) &&
				    (bitneed[ch][sb] < bitslice + 16))
					slicecount++;
				else if (bitneed[ch][sb] == bitslice + 1)
					slicecount += 2;
			}
		}
	} while (bitcount + slicecount < d->bitpool);

	/* check if exactly one more fits */
	if (bitcount + slicecount == d->bitpool) {
		bitcount += slicecount;
		bitslice--;
	}
	for (ch = start_chan; ch < channels; ch++) {
		for (sb = 0; sb < d->bands; sb++) {
			if (bitneed[ch][sb] < bitslice + 2) {
				d->bits[ch][sb] = 0;
			} else {
				d->bits[ch][sb] = bitneed[ch][sb] - bitslice;
				if (d->bits[ch][sb] > 16)
					d->bits[ch][sb] = 16;
			}
		}
	}

	ch = (d->mode == SBC_MODE_DUAL) ? start_chan : 0;
	sb = 0;
	while (bitcount < d->bitpool && sb < d->bands) {
		if ((d->bits[ch][sb] >= 2) && (d->bits[ch][sb] < 16)) {
			d->bits[ch][sb]++;
			bitcount++;
		} else if ((bitneed[ch][sb] == bitslice + 1) &&
		    (d->bitpool > bitcount + 1)) {
			d->bits[ch][sb] = 2;
			bitcount += 2;
		}
		if (channels == 1 || start_chan == 1)
			sb++;
		else if (ch == 1) {
			ch = 0;
			sb++;
		} else
			ch = 1;
	}

	ch = (d->mode == SBC_MODE_DUAL) ? start_chan : 0;
	sb = 0;
	while (bitcount < d->bitpool && sb < d->bands) {
		if (d->bits[ch][sb] < 16) {
			d->bits[ch][sb]++;
			bitcount++;
		}
		if (channels == 1 || start_chan == 1)
			sb++;
		else if (ch == 1) {
			ch = 0;
			sb++;
		} else
			ch = 1;
	}

	if (d->mode == SBC_MODE_DUAL && start_chan == 0) {
		start_chan = 1;
		channels = 2;
		goto next_chan;
	}
}

/* Dequantise, undo joint stereo, and run the synthesis filter bank. */
static void
synthesise(struct sbc_dec *d, int16_t *pcm)
{
	float delta[2][8];
	float levels[2][8];
	float audioout;
	float U[160];
	float W[160];
	float S[8];
	float x, *V;
	int position;
	int block;
	int chan;
	int sb;
	int i;
	int k;

	for (chan = 0; chan < d->channels; chan++) {
		for (sb = 0; sb < d->bands; sb++) {
			levels[chan][sb] = (float)((1 << d->bits[chan][sb]) - 1);
			/*
			 * 2^(scale_factor + 1), as A2DP 1.3 §12.6.4 has it.
			 * virtual_oss used 2^scale_factor and played 6 dB low
			 * against its own encoder.
			 */
			delta[chan][sb] =
			    (float)(1 << (d->scalefactor[chan][sb] + 1));
		}
	}

	i = 0;
	for (block = 0; block < d->blocks; block++) {
		for (chan = 0; chan < d->channels; chan++) {
			for (sb = 0; sb < d->bands; sb++) {
				if (d->bits[chan][sb] == 0) {
					audioout = 0;
				} else {
					audioout = ((((d->sb[i] * 2.0f) +
					    1.0f) * delta[chan][sb]) /
					    levels[chan][sb]) -
					    delta[chan][sb];
				}
				d->sb[i++] = audioout;
			}
		}
	}

	if (d->mode == SBC_MODE_JOINT) {
		i = 0;
		while (i < (d->blocks * d->bands * d->channels)) {
			for (sb = 0; sb < d->bands; sb++) {
				if (d->join & (1 << (d->bands - sb - 1))) {
					audioout = d->sb[i];
					d->sb[i] = (2.0f * d->sb[i]) +
					    (2.0f * d->sb[i + d->bands]);
					d->sb[i + d->bands] =
					    (2.0f * audioout) -
					    (2.0f * d->sb[i + d->bands]);
					d->sb[i] /= 2.0f;
					d->sb[i + d->bands] /= 2.0f;
				}
				i++;
			}
			i += d->bands;
		}
	}

	position = 0;
	for (block = 0; block < d->blocks; block++) {
		for (chan = 0; chan < d->channels; chan++) {
			V = d->v[chan];
			for (i = 0; i < d->bands; i++)
				S[i] = d->sb[position++];

			for (i = (d->bands * 20) - 1; i >= (d->bands * 2); i--)
				V[i] = V[i - (d->bands * 2)];
			for (k = 0; k < d->bands * 2; k++) {
				float vk = 0;

				for (i = 0; i < d->bands; i++) {
					if (d->bands == 8)
						vk += cosdec8[i][k] * S[i];
					else
						vk += cosdec4[i][k] * S[i];
				}
				V[k] = vk;
			}
			for (i = 0; i <= 4; i++) {
				for (k = 0; k < d->bands; k++) {
					U[(i * d->bands * 2) + k] =
					    V[(i * d->bands * 4) + k];
					U[(i * d->bands * 2) + d->bands + k] =
					    V[(i * d->bands * 4) +
					    (d->bands * 3) + k];
				}
			}
			for (i = 0; i < d->bands * 10; i++) {
				if (d->bands == 8)
					W[i] = U[i] * (coeffs8[i] * -8.0f);
				else
					W[i] = U[i] * (coeffs4[i] * -4.0f);
			}
			for (k = 0; k < d->bands; k++) {
				x = 0;
				for (i = 0; i < 10; i++)
					x += W[k + (i * d->bands)];
				if (x > 32767.0f)
					x = 32767.0f;
				else if (x < -32767.0f)
					x = -32767.0f;
				pcm[((block * d->bands) + k) * d->channels +
				    chan] = (int16_t)x;
			}
		}
	}
}

size_t
sbc_dec_frame(struct sbc_dec *d, const uint8_t *buf, size_t len,
    int16_t *pcm, int *nframes)
{
	uint8_t config;
	int block, chan, sb, i, j;

	*nframes = 0;
	if (len < 4 || buf[0] != SBC_SYNCWORD)
		return (0);
	config = buf[1];
	d->freq = (config >> 6) & 3;
	d->rate = rates[d->freq];
	d->blocks = 4 * (((config >> 4) & 3) + 1);
	d->mode = (config >> 2) & 3;
	d->alloc = (config >> 1) & 1;
	d->bands = (config & 1) ? 8 : 4;
	d->channels = (d->mode == SBC_MODE_MONO) ? 1 : 2;
	d->bitpool = buf[2];
	/* buf[3] is the CRC; virtual_oss does not check it either. */

	d->p = buf;
	d->truncated = false;
	d->off = 32;
	d->max = (uint32_t)(len > 8192 ? 8192 : len) * 8;

	if (d->mode == SBC_MODE_JOINT)
		d->join = (uint8_t)load_bits(d, (uint32_t)d->bands);
	else
		d->join = 0;
	for (i = 0; i < d->channels; i++) {
		for (j = 0; j < d->bands; j++)
			d->scalefactor[i][j] = (uint8_t)load_bits(d, 4);
	}

	calc_bitneed(d);

	i = 0;
	for (block = 0; block < d->blocks; block++) {
		for (chan = 0; chan < d->channels; chan++) {
			for (sb = 0; sb < d->bands; sb++) {
				if (d->bits[chan][sb] == 0) {
					d->sb[i++] = 0;
					continue;
				}
				d->sb[i++] = (float)load_bits(d,
				    (uint32_t)d->bits[chan][sb]);
			}
		}
	}
	if (d->truncated)
		return (0);

	synthesise(d, pcm);
	*nframes = d->blocks * d->bands;
	return ((d->off + 7) / 8);
}
