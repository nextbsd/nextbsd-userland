/*
 * wlan_mig_types.h — C typedefs for the named MIG types in wlan.defs.
 *
 * MIG records the wire layout of a `type` declaration but does not emit a C
 * typedef for it: the generated stubs use the type name as-is and expect an
 * imported header to define it. wlan.defs imports this header so both the
 * generated server (wlanServer.{c,h}) and user (wlanUser.c / wlan.h) sides
 * agree. Same arrangement as ipconfig_mig_types.h.
 *
 * Everything here is a FIXED-SIZE array or a scalar, deliberately. Scan results
 * use the flat indexed pattern ipconfigd established (wlan_scan_count() then
 * wlan_scan_entry(i)), and wlan_connect_eap's credentials stay inline rather
 * than travelling in out-of-line pages. See the note at the top of wlan.defs.
 */
#ifndef _WLAN_MIG_TYPES_H
#define _WLAN_MIG_TYPES_H

#include <stdint.h>
#include <net/if.h>		/* IF_NAMESIZE */

/* Interface name — "wlan0". Same shape as ipconfigd's InterfaceName. */
typedef char	WLANInterfaceName[IF_NAMESIZE];

/*
 * An 802.11 SSID is up to 32 OCTETS (not characters, and not necessarily
 * NUL-terminated or even valid UTF-8 on the wire). We carry 33 so there is
 * always room to NUL-terminate for C string handling, and treat the content as
 * opaque bytes we only ever print.
 */
#define	WLAN_SSID_MAX	32
typedef char	WLANSSID[WLAN_SSID_MAX + 1];

/* "aa:bb:cc:dd:ee:ff" + NUL. */
typedef char	WLANBSSID[18];

/*
 * WPA passphrase: 8..63 printable characters (a 64-char hex PSK is also legal).
 * 65 gives room for the longest legal value plus a NUL.
 */
#define	WLAN_PSK_MAX	64
typedef char	WLANPassphrase[WLAN_PSK_MAX + 1];

/*
 * 802.1X credentials for wlan_connect_eap. Names and secrets get 256 bytes, and
 * certificate and key paths get 1024 (PATH_MAX). All of them together stay well
 * under WLAN_RPC_BUFSZ.
 */
#define	WLAN_EAP_STR_MAX	255
#define	WLAN_EAP_PATH_MAX	1023
typedef char	WLANEAPName[WLAN_EAP_STR_MAX + 1];
typedef char	WLANEAPSecret[WLAN_EAP_STR_MAX + 1];
typedef char	WLANEAPPath[WLAN_EAP_PATH_MAX + 1];

/* EAP method numbers: Darwin's EAPType (eap8021x EAP.h) and the IANA values. */
#define	WLAN_EAP_TYPE_TLS	13
#define	WLAN_EAP_TYPE_TTLS	21
#define	WLAN_EAP_TYPE_PEAP	25

/* TTLS inner method. Zero is Darwin's default, MSCHAPv2. */
typedef enum {
	wlan_ttls_inner_mschapv2_e	= 0,
	wlan_ttls_inner_pap_e		= 1,
	wlan_ttls_inner_chap_e		= 2
} wlan_ttls_inner_t;

/* Association state, mirroring what wpa_supplicant reports. */
typedef enum {
	wlan_state_inactive_e		= 0,	/* no VAP / radio off */
	wlan_state_scanning_e		= 1,
	wlan_state_associating_e	= 2,
	wlan_state_associated_e		= 3,	/* RUN, but NOT yet keyed */
	wlan_state_4way_e		= 4,	/* handshake in progress */
	wlan_state_connected_e		= 5,	/* keyed — safe to DHCP */
	wlan_state_disconnected_e	= 6
} wlan_state_t;

/*
 * Security of a scanned network, derived from wpa_supplicant's scan flags.
 * Ordered weakest -> strongest so a UI can sort on it.
 */
typedef enum {
	wlan_security_open_e		= 0,
	wlan_security_wep_e		= 1,
	wlan_security_wpa_e		= 2,	/* WPA1 (TKIP era) */
	wlan_security_wpa2_e		= 3,	/* RSN / WPA2-PSK */
	wlan_security_wpa3_e		= 4,	/* SAE */
	wlan_security_enterprise_e	= 5,	/* 802.1X / EAP */
	wlan_security_unknown_e		= 6
} wlan_security_t;

/*
 * Status codes. Values are ours (there is no Apple original to match here —
 * Apple's equivalent surface is inside the closed wifid), but the shape follows
 * ipconfig_status_t so the CLI's error handling reads the same.
 */
typedef enum {
	wlan_status_success_e			= 0,
	wlan_status_invalid_parameter_e		= 1,
	wlan_status_interface_does_not_exist_e	= 2,
	wlan_status_no_supplicant_e		= 3,	/* wpa_supplicant unreachable */
	wlan_status_not_associated_e		= 4,
	wlan_status_index_out_of_range_e		= 5,
	wlan_status_internal_error_e		= 6,
	wlan_status_operation_not_supported_e	= 7,
	wlan_status_enterprise_required_e	= 8,	/* 802.1X needs --eap */
	wlan_status_no_trust_anchor_e		= 9,	/* no CA and no name */
	wlan_status_ca_bundle_missing_e		= 10,	/* name given, no bundle */
	wlan_status_credential_file_e		= 11,	/* unusable cert/key/pin */
	wlan_status_key_file_insecure_e		= 12	/* key readable by others */
} wlan_status_t;

#endif /* _WLAN_MIG_TYPES_H */
