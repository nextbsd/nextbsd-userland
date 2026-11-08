/*
 * wland_mig.c — server-side bodies for wlan.defs.
 *
 * The MIG-generated demux _wlan_server() (in wlanServer.c) calls these per
 * routine. They run on the Mach service thread and read the interface table the
 * main loop writes, so every one of them takes wland_lock().
 *
 * MIG passes fixed-size char arrays that may NOT be NUL-terminated if the client
 * filled every byte. Each routine defensively re-terminates before touching a
 * name — the same care ipconfigd_mig.c takes with InterfaceName.
 */
#include "wland.h"
#include "airport.h"	/* known_net_save */
#include "supplicant.h"
#include "wlan_mig_types.h"

#include <mach/mach.h>
#include "wlanServer.h"			/* MIG-emitted prototypes */

#include <string.h>
#include <strings.h>

/* Copy a MIG fixed-size name into a guaranteed-NUL-terminated buffer. */
static void
name_in(char *dst, size_t dst_sz, const char *src)
{
	(void)memset(dst, 0, dst_sz);
	(void)memcpy(dst, src, dst_sz - 1);
}

kern_return_t
_wlan_if_count(mach_port_t server, int *count)
{
	(void)server;

	wland_lock();
	*count = wland_if_count_locked();
	wland_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_if_name(mach_port_t server, int index, WLANInterfaceName name,
    wlan_status_t *status)
{
	struct wlan_if *w;

	(void)server;
	(void)memset(name, 0, sizeof(WLANInterfaceName));

	wland_lock();
	w = wland_if_at_locked(index);
	if (w == NULL) {
		*status = wlan_status_index_out_of_range_e;
	} else {
		(void)strlcpy(name, w->vap, sizeof(WLANInterfaceName));
		*status = wlan_status_success_e;
	}
	wland_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_scan(mach_port_t server, WLANInterfaceName name, wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	struct wlan_if *w;

	(void)server;
	name_in(ifname, sizeof(ifname), name);

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL)
		*status = wlan_status_interface_does_not_exist_e;
	else if (w->supp == NULL)
		*status = wlan_status_no_supplicant_e;
	else if (supplicant_scan(w->supp) != 0)
		*status = wlan_status_internal_error_e;
	else
		*status = wlan_status_success_e;
	wland_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_scan_count(mach_port_t server, WLANInterfaceName name, int *count,
    wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	struct wlan_if *w;

	(void)server;
	name_in(ifname, sizeof(ifname), name);
	*count = 0;

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL) {
		*status = wlan_status_interface_does_not_exist_e;
	} else if (w->supp == NULL) {
		*status = wlan_status_no_supplicant_e;
	} else {
		*count = supplicant_scan_count(w->supp);
		*status = wlan_status_success_e;
	}
	wland_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_scan_generation(mach_port_t server, WLANInterfaceName name,
    uint32_t *generation, boolean_t *last_failed, wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	struct wlan_if *w;
	bool failed = false;

	(void)server;
	name_in(ifname, sizeof(ifname), name);
	*generation = 0;

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL) {
		*status = wlan_status_interface_does_not_exist_e;
	} else if (w->supp == NULL) {
		*status = wlan_status_no_supplicant_e;
	} else {
		supplicant_scan_generation(w->supp, generation, &failed);
		*status = wlan_status_success_e;
	}
	wland_unlock();
	*last_failed = failed ? TRUE : FALSE;
	return (KERN_SUCCESS);
}

/*
 * One scan result, by index.
 *
 * This flat, one-entry-per-RPC shape is FORCED, not chosen: MIG out-of-line data
 * is broken in this port's kernel, so wland cannot hand back a scan-results
 * plist, and CONFIG_DATA_MAX caps any dynamic-store value at 8 KiB, so the list
 * cannot go through configd either. So the CLI calls wlan_scan_count() and then
 * loops wlan_scan_entry(i) — exactly the pattern ipconfigd established with
 * ipconfig_if_count / ipconfig_if_addr, for exactly the same reason.
 */
kern_return_t
_wlan_scan_entry(mach_port_t server, WLANInterfaceName name, int index,
    WLANSSID ssid, WLANBSSID bssid, int *rssi, int *channel,
    wlan_security_t *security, wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	struct wlan_if *w;
	struct scan_entry e;

	(void)server;
	name_in(ifname, sizeof(ifname), name);

	(void)memset(ssid, 0, sizeof(WLANSSID));
	(void)memset(bssid, 0, sizeof(WLANBSSID));
	*rssi = 0;
	*channel = 0;
	*security = wlan_security_unknown_e;

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL) {
		*status = wlan_status_interface_does_not_exist_e;
	} else if (w->supp == NULL) {
		*status = wlan_status_no_supplicant_e;
	} else if (!supplicant_scan_get(w->supp, index, &e)) {
		*status = wlan_status_index_out_of_range_e;
	} else {
		(void)strlcpy(ssid, e.ssid, sizeof(WLANSSID));
		(void)strlcpy(bssid, e.bssid, sizeof(WLANBSSID));
		*rssi = e.rssi;
		*channel = e.channel;
		*security = e.security;
		*status = wlan_status_success_e;
	}
	wland_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_connect(mach_port_t server, WLANInterfaceName name, WLANSSID ssid,
    WLANPassphrase psk, wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	char c_ssid[WLAN_SSID_MAX + 1];
	char c_psk[WLAN_PSK_MAX + 1];
	struct wlan_if *w;

	(void)server;
	name_in(ifname, sizeof(ifname), name);
	name_in(c_ssid, sizeof(c_ssid), ssid);
	name_in(c_psk, sizeof(c_psk), psk);

	if (c_ssid[0] == '\0') {
		*status = wlan_status_invalid_parameter_e;
		return (KERN_SUCCESS);
	}

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL) {
		*status = wlan_status_interface_does_not_exist_e;
	} else if (w->supp == NULL) {
		*status = wlan_status_no_supplicant_e;
	} else if (supplicant_needs_eap(w->supp, c_ssid, c_psk[0] != '\0')) {
		/* Without this, no passphrase means an open association. */
		*status = wlan_status_enterprise_required_e;
	} else if (supplicant_connect(w->supp, c_ssid, c_psk) != 0) {
		*status = wlan_status_internal_error_e;
	} else {
		/*
		 * Remember it. A network you successfully asked to join should
		 * be there after a reboot — that is the whole point of having a
		 * known-networks store, and it is what makes autojoin work at
		 * the next boot without the user typing the passphrase again.
		 */
		(void)known_net_save(c_ssid, c_psk, true);
		*status = wlan_status_success_e;
	}
	wland_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_connect_eap(mach_port_t server, WLANInterfaceName name, WLANSSID ssid,
    int eap_type, wlan_ttls_inner_t ttls_inner, WLANEAPName identity,
    WLANEAPName anonymous_identity, WLANEAPSecret password, WLANEAPPath ca_cert,
    WLANEAPName server_name, WLANEAPPath client_cert, WLANEAPPath private_key,
    WLANEAPSecret key_passphrase, boolean_t no_verify_server,
    wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	char c_ssid[WLAN_SSID_MAX + 1];
	struct eap_config cfg;
	struct wlan_if *w;

	(void)server;
	name_in(ifname, sizeof(ifname), name);
	name_in(c_ssid, sizeof(c_ssid), ssid);

	(void)memset(&cfg, 0, sizeof(cfg));
	cfg.type = eap_type;
	cfg.ttls_inner = ttls_inner;
	cfg.no_verify_server = (no_verify_server != FALSE);
	name_in(cfg.identity, sizeof(cfg.identity), identity);
	name_in(cfg.anonymous_identity, sizeof(cfg.anonymous_identity),
	    anonymous_identity);
	name_in(cfg.password, sizeof(cfg.password), password);
	name_in(cfg.ca_cert, sizeof(cfg.ca_cert), ca_cert);
	name_in(cfg.server_name, sizeof(cfg.server_name), server_name);
	name_in(cfg.client_cert, sizeof(cfg.client_cert), client_cert);
	name_in(cfg.private_key, sizeof(cfg.private_key), private_key);
	name_in(cfg.key_passphrase, sizeof(cfg.key_passphrase), key_passphrase);

	if (c_ssid[0] == '\0') {
		*status = wlan_status_invalid_parameter_e;
		explicit_bzero(&cfg, sizeof(cfg));
		return (KERN_SUCCESS);
	}

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL) {
		*status = wlan_status_interface_does_not_exist_e;
	} else if (w->supp == NULL) {
		*status = wlan_status_no_supplicant_e;
	} else if ((*status = supplicant_eap_check(&cfg)) !=
	    wlan_status_success_e) {
		/* *status says why; supplicant_eap_check logged it. */
	} else if (supplicant_connect_eap(w->supp, c_ssid, &cfg) != 0) {
		*status = wlan_status_internal_error_e;
	} else {
		/* Asked for by name: forget any hold from earlier failures. */
		supplicant_eap_autojoin_release(w->supp);
		(void)known_net_save_eap(c_ssid, &cfg, true);
		*status = wlan_status_success_e;
	}
	wland_unlock();
	explicit_bzero(&cfg, sizeof(cfg));
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_disconnect(mach_port_t server, WLANInterfaceName name,
    wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	struct wlan_if *w;

	(void)server;
	name_in(ifname, sizeof(ifname), name);

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL)
		*status = wlan_status_interface_does_not_exist_e;
	else if (w->supp == NULL)
		*status = wlan_status_no_supplicant_e;
	else if (supplicant_disconnect(w->supp) != 0)
		*status = wlan_status_internal_error_e;
	else
		*status = wlan_status_success_e;
	wland_unlock();
	return (KERN_SUCCESS);
}

kern_return_t
_wlan_status(mach_port_t server, WLANInterfaceName name, wlan_state_t *state,
    WLANSSID ssid, WLANBSSID bssid, int *rssi, int *channel,
    wlan_security_t *security, wlan_status_t *status)
{
	char ifname[IF_NAMESIZE];
	struct wlan_if *w;
	struct assoc_state st;

	(void)server;
	name_in(ifname, sizeof(ifname), name);

	(void)memset(ssid, 0, sizeof(WLANSSID));
	(void)memset(bssid, 0, sizeof(WLANBSSID));
	*state = wlan_state_inactive_e;
	*rssi = 0;
	*channel = 0;
	*security = wlan_security_unknown_e;

	wland_lock();
	w = wland_if_by_name_locked(ifname);
	if (w == NULL) {
		*status = wlan_status_interface_does_not_exist_e;
	} else if (w->supp == NULL) {
		*status = wlan_status_no_supplicant_e;
	} else {
		supplicant_get_state(w->supp, &st);
		*state = st.state;
		(void)strlcpy(ssid, st.ssid, sizeof(WLANSSID));
		(void)strlcpy(bssid, st.bssid, sizeof(WLANBSSID));
		*rssi = st.rssi;
		*channel = st.channel;
		*security = st.security;
		*status = wlan_status_success_e;
	}
	wland_unlock();
	return (KERN_SUCCESS);
}
