/*
 * supplicant.c — wland's policy layer over stock wpa_supplicant.
 * See supplicant.h for why wland is the sole author of the network blocks.
 */
#include "supplicant.h"
#include "wpa_ctrl.h"

#include <net/if.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	SCAN_MAX	64

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

int
supplicant_connect(struct supplicant *s, const char *ssid, const char *psk)
{
	char cmd[256], reply[64];
	int id;

	if (s == NULL || ssid == NULL || ssid[0] == '\0')
		return (-1);

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
