/*
 * hfp.h — the Hands-Free Profile Audio Gateway (#525): a headset's
 * microphone and earpiece for calls.
 *
 * bluetoothd is the AG (the phone's role, HFP 1.8); the headset is the HF.
 *
 *   SDP        the Handsfree AG record (0x111F) on RFCOMM channel
 *              HFP_RFCOMM_CHANNEL, published at start-up, with wide band
 *              speech; Class of Device Audio and Telephony
 *              (adapter_set_cod_services_locked, ADAPTER_COD_HFP).
 *   SLC        the service level connection: one RFCOMM channel per
 *              headset. bluetoothd listens on HFP_RFCOMM_CHANNEL for any
 *              paired device, and opens it itself HFP_CONNECT_WAIT_MS after
 *              the ACL link to a paired headset (its bond has HFP, its Audio
 *              switch is on) came up if the headset did not, to the
 *              headset's Handsfree record's channel (sdp_client.h).
 *   AT         AT+BRSF, +BAC, +CIND=?/?, +CMER, +CHLD=? (the SLC), then
 *              +BCS (codec), +BCC, +VGS, +VGM, +NREC, +CLIP, +CCWA, +CMEE,
 *              +BIA, +CLCC, +COPS, +CNUM, +BTRH, ATA, +CHUP, +CKPD, and
 *              Apple's +XAPL / +IPHONEACCEV (headset battery). Everything
 *              else is ERROR. There are no calls: the indicators say
 *              "service, no call".
 *   codec      with codec negotiation on both sides, +BCS:2 (mSBC, wide
 *              band, 16 kHz) when the headset listed it in AT+BAC, else
 *              +BCS:1 (CVSD, 8 kHz); without it, CVSD. An mSBC link the
 *              controller refuses falls back to CVSD for the session.
 *   SCO        bluetoothd opens it (BLUETOOTH_PROTO_SCO with
 *              SO_SCO_SYNC_PARAMS, ng_btsocket_sco_sync.h), trying the HFP
 *              parameter sets in order (T2, T1 for mSBC; S4, S3, S1, D1 for
 *              CVSD); a SCO link the headset opens is accepted (CVSD).
 *   audio      the SCO link is carried by sco_bridge.c through /dev/btscoN,
 *              and the headset's mixer (audio.c) is pointed at it with
 *              audio_route_to(AUDIO_ROUTE_HFP): /dev/dsp.btN, and /dev/dsp
 *              when the headset owns it, then record from the headset's
 *              microphone and play to its earpiece. Closing the SCO link
 *              points the mixer back at A2DP.
 *
 * When: the microphone is opened on request (hfp_set_route, `bluetooth
 * audio <dev> hfp`), when the headset asks (AT+BCC, ATA, AT+CKPD) or opens
 * SCO itself; it is closed on request (`bluetooth audio <dev> a2dp`), when
 * the headset hangs up (AT+CHUP) or closes SCO, and with the SLC or the
 * link. Nothing is switched just because a program opened the device:
 * virtual_oss tells nobody when a client starts recording.
 *
 * Threads: one serial queue, hfp_queue(). It may take bluetoothd_lock() and
 * may wait for the audio queue (audio_route_to); the audio queue only ever
 * queues work here (hfp_link_up and friends), never waits for it.
 */
#ifndef _BLUETOOTHD_HFP_H_
#define _BLUETOOTHD_HFP_H_

#include <stdbool.h>
#include <stdint.h>

#include <dispatch/dispatch.h>

#include "audio.h"
#include "bluetooth_mig_types.h"
#include "engine.h"

#define	HFP_RFCOMM_CHANNEL	13	/* sdp_server.c's default */
#define	HFP_CONNECT_WAIT_MS	3000	/* the headset's turn to open the SLC */
#define	HFP_CONNECT_MS		20000
#define	HFP_CODEC_WAIT_MS	3000	/* for AT+BCS after +BCS */

/* Class of Device service bits: Audio (21) and Telephony (22). */
#define	HFP_COD_SERVICES	0x00600000

/*
 * bt_audio_route's `route` (MIG 31085): audio_route_t's A2DP and HFP, and
 * HFP held to CVSD for this session (a headset whose mSBC sounds wrong).
 */
#define	HFP_ROUTE_HFP_CVSD	3

/* What bt_hfp_status (MIG 31086) reports for one headset. */
struct hfp_status {
	bool		slc;		/* service level connection up */
	bool		audio;		/* SCO up: the microphone is live */
	bool		pending;	/* codec negotiation or SCO setup */
	char		codec[8];	/* "CVSD", "mSBC"; "" without audio */
	char		device[32];	/* "/dev/btsco0"; "" without audio */
	uint32_t	hf_features;	/* the headset's AT+BRSF */
	int		speaker_gain;	/* AT+VGS, 0-15; -1 */
	int		mic_gain;	/* AT+VGM, 0-15; -1 */
	int		battery;	/* percent; -1 */
};

/* The serial queue the gateway runs on. Created on first use. */
dispatch_queue_t	hfp_queue(void);

/*
 * Publish the AG record and the CoD bits, and open the RFCOMM and SCO
 * listeners. Any thread; queued. Fails quietly until the Bluetooth sockets
 * are loaded, so the controller bring-up calls it again.
 */
void		hfp_start(void);

/* Close every session and listener. At exit, not on the audio queue. */
void		hfp_shutdown(void);

/* ---- from audio.c (audio queue; these only queue work) ----------------- */

void		hfp_link_up(const struct bt_addr *peer);
void		hfp_link_down(const struct bt_addr *peer);
void		hfp_bond_updated(const struct bt_addr *peer);
void		hfp_mixer_started(const struct bt_addr *peer);

/* ---- the RPCs (Mach service thread, no lock held) ---------------------- */

/*
 * Open (AUDIO_ROUTE_HFP, HFP_ROUTE_HFP_CVSD) or close (AUDIO_ROUTE_A2DP) the
 * microphone of `peer`. Returns at once: not_connected without an SLC,
 * success when the switch has started; bt_hfp_status follows it.
 */
bt_status_t	hfp_set_route(const struct bt_addr *peer, int route);

/* `peer`'s hands-free state; false when it has no SLC or session. */
bool		hfp_status(const struct bt_addr *peer, struct hfp_status *out);

#endif /* _BLUETOOTHD_HFP_H_ */
