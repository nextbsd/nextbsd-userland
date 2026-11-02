/*
 * sco_bridge.h — the hands-free gateway's audio path (#525): a headset's SCO
 * link as a sound device the headset's mixer can record from and play to.
 *
 *   programs ── /dev/dsp.btN (and /dev/dsp) ── virtual_oss (the headset's
 *   mixer, audio.c) ── -R/-P /dev/btscoN ── bluetoothd (this) ── SCO ── mic
 *                                                                  and ear
 *
 * Each bridge is a character device bluetoothd serves through cuse(3),
 * /dev/btsco<slot> (root only: the mixer is its one user). It speaks just
 * enough OSS for virtual_oss's oss backend: signed 16-bit little-endian,
 * two channels, 48 kHz, which is the mixer's own format, so the mixer
 * needs no conversion. audio_route_to() points the mixer's -R and -P at it
 * while the device is on HFP; A2DP points them back.
 *
 * Behind it, one thread per SCO link reads a packet, converts it and writes
 * one of the same size back, so the link sets the pace in both directions
 * (ng_btsocket_sco_sync.h: there is no flow control):
 *
 *   CVSD  each packet is 16-bit linear PCM at 8 kHz (the controller does the
 *         CVSD; voice setting 0x0060), resampled 8 <-> 48 kHz.
 *   mSBC  each packet is a slice of a byte stream of 60-byte units: the H2
 *         header, a 57-byte mSBC frame, one byte of padding (HFP 1.8,
 *         5.7.4), decoded with sbc_decode.c, encoded with msbc.c, resampled
 *         16 <-> 48 kHz.
 *
 * With no SCO link the device still runs: reads return silence at the
 * mixer's rate and writes are dropped, so the mixer never sees an error and
 * never falls into its two-second reopen loop while the link is set up or
 * torn down. The devices are created on first use and kept.
 *
 * Threads: sco_bridge_* are called from the hands-free queue (hfp.c). The
 * cuse workers and the SCO threads are bluetoothd's own, with every signal
 * blocked: libcuse interrupts a worker with SIGHUP, which would otherwise
 * reach bluetoothd's terminate handler.
 */
#ifndef _BLUETOOTHD_SCO_BRIDGE_H_
#define _BLUETOOTHD_SCO_BRIDGE_H_

#include <stdbool.h>
#include <stddef.h>

#include <dispatch/dispatch.h>

#define	SCO_BRIDGE_MAX		4	/* SCO links at once */
#define	SCO_BRIDGE_RATE		48000	/* the mixer's (audio.c) */
#define	SCO_BRIDGE_CHANNELS	2

/* HFP codec ids (HFP 1.8, 10, "Codec IDs"); AT+BAC and +BCS use them. */
typedef enum {
	SCO_CODEC_CVSD	= 1,
	SCO_CODEC_MSBC	= 2
} sco_codec_t;

/*
 * The bridge device of `slot` (0 .. SCO_BRIDGE_MAX-1), created on first use:
 * its path ("/dev/btsco0") into `path`. Returns 0, or -1 when cuse is not
 * available.
 */
int	sco_bridge_device(int slot, char *path, size_t len);

/*
 * Carry the connected SCO socket `fd` through `slot`'s device with `codec`.
 * The bridge owns `fd` from here, also on failure. When the link ends by
 * itself (the headset closed it, the ACL went), `ended(ctx, slot)` is called
 * on `q`; not after sco_bridge_stop(). Returns 0 or -1.
 */
int	sco_bridge_start(int slot, int fd, sco_codec_t codec,
	    dispatch_queue_t q, void (*ended)(void *ctx, int slot), void *ctx);

/* Close `slot`'s SCO socket and wait for its thread (at most ~0.2 s). */
void	sco_bridge_stop(int slot);

/* Is `slot` carrying a link? */
bool	sco_bridge_active(int slot);

#endif /* _BLUETOOTHD_SCO_BRIDGE_H_ */
