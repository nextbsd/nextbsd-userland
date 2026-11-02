/*
 * avrcp.h — AVRCP (#220): a headset's buttons as media keys, and its volume.
 *
 * One AVCTP control channel (L2CAP PSM 0x17) per headset. bluetoothd listens
 * on PSM 0x17 for any paired device, and opens the channel itself when the
 * headset has not done so a few seconds after the mixer started (audio.c).
 *
 *   target      (TG, category 1) for the headset's keys: UNIT INFO and
 *               SUBUNIT INFO are answered; PASS THROUGH PLAY, PAUSE, STOP,
 *               FORWARD, BACKWARD, VOLUME UP/DOWN, MUTE, FAST FORWARD and
 *               REWIND are accepted and delivered; GetCapabilities says
 *               who we are; everything else is NOT IMPLEMENTED, so a
 *               headset probing for metadata keeps the link.
 *   keys        per session, one IOHIDUserDevice with a Consumer Control
 *               collection (usage page 0x0C: Play/Pause 0xCD for both PLAY
 *               and PAUSE, Scan Next 0xB5, Scan Previous 0xB6, Stop 0xB7,
 *               Volume Increment 0xE9, Decrement 0xEA, Mute 0xE2, Fast
 *               Forward 0xB3, Rewind 0xB4), Transport "Bluetooth". hidbus
 *               attaches hcons, and the keys reach evdev and X as the media
 *               keys of a USB keyboard do. Cancelled with the session; a key
 *               still held is released first.
 *   controller  (CT, category 2) for absolute volume, as AVRCP 1.4 has the
 *               source do: RegisterNotification(EVENT_VOLUME_CHANGED)
 *               follows the headset's own volume (re-registered after each
 *               CHANGED), SetAbsoluteVolume sets it. The value (0-127) is
 *               published by audio.c.
 *
 * SDP: the AVRCP Target (0x110C) and Controller (0x110E) records are
 * bluetoothd's (sdp_server.h), on from start-up with the A2DP Source one.
 *
 * Everything runs on the audio queue (audio.h).
 */
#ifndef _BLUETOOTHD_AVRCP_H_
#define _BLUETOOTHD_AVRCP_H_

#include <stdbool.h>

#include "bluetooth_mig_types.h"
#include "engine.h"

#define	AVCTP_PSM		0x0017
#define	AVRCP_MAX		4	/* sessions at once */
#define	AVRCP_CONNECT_MS	15000

/*
 * Publish the source and AVRCP records and open the PSM 0x17 listener.
 * Any thread; queued. Fails quietly until the L2CAP sockets are loaded, so
 * the controller bring-up calls it again.
 */
void		avrcp_start(void);

/* Open the channel to `peer` unless one is up or being opened. Audio queue. */
void		avrcp_connect(const struct bt_addr *peer);

/* Close the session with `peer`, if any. Audio queue. */
void		avrcp_disconnect(const struct bt_addr *peer);

/* SetAbsoluteVolume (0..127). Audio queue. */
bt_status_t	avrcp_set_volume(const struct bt_addr *peer, int volume);

#endif /* _BLUETOOTHD_AVRCP_H_ */
