/*
 * supplicant.c — wland's policy layer over stock wpa_supplicant.
 * See supplicant.h for why wland is the sole author of the network blocks.
 */
#include "supplicant.h"
#include "wpa_ctrl.h"

#include <net/if.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define	SCAN_MAX	64

/*
 * The trust anchors --server-name is checked against when no --ca-cert is
 * given: certctl(8)'s bundle, NextBSD's counterpart to Darwin's system roots.
 */
#define	SYSTEM_CA_BUNDLE	"/etc/ssl/cert.pem"

/* "hash://server/sha256/" + 64 hex digits, as tls_openssl.c parses it. */
#define	CA_PIN_PREFIX		"hash://server/sha256/"
#define	CA_PIN_HEX_LEN		64

/* How long autojoin leaves enterprise networks alone after an EAP failure. */
#define	EAP_HOLD_MIN_SEC	300
#define	EAP_HOLD_MAX_SEC	3600

struct supplicant {
	char			ifname[IF_NAMESIZE];
	struct wpa_ctrl		*cmd;
	struct wpa_ctrl		*evt;
	int			net_id;		/* our one network block, or -1 */

	struct scan_entry	scan[SCAN_MAX];
	int			scan_n;
	uint32_t		scan_generation; /* sweeps finished */
	bool			scan_last_failed; /* latest sweep failed */

	bool			autojoin_armed;	/* autojoin awaits a sweep */
	bool			autojoin_ready;	/* ...and it has finished */

	bool			eap_attempt_failed; /* counted this attempt */
	unsigned int		eap_failures;	/* in a row */
	time_t			eap_hold_until;	/* no enterprise autojoin */

	struct assoc_state	state;
};

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "wland[supp] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * 802.11 channel from centre frequency in MHz.
 *   2.4 GHz: channels 1-13 are 2412 + 5*(ch-1); channel 14 is the 2484 oddity.
 *   5 GHz and 6 GHz: ch = (freq - 5000) / 5.
 * Returns 0 if the frequency is not one we recognise.
 */
static int
freq_to_channel(int freq)
{
	if (freq == 2484)
		return (14);
	if (freq >= 2412 && freq <= 2472)
		return ((freq - 2407) / 5);
	if (freq >= 5000 && freq < 6000)
		return ((freq - 5000) / 5);
	if (freq >= 5955 && freq <= 7115)	/* 6 GHz (WiFi 6E) */
		return ((freq - 5950) / 5);
	return (0);
}

/*
 * Classify a network from wpa_supplicant's scan flags column, e.g.
 *   [WPA2-PSK-CCMP][ESS]
 *   [WPA2-PSK+SAE-CCMP][ESS]        (WPA2/WPA3 transitional)
 *   [WPA2-EAP-CCMP][ESS]
 *   [ESS]                           (open)
 *
 * Checked strongest-first: a transitional AP advertises both PSK and SAE, and
 * calling that WPA2 would understate what we will actually negotiate.
 */
static wlan_security_t
flags_to_security(const char *flags)
{
	if (flags == NULL)
		return (wlan_security_unknown_e);
	if (strstr(flags, "EAP") != NULL)
		return (wlan_security_enterprise_e);
	if (strstr(flags, "SAE") != NULL)
		return (wlan_security_wpa3_e);
	if (strstr(flags, "WPA2") != NULL || strstr(flags, "RSN") != NULL)
		return (wlan_security_wpa2_e);
	if (strstr(flags, "WPA") != NULL)
		return (wlan_security_wpa_e);
	if (strstr(flags, "WEP") != NULL)
		return (wlan_security_wep_e);
	return (wlan_security_open_e);
}

/* wpa_supplicant's wpa_state= string -> our enum. */
static wlan_state_t
wpa_state_to_enum(const char *s)
{
	if (strcmp(s, "COMPLETED") == 0)
		return (wlan_state_connected_e);
	if (strcmp(s, "4WAY_HANDSHAKE") == 0 ||
	    strcmp(s, "GROUP_HANDSHAKE") == 0)
		return (wlan_state_4way_e);
	if (strcmp(s, "ASSOCIATED") == 0)
		return (wlan_state_associated_e);
	if (strcmp(s, "ASSOCIATING") == 0 ||
	    strcmp(s, "AUTHENTICATING") == 0)
		return (wlan_state_associating_e);
	if (strcmp(s, "SCANNING") == 0)
		return (wlan_state_scanning_e);
	if (strcmp(s, "DISCONNECTED") == 0)
		return (wlan_state_disconnected_e);
	return (wlan_state_inactive_e);
}

struct supplicant *
supplicant_attach(const char *ifname)
{
	struct supplicant *s;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return (NULL);
	(void)strlcpy(s->ifname, ifname, sizeof(s->ifname));
	s->net_id = -1;
	s->state.state = wlan_state_inactive_e;

	s->cmd = wpa_ctrl_open(ifname);
	if (s->cmd == NULL) {
		free(s);
		return (NULL);		/* not up yet — caller retries */
	}
	s->evt = wpa_ctrl_open_event(ifname);
	if (s->evt == NULL) {
		wpa_ctrl_close(s->cmd);
		free(s);
		return (NULL);
	}

	/*
	 * Start from a clean slate. wpa_supplicant is launched with an empty
	 * config, but a wland restart can leave network blocks behind in a
	 * still-running supplicant — and a stale block is a second autojoin
	 * brain (see supplicant.h). Remove them all; we re-add on demand.
	 */
	(void)wpa_ctrl_ok(s->cmd, "REMOVE_NETWORK all");

	xlog("%s: attached to wpa_supplicant", ifname);
	(void)supplicant_refresh_state(s);
	return (s);
}

void
supplicant_detach(struct supplicant *s)
{
	if (s == NULL)
		return;
	wpa_ctrl_close(s->evt);
	wpa_ctrl_close(s->cmd);
	free(s);
}

int
supplicant_event_fd(const struct supplicant *s)
{
	return (s != NULL ? wpa_ctrl_fd(s->evt) : -1);
}

int
supplicant_scan(struct supplicant *s)
{
	if (s == NULL)
		return (-1);
	/*
	 * "FAIL-BUSY" just means a scan is already running — which is what the
	 * caller wanted anyway, so it is not an error.
	 */
	if (!wpa_ctrl_ok(s->cmd, "SCAN")) {
		char reply[32];

		if (wpa_ctrl_request(s->cmd, "SCAN", reply, sizeof(reply),
		    2000) == 0 && strstr(reply, "BUSY") != NULL)
			return (0);
		return (-1);
	}
	return (0);
}

int
supplicant_refresh_scan(struct supplicant *s)
{
	char *buf, *line, *save;
	size_t bufsz = 8192;
	int n = 0;

	if (s == NULL)
		return (-1);

	buf = malloc(bufsz);
	if (buf == NULL)
		return (-1);
	if (wpa_ctrl_request(s->cmd, "SCAN_RESULTS", buf, bufsz, 3000) != 0) {
		free(buf);
		return (-1);
	}

	/*
	 * Tab-separated, one AP per line, with a header line first:
	 *   bssid / frequency / signal level / flags / ssid
	 *   aa:bb:cc:dd:ee:ff	2412	-45	[WPA2-PSK-CCMP][ESS]	MyNet
	 *
	 * The SSID is last precisely because it may contain anything, including
	 * spaces — so it is taken as the whole remainder of the line, never
	 * tokenized.
	 */
	for (line = strtok_r(buf, "\n", &save); line != NULL;
	     line = strtok_r(NULL, "\n", &save)) {
		char *f[5], *p = line;
		int i, freq;

		if (strncmp(line, "bssid", 5) == 0)
			continue;			/* header */
		if (n >= SCAN_MAX)
			break;

		for (i = 0; i < 4; i++) {
			f[i] = p;
			p = strchr(p, '\t');
			if (p == NULL)
				break;
			*p++ = '\0';
		}
		if (i < 4)
			continue;			/* malformed */
		f[4] = p;				/* SSID: rest of line */

		(void)memset(&s->scan[n], 0, sizeof(s->scan[n]));
		(void)strlcpy(s->scan[n].bssid, f[0],
		    sizeof(s->scan[n].bssid));
		freq = (int)strtol(f[1], NULL, 10);
		s->scan[n].channel = freq_to_channel(freq);
		s->scan[n].rssi = (int)strtol(f[2], NULL, 10);
		s->scan[n].security = flags_to_security(f[3]);
		s->scan[n].psk_ok = (strstr(f[3], "PSK") != NULL);
		(void)strlcpy(s->scan[n].ssid, f[4] != NULL ? f[4] : "",
		    sizeof(s->scan[n].ssid));

		/* A hidden AP broadcasts an empty SSID — nothing to show. */
		if (s->scan[n].ssid[0] == '\0')
			continue;
		n++;
	}
	free(buf);

	s->scan_n = n;
	xlog("%s: %d networks in scan cache", s->ifname, n);
	return (n);
}

int
supplicant_scan_count(const struct supplicant *s)
{
	return (s != NULL ? s->scan_n : 0);
}

bool
supplicant_scan_get(const struct supplicant *s, int index,
    struct scan_entry *out)
{
	if (s == NULL || out == NULL || index < 0 || index >= s->scan_n)
		return (false);
	*out = s->scan[index];
	return (true);
}

void
supplicant_scan_generation(const struct supplicant *s, uint32_t *generation,
    bool *last_failed)
{
	*generation = (s != NULL) ? s->scan_generation : 0;
	*last_failed = (s != NULL) ? s->scan_last_failed : false;
}

void
supplicant_autojoin_arm(struct supplicant *s)
{
	if (s != NULL)
		s->autojoin_armed = true;
}

bool
supplicant_autojoin_take(struct supplicant *s)
{
	bool ready;

	if (s == NULL)
		return (false);
	ready = s->autojoin_ready;
	s->autojoin_ready = false;
	return (ready);
}

/*
 * Replace whatever network block exists with a new, empty one. Returns its id,
 * or -1.
 */
static int
network_fresh(struct supplicant *s)
{
	char reply[64];
	int id;

	/* This choice stands: a sweep still in flight must not override it. */
	s->autojoin_armed = false;

	/*
	 * One network block, ever. Removing the old one before adding the new
	 * one is what keeps wland the sole autojoin brain — leave two blocks
	 * behind and wpa_supplicant will happily pick between them itself.
	 */
	(void)wpa_ctrl_ok(s->cmd, "REMOVE_NETWORK all");
	s->net_id = -1;

	if (wpa_ctrl_request(s->cmd, "ADD_NETWORK", reply, sizeof(reply),
	    2000) != 0) {
		xlog("%s: ADD_NETWORK failed", s->ifname);
		return (-1);
	}
	id = (int)strtol(reply, NULL, 10);
	if (id < 0) {
		xlog("%s: ADD_NETWORK returned '%s'", s->ifname, reply);
		return (-1);
	}
	return (id);
}

int
supplicant_connect(struct supplicant *s, const char *ssid, const char *psk)
{
	char cmd[256];
	int id;

	if (s == NULL || ssid == NULL || ssid[0] == '\0')
		return (-1);

	id = network_fresh(s);
	if (id < 0)
		return (-1);

	(void)snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", id, ssid);
	if (!wpa_ctrl_ok(s->cmd, cmd)) {
		xlog("%s: SET_NETWORK ssid failed", s->ifname);
		return (-1);
	}

	if (psk != NULL && psk[0] != '\0') {
		(void)snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"",
		    id, psk);
		if (!wpa_ctrl_ok(s->cmd, cmd)) {
			/*
			 * Almost always a passphrase outside 8..63 characters.
			 * wpa_supplicant rejects it here rather than failing
			 * later at association, which is the good outcome.
			 */
			xlog("%s: SET_NETWORK psk rejected (passphrase must be "
			    "8-63 characters)", s->ifname);
			return (-1);
		}
	} else {
		(void)snprintf(cmd, sizeof(cmd),
		    "SET_NETWORK %d key_mgmt NONE", id);
		if (!wpa_ctrl_ok(s->cmd, cmd)) {
			xlog("%s: SET_NETWORK key_mgmt NONE failed", s->ifname);
			return (-1);
		}
	}

	(void)snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
	if (!wpa_ctrl_ok(s->cmd, cmd)) {
		xlog("%s: SELECT_NETWORK failed", s->ifname);
		return (-1);
	}

	s->net_id = id;
	xlog("%s: associating with '%s' (%s)", s->ifname, ssid,
	    (psk != NULL && psk[0] != '\0') ? "PSK" : "open");
	return (0);
}

bool
supplicant_needs_eap(const struct supplicant *s, const char *ssid,
    bool have_psk)
{
	int i;

	if (s == NULL || ssid == NULL)
		return (false);
	for (i = 0; i < s->scan_n; i++) {
		if (s->scan[i].security != wlan_security_enterprise_e ||
		    strcmp(s->scan[i].ssid, ssid) != 0)
			continue;
		if (!have_psk || !s->scan[i].psk_ok)
			return (true);
	}
	return (false);
}

/* A certificate or key path wland can hand to wpa_supplicant: absolute, a file. */
static bool
credential_file_ok(const char *path, struct stat *st)
{
	if (path[0] != '/') {
		errno = EINVAL;
		return (false);
	}
	if (stat(path, st) != 0)
		return (false);
	if (!S_ISREG(st->st_mode)) {
		errno = EFTYPE;
		return (false);
	}
	return (true);
}

/* Is this a well-formed hash://server/sha256/<hex> server certificate pin? */
static bool
ca_is_pin(const char *ca)
{
	size_t plen = strlen(CA_PIN_PREFIX);
	size_t i;

	if (strncmp(ca, CA_PIN_PREFIX, plen) != 0 ||
	    strlen(ca) != plen + CA_PIN_HEX_LEN)
		return (false);
	for (i = plen; ca[i] != '\0'; i++) {
		if (!isxdigit((unsigned char)ca[i]))
			return (false);
	}
	return (true);
}

wlan_status_t
supplicant_eap_check(const struct eap_config *c)
{
	struct stat st;
	bool have_ca, have_name, password_auth;

	if (c->type != WLAN_EAP_TYPE_TLS && c->type != WLAN_EAP_TYPE_TTLS &&
	    c->type != WLAN_EAP_TYPE_PEAP) {
		xlog("802.1X: unknown EAP type %d", c->type);
		return (wlan_status_invalid_parameter_e);
	}
	if (c->ttls_inner != wlan_ttls_inner_mschapv2_e &&
	    c->ttls_inner != wlan_ttls_inner_pap_e &&
	    c->ttls_inner != wlan_ttls_inner_chap_e) {
		xlog("802.1X: unknown TTLS inner method %d", (int)c->ttls_inner);
		return (wlan_status_invalid_parameter_e);
	}

	password_auth = (c->type != WLAN_EAP_TYPE_TLS);
	if (c->identity[0] == '\0') {
		xlog("802.1X: no identity");
		return (wlan_status_invalid_parameter_e);
	}
	if (password_auth && c->password[0] == '\0') {
		xlog("802.1X: no password");
		return (wlan_status_invalid_parameter_e);
	}
	if (!password_auth && c->private_key[0] == '\0') {
		xlog("802.1X: EAP-TLS with no private key");
		return (wlan_status_invalid_parameter_e);
	}

	/*
	 * Server trust. Darwin verifies by default and, with no anchors and no
	 * names configured, asks the user (EAPTLSUtil.c:1263-1280,1665-1676).
	 * wland has nobody to ask, so that case fails closed; --no-verify-server
	 * is the explicit way out. TLSVerifyServerCertificate=false overrides
	 * everything on Darwin; here giving it with an anchor is an error, since
	 * the combination has no sensible meaning.
	 */
	have_ca = (c->ca_cert[0] != '\0');
	have_name = (c->server_name[0] != '\0');
	if (c->no_verify_server) {
		if (have_ca || have_name) {
			xlog("802.1X: no_verify_server with a CA or name");
			return (wlan_status_invalid_parameter_e);
		}
	} else if (!have_ca && !have_name) {
		xlog("802.1X: no CA certificate or server name, and "
		    "verification was not switched off");
		return (wlan_status_no_trust_anchor_e);
	}

	if (have_ca && strncmp(c->ca_cert, "hash://", 7) == 0) {
		if (!ca_is_pin(c->ca_cert)) {
			xlog("802.1X: malformed hash:// CA pin");
			return (wlan_status_credential_file_e);
		}
	} else if (have_ca && !credential_file_ok(c->ca_cert, &st)) {
		xlog("802.1X: CA certificate %s: %s", c->ca_cert,
		    strerror(errno));
		return (wlan_status_credential_file_e);
	} else if (!have_ca && have_name &&
	    !credential_file_ok(SYSTEM_CA_BUNDLE, &st)) {
		xlog("802.1X: --server-name needs the system CA bundle "
		    SYSTEM_CA_BUNDLE ": %s", strerror(errno));
		return (wlan_status_ca_bundle_missing_e);
	}

	if (c->client_cert[0] != '\0' &&
	    !credential_file_ok(c->client_cert, &st)) {
		xlog("802.1X: client certificate %s: %s", c->client_cert,
		    strerror(errno));
		return (wlan_status_credential_file_e);
	}
	if (c->private_key[0] != '\0') {
		if (!credential_file_ok(c->private_key, &st)) {
			xlog("802.1X: private key %s: %s", c->private_key,
			    strerror(errno));
			return (wlan_status_credential_file_e);
		}
		if ((st.st_mode & (S_IRGRP | S_IROTH)) != 0) {
			xlog("802.1X: private key %s is group- or "
			    "world-readable; refusing it", c->private_key);
			return (wlan_status_key_file_insecure_e);
		}
	}
	return (wlan_status_success_e);
}

/*
 * One SET_NETWORK. The buffer is sized to the value because certificate paths
 * do not fit the 256 bytes supplicant_connect uses, and is wiped because the
 * value may be a credential. Rejections log the field name, never the value.
 */
static int
network_set(struct supplicant *s, int id, const char *field, const char *value,
    bool quoted)
{
	char *cmd;
	bool ok;
	int n;

	if (quoted)
		n = asprintf(&cmd, "SET_NETWORK %d %s \"%s\"", id, field, value);
	else
		n = asprintf(&cmd, "SET_NETWORK %d %s %s", id, field, value);
	if (n < 0) {
		xlog("%s: out of memory setting %s", s->ifname, field);
		return (-1);
	}
	ok = wpa_ctrl_ok(s->cmd, cmd);
	explicit_bzero(cmd, (size_t)n);
	free(cmd);
	if (!ok) {
		xlog("%s: SET_NETWORK %s rejected", s->ifname, field);
		return (-1);
	}
	return (0);
}

/* Set a quoted string field, unless it is empty. */
static int
network_set_opt(struct supplicant *s, int id, const char *field,
    const char *value)
{
	if (value[0] == '\0')
		return (0);
	return (network_set(s, id, field, value, true));
}

static const char *
eap_method_name(int type)
{
	switch (type) {
	case WLAN_EAP_TYPE_TLS:		return ("TLS");
	case WLAN_EAP_TYPE_TTLS:	return ("TTLS");
	default:			return ("PEAP");
	}
}

int
supplicant_connect_eap(struct supplicant *s, const char *ssid,
    const struct eap_config *c)
{
	char cmd[32];
	const char *phase2 = NULL;
	const char *ca;
	int id;

	if (s == NULL || ssid == NULL || ssid[0] == '\0' || c == NULL)
		return (-1);

	id = network_fresh(s);
	if (id < 0)
		return (-1);
	s->eap_attempt_failed = false;

	/* PEAP is MSCHAPv2 only; TTLS defaults to it, as Darwin does. */
	if (c->type == WLAN_EAP_TYPE_PEAP) {
		phase2 = "auth=MSCHAPV2";
	} else if (c->type == WLAN_EAP_TYPE_TTLS) {
		switch (c->ttls_inner) {
		case wlan_ttls_inner_pap_e:	phase2 = "auth=PAP";	break;
		case wlan_ttls_inner_chap_e:	phase2 = "auth=CHAP";	break;
		default:			phase2 = "auth=MSCHAPV2"; break;
		}
	}

	if (network_set_opt(s, id, "ssid", ssid) != 0 ||
	    network_set(s, id, "key_mgmt", "WPA-EAP", false) != 0 ||
	    network_set(s, id, "eap", eap_method_name(c->type), false) != 0 ||
	    network_set_opt(s, id, "identity", c->identity) != 0 ||
	    network_set_opt(s, id, "anonymous_identity",
	    c->anonymous_identity) != 0 ||
	    network_set_opt(s, id, "client_cert", c->client_cert) != 0 ||
	    network_set_opt(s, id, "private_key", c->private_key) != 0 ||
	    network_set_opt(s, id, "private_key_passwd",
	    c->key_passphrase) != 0)
		goto fail;
	if (phase2 != NULL &&
	    (network_set_opt(s, id, "password", c->password) != 0 ||
	    network_set_opt(s, id, "phase2", phase2) != 0))
		goto fail;

	if (!c->no_verify_server) {
		/*
		 * A name with no CA is checked against the system's roots.
		 * domain_suffix_match on its own is not enough: with no
		 * ca_cert, wpa_supplicant skips chain validation
		 * (tls_openssl.c, ca_cert_verify) and would match the name
		 * against any self-signed certificate that claims it. Darwin
		 * evaluates the chain against the system's anchors here, too.
		 */
		ca = (c->ca_cert[0] != '\0') ? c->ca_cert : SYSTEM_CA_BUNDLE;
		if (network_set_opt(s, id, "ca_cert", ca) != 0 ||
		    network_set_opt(s, id, "domain_suffix_match",
		    c->server_name) != 0)
			goto fail;
	}

	(void)snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
	if (!wpa_ctrl_ok(s->cmd, cmd)) {
		xlog("%s: SELECT_NETWORK failed", s->ifname);
		goto fail;
	}

	s->net_id = id;
	xlog("%s: associating with '%s' (802.1X EAP-%s)", s->ifname, ssid,
	    eap_method_name(c->type));
	return (0);

fail:
	/* Do not leave a half-built block holding credentials. */
	(void)wpa_ctrl_ok(s->cmd, "REMOVE_NETWORK all");
	return (-1);
}

bool
supplicant_eap_autojoin_held(const struct supplicant *s)
{
	return (s != NULL && time(NULL) < s->eap_hold_until);
}

void
supplicant_eap_autojoin_release(struct supplicant *s)
{
	if (s == NULL)
		return;
	s->eap_failures = 0;
	s->eap_hold_until = 0;
}

/*
 * Count one failed EAP attempt and hold enterprise autojoin off. Autojoin
 * replaces the network block every time, which resets wpa_supplicant's own
 * back-off, so without this a wrong saved password would reach the RADIUS
 * server every AUTOJOIN_INTERVAL_SEC and lock the account in the directory
 * behind it. A certificate error and the EAP failure that follows it are one
 * attempt; CTRL-EVENT-EAP-STARTED begins the next.
 */
static void
note_eap_failure(struct supplicant *s)
{
	time_t hold = EAP_HOLD_MIN_SEC;
	unsigned int i;

	if (s->eap_attempt_failed)
		return;
	s->eap_attempt_failed = true;
	s->eap_failures++;
	for (i = 1; i < s->eap_failures && hold < EAP_HOLD_MAX_SEC; i++)
		hold *= 2;
	if (hold > EAP_HOLD_MAX_SEC)
		hold = EAP_HOLD_MAX_SEC;
	s->eap_hold_until = time(NULL) + hold;
	xlog("%s: autojoin leaves enterprise networks alone for %d seconds",
	    s->ifname, (int)hold);
}

int
supplicant_disconnect(struct supplicant *s)
{
	if (s == NULL)
		return (-1);
	s->autojoin_armed = false;
	(void)wpa_ctrl_ok(s->cmd, "DISCONNECT");
	(void)wpa_ctrl_ok(s->cmd, "REMOVE_NETWORK all");
	s->net_id = -1;
	xlog("%s: disconnected", s->ifname);
	(void)supplicant_refresh_state(s);
	return (0);
}

bool
supplicant_refresh_state(struct supplicant *s)
{
	char buf[2048], *line, *save;
	struct assoc_state prev;
	int freq = 0;

	if (s == NULL)
		return (false);
	prev = s->state;

	(void)memset(&s->state, 0, sizeof(s->state));
	s->state.state = wlan_state_inactive_e;
	s->state.rssi = 0;

	if (wpa_ctrl_request(s->cmd, "STATUS", buf, sizeof(buf), 2000) != 0)
		goto done;

	/* STATUS is key=value, one per line. */
	for (line = strtok_r(buf, "\n", &save); line != NULL;
	     line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '=');

		if (eq == NULL)
			continue;
		*eq++ = '\0';

		if (strcmp(line, "wpa_state") == 0)
			s->state.state = wpa_state_to_enum(eq);
		else if (strcmp(line, "ssid") == 0)
			(void)strlcpy(s->state.ssid, eq,
			    sizeof(s->state.ssid));
		else if (strcmp(line, "bssid") == 0)
			(void)strlcpy(s->state.bssid, eq,
			    sizeof(s->state.bssid));
		else if (strcmp(line, "freq") == 0)
			freq = (int)strtol(eq, NULL, 10);
	}
	s->state.channel = freq_to_channel(freq);

	/*
	 * STATUS does not carry a signal level. Pull the RSSI (and the
	 * security, which STATUS also omits) from the scan cache entry for the
	 * BSSID we are actually on.
	 */
	if (s->state.bssid[0] != '\0') {
		int i;

		for (i = 0; i < s->scan_n; i++) {
			if (strcmp(s->scan[i].bssid, s->state.bssid) == 0) {
				s->state.rssi = s->scan[i].rssi;
				s->state.security = s->scan[i].security;
				break;
			}
		}
	}

done:
	return (memcmp(&prev, &s->state, sizeof(prev)) != 0);
}

void
supplicant_get_state(const struct supplicant *s, struct assoc_state *out)
{
	if (out == NULL)
		return;
	if (s == NULL) {
		(void)memset(out, 0, sizeof(*out));
		out->state = wlan_state_inactive_e;
		return;
	}
	*out = s->state;
}

bool
supplicant_pump_events(struct supplicant *s)
{
	char ev[512];
	bool changed = false;
	int rc;

	if (s == NULL)
		return (false);

	while ((rc = wpa_ctrl_recv_event(s->evt, ev, sizeof(ev))) > 0) {
		if (strncmp(ev, "CTRL-EVENT-CONNECTED", 20) == 0) {
			/*
			 * THE event. The 4-way handshake is done and the port is
			 * keyed — only NOW is it safe to DHCP. wland's caller
			 * republishes AirPort{Authenticated:true}, which is the
			 * predicate ipconfigd's gate is waiting on.
			 */
			xlog("%s: CONNECTED — 4-way handshake complete",
			    s->ifname);
			(void)supplicant_refresh_state(s);
			supplicant_eap_autojoin_release(s);
			changed = true;
		} else if (strncmp(ev, "CTRL-EVENT-EAP-STARTED", 22) == 0) {
			s->eap_attempt_failed = false;
		} else if (strncmp(ev, "CTRL-EVENT-EAP-TLS-CERT-ERROR", 29)
		    == 0) {
			/*
			 * The server's certificate did not match the CA or name
			 * this network was configured with. The event line says
			 * which check failed and carries no credentials.
			 */
			xlog("%s: 802.1X server certificate rejected: %s",
			    s->ifname, ev);
			note_eap_failure(s);
		} else if (strncmp(ev, "CTRL-EVENT-EAP-FAILURE", 22) == 0) {
			xlog("%s: 802.1X authentication failed (wrong username "
			    "or password, or the server refused them): %s",
			    s->ifname, ev);
			note_eap_failure(s);
			(void)supplicant_refresh_state(s);
			changed = true;
		} else if (strncmp(ev, "CTRL-EVENT-DISCONNECTED", 23) == 0) {
			xlog("%s: DISCONNECTED", s->ifname);
			(void)supplicant_refresh_state(s);
			changed = true;
		} else if (strncmp(ev, "CTRL-EVENT-SCAN-RESULTS", 23) == 0) {
			(void)supplicant_refresh_scan(s);
			/*
			 * After the refresh, so a client that sees the new
			 * generation also sees the new results.
			 */
			s->scan_generation++;
			s->scan_last_failed = false;
			if (s->autojoin_armed) {
				s->autojoin_armed = false;
				s->autojoin_ready = true;
			}
			/* Not an association change — no republish needed. */
		} else if (strncmp(ev, "CTRL-EVENT-SCAN-FAILED", 22) == 0) {
			xlog("%s: scan failed: %s", s->ifname, ev);
			s->scan_generation++;
			s->scan_last_failed = true;
			s->autojoin_armed = false;
		} else if (strncmp(ev, "CTRL-EVENT-SSID-TEMP-DISABLED", 29)
		    == 0) {
			/*
			 * wpa_supplicant blacklists an SSID after repeated auth
			 * failures. In practice this is a wrong passphrase, and
			 * it is the only signal we get that says so.
			 */
			xlog("%s: SSID temporarily disabled — authentication "
			    "is failing (wrong passphrase?): %s",
			    s->ifname, ev);
			(void)supplicant_refresh_state(s);
			changed = true;
		} else if (strncmp(ev, "CTRL-EVENT-TERMINATING", 22) == 0) {
			xlog("%s: wpa_supplicant is terminating", s->ifname);
			changed = true;
		}
	}
	if (rc < 0)
		xlog("%s: event connection error: %s", s->ifname,
		    strerror(errno));

	return (changed);
}
