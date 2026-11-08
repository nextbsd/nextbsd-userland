/*
 * airport.h — wland's SystemConfiguration surface.
 *
 * Two jobs, both CoreFoundation, kept in one translation unit so the CF header
 * pull stays contained (same arrangement as ipconfigd's sc_publish.c — the rest
 * of the daemon is plain C).
 *
 * ---------------------------------------------------------------------------
 * 1. Publish State:/Network/Interface/<if>/AirPort
 *
 *      { Authenticated, SSID, BSSID, RSSI, Channel, Security }
 *
 * This key is THE contract of the whole WLAN design. ipconfigd's DHCP gate
 * requires Link{Active} AND AirPort{Authenticated} before it will send a
 * DHCPDISCOVER, because net80211 reports the link UP at ASSOCIATION — before the
 * WPA 4-way handshake — and DHCP onto an unauthenticated port is silently
 * dropped. Nothing else in the system publishes an association-complete signal,
 * which is precisely why the "cheap path" of skipping wland was rejected: there
 * would be nothing for the gate to gate on.
 *
 * We are INVENTING this schema. SCSchemaDefinitions.h stops at
 * IPv4/IPv6/DHCP/DNS/Proxies/Interface/Link — there is no kSCEntNetAirPort, so
 * the key and field names here are the definition, not a copy of one. They are
 * chosen to match what macOS uses so a future CoreWLAN-shaped layer sees a
 * familiar shape.
 *
 * ONLY the compact connected-state summary goes in the store. Scan results do
 * NOT: CONFIG_DATA_MAX (config_types.h:74) rejects any key OR value over 8192
 * bytes, and a busy scan blows straight past that. Scan results go over wland's
 * MIG surface instead (wlan.defs).
 *
 * ---------------------------------------------------------------------------
 * 2. Known networks in SCPreferences
 *
 * SSID -> { PSK, Security, AutoJoin, LastUsed }, in the root-owned
 * preferences.plist under /Local/Library/Preferences/SystemConfiguration.
 * SCPreferences gives us atomic commit (temp file + rename) and a lockfile for
 * free.
 *
 * A WPA-Enterprise network's entry instead carries AuthenticationProperties,
 * the dictionary Darwin's 802.1X profiles use (EAPOLClientProfile.c), with
 * Darwin's key names (EAPClientProperties.h) and three org.nextbsd.* paths for
 * the files Darwin would hold in the keychain. The password (UserPassword) and
 * private key passphrase sit beside it in the entry, as a PSK does.
 *
 * The PSK is PLAINTEXT. That is not a decision we get to make: there is no
 * keychain on NextBSD, it is not started, and it is not planned — so WLAN
 * passwords are plaintext-on-disk no matter which path we take. The only real
 * choice is WHICH plaintext file, and this one keeps wland the sole author of
 * wpa_supplicant's network blocks (see supplicant.h) instead of scattering
 * credentials into /etc/wpa_supplicant.conf where wpa_supplicant's own autojoin
 * would start competing with ours. When a real keychain lands, this is the one
 * function that changes. (WLAN plan Q2; see the Keychain plan for the option
 * space.)
 */
#ifndef _WLAND_AIRPORT_H_
#define _WLAND_AIRPORT_H_

#include <stdbool.h>
#include <stddef.h>

#include "supplicant.h"		/* struct assoc_state */

struct airport;

/* Open a configd session. NULL on failure — the caller keeps running. */
struct airport	*airport_open(void);
void		airport_close(struct airport *a);

/*
 * Publish State:/Network/Interface/<ifname>/AirPort from `st`.
 *
 * Authenticated is true ONLY in wlan_state_connected_e — i.e. only after
 * CTRL-EVENT-CONNECTED. Setting it true at association would reintroduce the
 * exact bug the gate exists to fix.
 */
int	airport_publish(struct airport *a, const char *ifname,
	    const struct assoc_state *st);

/* Remove the key — the VAP is going away, or the radio is off. */
int	airport_remove(struct airport *a, const char *ifname);

/* A remembered network. */
struct known_net {
	char	ssid[33];
	char	psk[65];
	bool	autojoin;
	struct eap_config eap;		/* eap.type is 0 unless 802.1X */
};

/*
 * Remember `ssid` with `psk` (empty for open). Overwrites any existing entry.
 * Commits immediately — a network you joined should survive a power cut.
 */
int	known_net_save(const char *ssid, const char *psk, bool autojoin);

/* The same for a WPA-Enterprise network. */
int	known_net_save_eap(const char *ssid, const struct eap_config *eap,
	    bool autojoin);

/* Look a network up by SSID. Returns false if not remembered. */
bool	known_net_find(const char *ssid, struct known_net *out);

/* Forget a network. */
int	known_net_forget(const char *ssid);

/*
 * Pick the best remembered network among the current scan results — wland's
 * autojoin brain, and the thing wpa_supplicant does not have.
 *
 * Policy: highest RSSI among known, autojoin-enabled networks that are actually
 * in range. That is deliberately simpler than macOS (which also weighs
 * last-used, per-network preference order, and captive-portal history), but it
 * is the behaviour people actually expect, and it is honest about what it does.
 *
 * With skip_enterprise, WPA-Enterprise networks are passed over.
 *
 * Returns false when nothing known is in range.
 */
bool	known_net_pick(const struct scan_entry *scan, int scan_n,
	    struct known_net *out, bool skip_enterprise);

#endif /* _WLAND_AIRPORT_H_ */
