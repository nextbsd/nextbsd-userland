/*
 * supplicant.h — wland's policy layer over stock wpa_supplicant.
 *
 * Owns, per WLAN interface: the two wpa_ctrl connections (command + event), the
 * scan-result cache, and the current association state.
 *
 * WLAND IS THE SOLE AUTHOR OF WPA_SUPPLICANT'S NETWORK BLOCKS. wpa_supplicant is
 * started with an EMPTY config and every network is injected at runtime via
 * ADD_NETWORK / SET_NETWORK / SELECT_NETWORK. This is not a stylistic choice: if
 * wpa_supplicant.conf ALSO carried SSIDs, then wpa_supplicant's own autojoin and
 * wland's autojoin would both be trying to pick a network, and they would fight.
 * That is the classic NetworkManager failure mode, and it produces exactly the
 * bug users hate — the machine keeps reconnecting to the wrong SSID and nothing
 * you click makes it stop. One brain. It is wland's.
 *
 * Known networks (SSID, PSK, autojoin, last-used) live in SCPreferences, not in
 * wpa_supplicant.conf — see airport.h. They are plaintext either way, because
 * there is no keychain and none is started; the only real choice is WHICH
 * plaintext file, and a root-owned preferences.plist is the Darwin-shaped one
 * and keeps wland the sole author. (WLAN plan Q2.)
 */
#ifndef _WLAND_SUPPLICANT_H_
#define _WLAND_SUPPLICANT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wlan_mig_types.h"

struct supplicant;

/* One cached scan result. */
struct scan_entry {
	char			ssid[WLAN_SSID_MAX + 1];
	char			bssid[18];
	int			rssi;		/* dBm */
	int			channel;
	wlan_security_t		security;
};

/* The current association, as wpa_supplicant reports it. */
struct assoc_state {
	wlan_state_t		state;
	char			ssid[WLAN_SSID_MAX + 1];
	char			bssid[18];
	int			rssi;
	int			channel;
	wlan_security_t		security;
};

/*
 * Attach to wpa_supplicant on `ifname`. Returns NULL if it is not listening yet
 * — the caller should retry; wland and wpa_supplicant are both launchd jobs with
 * no ordering guarantee (there is no StartAfter/Requires in this launchd; the
 * plist directory scan is not even sorted).
 */
struct supplicant	*supplicant_attach(const char *ifname);
void			supplicant_detach(struct supplicant *s);

/* The event-connection fd, for a read watch in the event loop. */
int	supplicant_event_fd(const struct supplicant *s);

/*
 * Drain and process every pending event. Returns true if the association state
 * CHANGED, meaning the caller should re-publish State:/.../AirPort.
 *
 * The events that matter:
 *   CTRL-EVENT-CONNECTED         4-way handshake done. NOW it is safe to DHCP.
 *   CTRL-EVENT-DISCONNECTED      association lost.
 *   CTRL-EVENT-SCAN-RESULTS      fresh scan results are available; refresh cache.
 *   CTRL-EVENT-SCAN-FAILED       the sweep failed; the cache is unchanged.
 *   CTRL-EVENT-SSID-TEMP-DISABLED  auth failed (usually a wrong PSK).
 */
bool	supplicant_pump_events(struct supplicant *s);

/* Ask for a scan. Returns immediately — results arrive as an event. */
int	supplicant_scan(struct supplicant *s);

/* Re-read SCAN_RESULTS into the cache. Returns the number of entries. */
int	supplicant_refresh_scan(struct supplicant *s);

int	supplicant_scan_count(const struct supplicant *s);
bool	supplicant_scan_get(const struct supplicant *s, int index,
	    struct scan_entry *out);

/*
 * How many sweeps have finished, successfully or not, and whether the latest
 * one failed. This is how a caller tells that a scan it requested is done.
 */
void	supplicant_scan_generation(const struct supplicant *s,
	    uint32_t *generation, bool *last_failed);

/*
 * Autojoin asks for a scan and then has to wait for it. Arm after requesting
 * the scan; once the sweep has finished, take returns true, once. A failed
 * sweep, or an explicit connect or disconnect in the meantime, disarms it.
 */
void	supplicant_autojoin_arm(struct supplicant *s);
bool	supplicant_autojoin_take(struct supplicant *s);

/*
 * Associate with `ssid` using `psk` (NULL/empty => open network). wland removes
 * any previous network block first, so there is exactly one at a time and no
 * chance of wpa_supplicant quietly autojoining a stale entry.
 */
int	supplicant_connect(struct supplicant *s, const char *ssid,
	    const char *psk);

/* Disassociate and drop the network block. */
int	supplicant_disconnect(struct supplicant *s);

/* Current association state (cached; refreshed by supplicant_pump_events). */
void	supplicant_get_state(const struct supplicant *s,
	    struct assoc_state *out);

/*
 * Re-read STATUS from wpa_supplicant and update the cached association state.
 * Returns true if it changed.
 */
bool	supplicant_refresh_state(struct supplicant *s);

#endif /* _WLAND_SUPPLICANT_H_ */
