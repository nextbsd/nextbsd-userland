/*
 * a2dp_sink.h — the A2DP sink (#219): a phone plays through this machine's
 * speakers. It replaces virtual_bt_speaker, which is not run.
 *
 *   preference  A2DPSink in the store, off by default: when on, bluetoothd
 *               publishes the AudioSink (0x110B) SDP record, adds the Audio
 *               and Rendering service bits to every controller's Class of
 *               Device, and listens on AVDTP's PSM 0x19. When off, none of
 *               that, and the kernel refuses PSM 0x19.
 *   PSM 0x19    bluetoothd owns the listener and decides per peer. A device
 *               whose bond says it is an A2DP sink (a headset) and not a
 *               source is ours to play to: the channel is closed, and the
 *               mixer (audio.h) connects out to it, as AVDTP lets either side
 *               initiate. Anything else (a phone, or a device whose services
 *               are not known yet) gets a sink session: one at a time.
 *   AVDTP       the acceptor: DISCOVER (one SBC sink endpoint),
 *               GET_(ALL_)CAPABILITIES, SET_CONFIGURATION, GET_CONFIGURATION,
 *               OPEN, START, SUSPEND, CLOSE, ABORT; everything else is
 *               rejected. The second channel to PSM 0x19 from the same peer,
 *               after OPEN, is the media channel.
 *   playback    a thread per stream reads RTP, decodes SBC (sbc_decode.c)
 *               and writes 16-bit PCM to /dev/dsp: the system output, which
 *               is virtual_oss when a mixer owns it. The device is closed on
 *               SUSPEND so other programs can have it.
 *   publish     State:/Bluetooth/Adapter/<node>/A2DPSink = { Enabled, Peer },
 *               and the peer's State:/Bluetooth/Device/<addr>/Audio
 *               (Role "Sink") through audio.c.
 *
 * Runs on the audio queue (audio.h), except the playback thread.
 */
#ifndef _BLUETOOTHD_A2DP_SINK_H_
#define _BLUETOOTHD_A2DP_SINK_H_

#include <stdbool.h>

#include "engine.h"

#define	A2DP_SINK_OUTPUT	"/dev/dsp"
#define	A2DP_SINK_SEID		1
#define	A2DP_COD_SERVICES	0x240000	/* Audio (21) + Rendering (18) */

/*
 * Apply the A2DPSink preference: open or close the listener, publish or
 * withdraw the record, set the Class of Device. Any thread; the work is
 * queued. Called once the L2CAP sockets are loaded, and on every change.
 */
void	a2dp_sink_prefs_changed(void);

/* The link to `peer` is gone: end its session. Audio queue. */
void	a2dp_sink_link_down(const struct bt_addr *peer);

/* End every session and close the listener. Audio queue. */
void	a2dp_sink_shutdown(void);

/* State:/Bluetooth/Adapter/<node>/A2DPSink. Main loop, no lock held. */
void	a2dp_sink_publish(void);

#endif /* _BLUETOOTHD_A2DP_SINK_H_ */
