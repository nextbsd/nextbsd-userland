/*
 * audio.h — bluetoothd's audio policy: the A2DP source (#218), the profile
 * routing HFP (#525) hooks into, and State:/Bluetooth/Device/<addr>/Audio.
 *
 * A2DP SOURCE (the peer is a headset or speaker: its bond has A2DPSink)
 *
 *   start     when the ACL link to a paired sink comes up, whoever paged
 *             whom, and its Audio switch is on: bluetoothd runs NextBSD's
 *             base mixer with the Bluetooth backend,
 *               /usr/sbin/virtual_oss -S -C 2 -c 2 -r 48000 -b 16 -s 768
 *                 -R /dev/null -P /dev/bluetooth/<AA:BB:CC:DD:EE:FF>
 *                 -d dsp.btN [-d dsp] -t dsp.btN.ctl
 *             (child.c). voss_bt.so does the SDP query for the AVDTP PSM,
 *             the AVDTP signalling and the SBC encoding itself; bluetoothd
 *             owns the link, the keys and the decision. With AutoSwitchAudio
 *             on, the first headset also takes /dev/dsp, so programs that
 *             open the default device play to it; virtual_oss turns
 *             hw.snd.basename_clone off for that and back on as it exits,
 *             and bluetoothd puts it back if virtual_oss could not.
 *   stop      when the link goes (or the device is unpaired, its switch is
 *             turned off, or the controller goes): SIGTERM, SIGKILL after
 *             AUDIO_STOP_GRACE_MS. virtual_oss never gives up on a sink that
 *             has gone, it retries every two seconds and pages the headset
 *             each time, so it must not outlive the link.
 *   restart   if it exits while the link is up, with a backoff
 *             (AUDIO_RESTART_MIN_MS doubling to AUDIO_RESTART_MAX_MS).
 *   AVRCP     once the mixer runs, the AVRCP session (avrcp.c) is opened
 *             if the headset has not opened it itself.
 *
 * ROUTING (the hook HFP uses)
 *
 *   A device's audio goes over one profile at a time: AUDIO_ROUTE_A2DP (the
 *   mixer plays to the headset, -R /dev/null -P /dev/bluetooth/<addr>) or
 *   AUDIO_ROUTE_HFP (the hands-free gateway, hfp.c, owns the SCO link and
 *   the mixer records from and plays to its bridge, -R/-P /dev/btscoN,
 *   sco_bridge.h; the AVDTP stream closes). audio_route_to() switches
 *   through the mixer's control device; the mixer keeps running and keeps
 *   /dev/dsp.btN (and /dev/dsp), so programs that have it open are not
 *   disturbed: on HFP the same device simply gains a microphone.
 *
 * A2DP SINK (the peer is a phone: its bond has A2DPSource) is a2dp_sink.c.
 * Both roles share PSM 0x19 by the rule there.
 *
 * Threads: the audio state lives on one serial dispatch queue,
 * audio_queue(), which avrcp.c and a2dp_sink.c share. The *_locked calls
 * are made by the policy layer (connections.c) with bluetoothd_lock() held
 * and only queue work; code on the audio queue takes bluetoothd_lock()
 * itself (never the other way round) to report a profile up or down.
 */
#ifndef _BLUETOOTHD_AUDIO_H_
#define _BLUETOOTHD_AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

#include <dispatch/dispatch.h>

#include "bluetooth_mig_types.h"
#include "engine.h"

#define	AUDIO_MIXER		"/usr/sbin/virtual_oss"
#define	AUDIO_START_DELAY_MS	1500	/* after ACL up: let encryption finish */
#define	AUDIO_STOP_GRACE_MS	3000
#define	AUDIO_RESTART_MIN_MS	2000
#define	AUDIO_RESTART_MAX_MS	30000
#define	AUDIO_AVRCP_WAIT_MS	3000	/* then we open AVRCP ourselves */
#define	AUDIO_MAX		4	/* headsets playing at once */

/* Which profile carries a device's audio. Wire values: append only. */
typedef enum {
	AUDIO_ROUTE_NONE	= 0,
	AUDIO_ROUTE_A2DP	= 1,
	AUDIO_ROUTE_HFP		= 2
} audio_route_t;

/* Our role towards the peer, as bt_audio_status reports it. */
typedef enum {
	AUDIO_ROLE_NONE		= 0,
	AUDIO_ROLE_SOURCE	= 1,	/* we play to it (A2DP source) */
	AUDIO_ROLE_SINK		= 2	/* it plays to us (A2DP sink) */
} audio_role_t;

/* What State:/Bluetooth/Device/<addr>/Audio and bt_audio_status show. */
struct audio_status {
	struct bt_addr	peer;
	audio_role_t	role;
	audio_route_t	route;
	bool		active;		/* mixer up (source) / stream up (sink) */
	bool		avrcp;		/* the AVRCP session is up */
	bool		default_out;	/* this mixer owns /dev/dsp */
	int		volume;		/* 0..127; -1 unknown */
	bool		hfp;		/* hands-free service level connection */
	bool		hfp_audio;	/* ...and its SCO link: the mic is live */
	int		battery;	/* percent, from the headset; -1 */
	char		node[32];	/* "dsp.bt0"; "" when none */
	char		codec[8];	/* "SBC"; on HFP "CVSD" or "mSBC" */
};

/* The serial queue every audio module runs on. Created on first use. */
dispatch_queue_t	audio_queue(void);

/* Kill mixers a crashed bluetoothd left behind. Once, at startup. */
void		audio_init(void);

/* Stop every mixer and close every session. At exit. */
void		audio_shutdown(void);

/* ---- from the policy layer (connections.c), bluetoothd_lock() held ---- */

void		audio_link_up_locked(const struct bt_addr *peer,
		    const char *node);
void		audio_link_down_locked(const struct bt_addr *peer);
void		audio_bond_updated_locked(const struct bt_addr *peer);
void		audio_adapter_down_locked(const char *node);

/* ---- the RPCs (Mach service thread, no lock held) ---------------------- */

/* Status of `peer`; false when it has no audio. */
bool		audio_status(const struct bt_addr *peer,
		    struct audio_status *out);

/* The idx-th device with audio; false past the end. */
bool		audio_status_at(int idx, struct audio_status *out);

/* Per-device switch (PairedDevices/<addr>/Audio), applied at once. */
bt_status_t	audio_set_enabled(const struct bt_addr *peer, bool on);

/* Absolute volume 0..127, sent to the headset over AVRCP. */
bt_status_t	audio_set_volume(const struct bt_addr *peer, int volume);

/* The AutoSwitchAudio preference changed; applies to the next mixer. */
void		audio_prefs_changed(void);

/* ---- the routing hook (HFP, #525) ------------------------------------- */

/*
 * Carry `peer`'s audio over `route`. AUDIO_ROUTE_HFP points the mixer's
 * record and play sides at `dev` (the SCO bridge; NULL: play to /dev/null),
 * with `codec` ("CVSD", "mSBC") for the status; AUDIO_ROUTE_A2DP points it
 * back at the headset. `force` resends an unchanged route (a restarted
 * mixer). Returns not_connected when there is no mixer for `peer`. Any
 * thread except the audio queue, no lock: it waits for the audio queue.
 */
bt_status_t	audio_route_to(const struct bt_addr *peer, audio_route_t route,
		    const char *dev, const char *codec, bool force);

/* audio_route_to(peer, route, NULL, NULL, false). */
bt_status_t	audio_route(const struct bt_addr *peer, audio_route_t route);

/* ---- from avrcp.c and a2dp_sink.c (audio queue) ------------------------ */

void		audio_avrcp_changed(const struct bt_addr *peer, bool up);
void		audio_volume_changed(const struct bt_addr *peer, int volume);
void		audio_sink_changed(const struct bt_addr *peer, bool up,
		    const char *codec);

/*
 * From hfp.c (any thread; queued): the service level connection is up or
 * down, its SCO link up or down, the headset's battery (percent, -1).
 */
void		audio_hfp_changed(const struct bt_addr *peer, bool slc,
		    bool sco, int battery);

/* Is `peer` a device we play to (its bond has A2DPSink)? Any thread. */
bool		audio_peer_is_sink(const struct bt_addr *peer);

/* ---- publication (main loop, no lock) ---------------------------------- */

void		audio_publish(void);

#endif /* _BLUETOOTHD_AUDIO_H_ */
