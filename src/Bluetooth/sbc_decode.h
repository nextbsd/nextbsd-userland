/*
 * sbc_decode.h — an SBC decoder for the A2DP sink (#219).
 *
 * SBC is the one codec every A2DP device must speak (A2DP 1.3, §4.3). Each
 * frame describes itself: its header carries the sampling frequency, the
 * block and subband counts, the channel mode, the allocation method and the
 * bitpool, so the decoder needs no configuration beyond a reset when a new
 * stream starts.
 *
 * The decoder is a port of virtual_oss's (lib/virtual_oss/bt/sbc_encode.c,
 * BSD-licensed, Nathanial Sloss), which virtual_bt_speaker used. It runs in
 * bluetoothd instead: no FreeBSD Bluetooth binary is involved.
 */
#ifndef _BLUETOOTHD_SBC_DECODE_H_
#define _BLUETOOTHD_SBC_DECODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define	SBC_SYNCWORD		0x9c
#define	SBC_MAX_SAMPLES		128	/* per channel: 16 blocks x 8 bands */

/* Channel modes, as the frame header and the A2DP capability encode them. */
#define	SBC_MODE_MONO		0
#define	SBC_MODE_DUAL		1
#define	SBC_MODE_STEREO		2
#define	SBC_MODE_JOINT		3

struct sbc_dec {
	/* The last frame's header. */
	int		freq;		/* 0 16 kHz, 1 32, 2 44.1, 3 48 */
	int		rate;		/* in Hz */
	int		blocks;		/* 4, 8, 12, 16 */
	int		bands;		/* 4, 8 */
	int		mode;		/* SBC_MODE_* */
	int		alloc;		/* 0 loudness, 1 SNR */
	int		channels;	/* 1, 2 */
	int		bitpool;

	/* Decoder state. */
	uint8_t		join;
	uint8_t		scalefactor[2][8];
	int		bits[2][8];
	float		sb[16 * 2 * 8];		/* subband samples */
	float		v[2][160];		/* synthesis history */

	/* Bit reader. */
	const uint8_t	*p;
	uint32_t	off;
	uint32_t	max;
	bool		truncated;	/* the frame ran past the buffer */
};

/* Forget the synthesis history: a new stream. */
void	sbc_dec_reset(struct sbc_dec *d);

/*
 * Decode one frame from `buf` (at most `len` bytes). On success it returns
 * the frame's length in bytes and writes `*nframes` samples per channel,
 * interleaved, into `pcm` (room for 2 * SBC_MAX_SAMPLES), with d->channels
 * and d->rate describing them. Returns 0 when `buf` does not start with a
 * frame (no syncword, or too short): the caller drops the rest of the packet.
 */
size_t	sbc_dec_frame(struct sbc_dec *d, const uint8_t *buf, size_t len,
	    int16_t *pcm, int *nframes);

#endif /* _BLUETOOTHD_SBC_DECODE_H_ */
