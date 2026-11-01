/*
 * vap.h — 802.11 PHY enumeration and VAP lifecycle.
 *
 * On FreeBSD an 802.11 driver does NOT present a usable network interface. The
 * driver registers a `struct ieee80211com` (the PHY — "iwlwifi0"), and you must
 * clone a Virtual AP off it to get something you can configure:
 *
 *     ifconfig wlan0 create wlandev iwlwifi0
 *
 * On stock FreeBSD that line lives in rc.conf (wlans_iwlwifi0="wlan0"). NextBSD
 * has no rc.conf and no /etc/rc.d, and never will — launchd is PID 1, and
 * nextbsd-freebsd-compat's build hard-strips /stage/etc. So the phy->VAP
 * lifecycle has NO OWNER, and that is one of the three things standing between
 * "Intel card" and "IP address".
 *
 * It is wland's, because wland is the only thing that knows when a radio should
 * have an interface at all. Note this is a responsibility Apple's wifid never
 * had: IO80211 presents en0 directly, with no VAP concept to manage.
 */
#ifndef _WLAND_VAP_H_
#define _WLAND_VAP_H_

#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>

/* A radio and the VAP we cloned off it. */
struct wlan_phy {
	char	phy[IF_NAMESIZE];	/* e.g. "iwlwifi0" */
	char	vap[IF_NAMESIZE];	/* e.g. "wlan0", empty if not created */
};

#define	WLAN_MAX_PHY	4

/*
 * Sentinel returned by vap_enumerate_phys when net80211 is not loaded — the
 * net.wlan.devices sysctl node is absent (ENOENT). net80211 is Wlan.kext, which
 * a Wi-Fi driver kext pulls in when it matches hardware, so this is NOT
 * permanent: it means "no radio yet", like 0 ("the node exists but lists no
 * radios"), and a caller watching for radios keeps watching. It stays distinct
 * from 0 only so a caller can tell the two apart in a log.
 */
#define	VAP_NO_NET80211	(-2)

/*
 * Enumerate the 802.11 PHYs the kernel knows about, via the net.wlan.devices
 * sysctl — a space-separated list of ieee80211com names. This is the only
 * reliable way to find them: a PHY is not an ifnet, so it does not appear in
 * getifaddrs at all.
 *
 * Fills up to `max` entries with the phy name and an EMPTY vap name. Returns the
 * count (>= 0), VAP_NO_NET80211 if net80211 is not loaded (yet), or -1 on a
 * transient failure.
 */
int	vap_enumerate_phys(struct wlan_phy *out, size_t max);

/*
 * Clone a station-mode VAP off `phy` (SIOCIFCREATE2 with
 * ieee80211_clone_params — exactly what `ifconfig wlanN create wlandev <phy>`
 * does under the hood; we issue the ioctl directly rather than fork/exec'ing
 * ifconfig from a daemon).
 *
 * The interface name is a WILDCARD: we ask for "wlan" and the kernel picks the
 * next free unit, writing the real name ("wlan0") back. Hard-coding wlan0 would
 * break the moment there are two radios.
 *
 * Writes the assigned name into vap_out (at least IF_NAMESIZE). Returns 0 on
 * success. If a VAP already exists on this phy, that is not an error — see
 * vap_find_existing().
 */
int	vap_create(const char *phy, char *vap_out, size_t vap_out_sz);

/* Destroy a VAP (SIOCIFDESTROY). Idempotent: already-gone is success. */
int	vap_destroy(const char *vap);

/* Administratively up an interface (IFF_UP), so its link can negotiate. */
int	vap_bring_up(const char *ifname);

/*
 * Find an existing VAP whose parent is `phy`, if any — so a wland restart
 * adopts the VAP it created last time instead of cloning a second one (which
 * would give us wlan0 AND wlan1 on one radio).
 *
 * Returns true and fills vap_out on a hit.
 */
bool	vap_find_existing(const char *phy, char *vap_out, size_t vap_out_sz);

#endif /* _WLAND_VAP_H_ */
