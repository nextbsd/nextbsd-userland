/*
 * msbc.h — the mSBC encoder for the hands-free gateway (#525).
 *
 * mSBC is SBC with one fixed configuration (sbc_decode.h, MSBC_*): 16 kHz
 * mono, 15 blocks of 8 subbands, loudness allocation, bitpool 26. The
 * decoder is sbc_decode.c's (sbc_dec_msbc); this is the other half: the
 * analysis filter bank, the scale factors, the shared bit allocation
 * (sbc_bit_alloc) and the bit packing, with the frame's CRC-8.
 *
 * Over SCO each frame travels with the two-byte H2 synchronisation header
 * and one byte of padding, 60 bytes per 7.5 ms (HFP 1.8, 5.7.4; sco_bridge.c
 * does that framing).
 */
#ifndef _BLUETOOTHD_MSBC_H_
#define _BLUETOOTHD_MSBC_H_

#include <stddef.h>
#include <stdint.h>

#include "sbc_decode.h"

struct msbc_enc {
	float		x[80];		/* analysis history */
	struct sbc_dec	alloc;		/* scale factors and bits */
};

/* A new stream. */
void	msbc_enc_reset(struct msbc_enc *e);

/*
 * Encode MSBC_SAMPLES samples into one MSBC_FRAME_LEN-byte frame (from the
 * 0xAD syncword). Returns MSBC_FRAME_LEN.
 */
size_t	msbc_encode(struct msbc_enc *e, const int16_t *pcm, uint8_t *out);

#endif /* _BLUETOOTHD_MSBC_H_ */
