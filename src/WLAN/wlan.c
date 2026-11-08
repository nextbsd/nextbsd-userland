/*
 * wlan — the WLAN command-line client. /usr/sbin/wlan
 *
 *   wlan list                       list WLAN interfaces
 *   wlan scan [<if>]                scan and print networks
 *   wlan status [<if>]              current association
 *   wlan connect <ssid> [pass]      associate (and remember the network)
 *   wlan connect <ssid> --eap ...   join a WPA-Enterprise (802.1X) network
 *   wlan disconnect [<if>]          disassociate
 *
 * A pure-C MIG client for org.nextbsd.wlan, modelled directly on
 * src/IPConfiguration/ipconfig.c — bootstrap_look_up, then straight RPCs.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS, AND WHY IT IS A CLI
 *
 * It is TWO things at once, and both are load-bearing.
 *
 * 1. It is the debugging tool. It was written when there was no scutil in this
 *    port, so no way at all to inspect the dynamic store ad hoc — and without
 *    this, a broken association was invisible. scutil(8) now shows what wland
 *    publishes to the store; wland's own state (scan results, supplicant) is
 *    still only reachable through this CLI.
 *
 * 2. It is the desktop's bridge across the Mach line. NO GERSHWIN PROCESS HAS
 *    EVER SPOKEN MACH — there is not one mach/, bootstrap_look_up, xpc_, or
 *    SystemConfiguration reference in the whole component tree, and that is
 *    deliberate: Gershwin's libdispatch is built with HAVE_MACH force-disabled so
 *    the desktop's dispatch stays isolated from the base stack's Mach one, and
 *    PORTING.md states the policy outright ("three CF lanes, one CF per binary").
 *    Linking libSystemConfiguration + libmach into a GNUstep bundle collides head
 *    on with that split.
 *
 *    The cheap path is the one already in the tree: ipconfigd is a Mach daemon
 *    and ipconfig is a pure-C CLI Mach client. Do the same. Gershwin's
 *    Network.prefPane already shells out to a privileged helper via NSTask
 *    (BSDBackend.m:262-268), so a /usr/sbin/wlan binary drops straight into the
 *    existing invocation path with NO ObjC<->Mach linkage problem at all.
 *
 * The CoreWLAN-shaped ObjC API stays ABOVE the Mach line, where ObjC actually
 * exists. That is also where it belongs architecturally.
 */
#include "wlan_mig_types.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "wlan.h"			/* MIG user stubs */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <readpassphrase.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define	WLAN_SERVICE_NAME	"org.nextbsd.wlan"

/* How often to check whether a sweep has finished, and for how long. */
#define	SCAN_POLL_USEC		(200 * 1000)
#define	SCAN_TIMEOUT_SEC	15

static const char *progname = "wlan";

static const char *
security_str(wlan_security_t s)
{
	switch (s) {
	case wlan_security_open_e:		return ("open");
	case wlan_security_wep_e:		return ("WEP");
	case wlan_security_wpa_e:		return ("WPA");
	case wlan_security_wpa2_e:		return ("WPA2");
	case wlan_security_wpa3_e:		return ("WPA3");
	case wlan_security_enterprise_e:	return ("802.1X");
	default:				return ("?");
	}
}

static const char *
state_str(wlan_state_t s)
{
	switch (s) {
	case wlan_state_inactive_e:	return ("inactive");
	case wlan_state_scanning_e:	return ("scanning");
	case wlan_state_associating_e:	return ("associating");
	case wlan_state_associated_e:	return ("associated (not yet keyed)");
	case wlan_state_4way_e:		return ("4-way handshake");
	case wlan_state_connected_e:	return ("connected");
	case wlan_state_disconnected_e:	return ("disconnected");
	default:			return ("?");
	}
}

static const char *
status_str(wlan_status_t s)
{
	switch (s) {
	case wlan_status_success_e:
		return ("success");
	case wlan_status_invalid_parameter_e:
		return ("invalid parameter");
	case wlan_status_interface_does_not_exist_e:
		return ("no such WLAN interface");
	case wlan_status_no_supplicant_e:
		return ("wpa_supplicant is not reachable");
	case wlan_status_not_associated_e:
		return ("not associated");
	case wlan_status_index_out_of_range_e:
		return ("index out of range");
	case wlan_status_operation_not_supported_e:
		return ("operation not supported");
	case wlan_status_enterprise_required_e:
		return ("this is a WPA-Enterprise (802.1X) network; "
		    "connect with --eap peap|ttls|tls");
	case wlan_status_no_trust_anchor_e:
		return ("server verification needs --ca-cert or --server-name "
		    "(--no-verify-server turns it off)");
	case wlan_status_ca_bundle_missing_e:
		return ("--server-name needs the system CA bundle "
		    "/etc/ssl/cert.pem, which is missing; give --ca-cert");
	case wlan_status_credential_file_e:
		return ("a certificate or key file is missing or unreadable, "
		    "or --ca-cert is not a valid hash://server/sha256/ pin; "
		    "see /var/log/wland.stderr");
	case wlan_status_key_file_insecure_e:
		return ("the private key file is group- or world-readable; "
		    "chmod 600 it");
	default:
		return ("internal error");
	}
}

/*
 * A crude signal bar. RSSI in dBm: -30 is excellent, -90 is unusable.
 * Purely cosmetic — but a bare "-67" tells a user nothing.
 */
static const char *
signal_bar(int rssi)
{
	if (rssi >= -50)	return ("****");
	if (rssi >= -60)	return ("*** ");
	if (rssi >= -70)	return ("**  ");
	if (rssi >= -80)	return ("*   ");
	return ("    ");
}

/*
 * Resolve the interface to act on. With no argument, use the first one wland
 * owns — which on any real machine is the only one.
 */
static int
pick_interface(mach_port_t svc, const char *arg, WLANInterfaceName out)
{
	wlan_status_t st;
	int count;

	if (arg != NULL) {
		(void)memset(out, 0, sizeof(WLANInterfaceName));
		(void)strlcpy(out, arg, sizeof(WLANInterfaceName));
		return (0);
	}
	if (wlan_if_count(svc, &count) != KERN_SUCCESS || count <= 0) {
		(void)fprintf(stderr, "%s: no WLAN interfaces\n", progname);
		return (-1);
	}
	if (wlan_if_name(svc, 0, out, &st) != KERN_SUCCESS ||
	    st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: could not resolve an interface\n",
		    progname);
		return (-1);
	}
	return (0);
}

static int
cmd_list(mach_port_t svc)
{
	int count, i;

	if (wlan_if_count(svc, &count) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (1);
	}
	if (count == 0) {
		(void)printf("no WLAN interfaces\n");
		return (0);
	}
	for (i = 0; i < count; i++) {
		WLANInterfaceName name;
		wlan_status_t st;

		if (wlan_if_name(svc, i, name, &st) != KERN_SUCCESS ||
		    st != wlan_status_success_e)
			continue;
		(void)printf("%s\n", name);
	}
	return (0);
}

static int
cmd_scan(mach_port_t svc, const char *ifarg)
{
	WLANInterfaceName name;
	struct timespec start, now;
	wlan_status_t st;
	uint32_t baseline, generation;
	boolean_t failed;
	int count, i;

	if (pick_interface(svc, ifarg, name) != 0)
		return (1);

	/* Read this BEFORE asking, so only a sweep that ends later counts. */
	if (wlan_scan_generation(svc, name, &baseline, &failed, &st)
	    != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (1);
	}
	if (st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: scan: %s\n", progname,
		    status_str(st));
		return (1);
	}

	if (wlan_scan(svc, name, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (1);
	}
	if (st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: scan: %s\n", progname,
		    status_str(st));
		return (1);
	}

	/*
	 * The scan RPC returns as soon as the scan is REQUESTED — a full-band
	 * sweep takes seconds, and holding a Mach reply port open that long
	 * would be rude. Poll until wland says the sweep has finished. The
	 * driver has no timeout of its own, so this one is ours: past it, the
	 * cache is still worth printing.
	 */
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (wlan_scan_generation(svc, name, &generation, &failed, &st)
		    != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (1);
		}
		if (st != wlan_status_success_e) {
			(void)fprintf(stderr, "%s: scan: %s\n", progname,
			    status_str(st));
			return (1);
		}
		/* Signed difference: correct across a counter wrap. */
		if ((int32_t)(generation - baseline) > 0) {
			if (failed) {
				(void)fprintf(stderr, "%s: scan failed\n",
				    progname);
				return (1);
			}
			break;
		}
		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - start.tv_sec) * 1000 +
		    (now.tv_nsec - start.tv_nsec) / 1000000 >=
		    SCAN_TIMEOUT_SEC * 1000) {
			(void)fprintf(stderr, "%s: scan did not finish within "
			    "%d seconds; showing cached results\n", progname,
			    SCAN_TIMEOUT_SEC);
			break;
		}
		(void)usleep(SCAN_POLL_USEC);
	}

	if (wlan_scan_count(svc, name, &count, &st) != KERN_SUCCESS ||
	    st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: scan_count: %s\n", progname,
		    status_str(st));
		return (1);
	}
	if (count == 0) {
		(void)printf("no networks found\n");
		return (0);
	}

	(void)printf("%-32s %-18s %-6s %4s %s\n",
	    "SSID", "BSSID", "SIGNAL", "CH", "SECURITY");
	for (i = 0; i < count; i++) {
		WLANSSID ssid;
		WLANBSSID bssid;
		wlan_security_t sec;
		int rssi, ch;

		if (wlan_scan_entry(svc, name, i, ssid, bssid, &rssi, &ch,
		    &sec, &st) != KERN_SUCCESS || st != wlan_status_success_e)
			continue;
		(void)printf("%-32s %-18s %s %3d %4d %s\n",
		    ssid, bssid, signal_bar(rssi), rssi, ch,
		    security_str(sec));
	}
	return (0);
}

static int
cmd_status(mach_port_t svc, const char *ifarg)
{
	WLANInterfaceName name;
	WLANSSID ssid;
	WLANBSSID bssid;
	wlan_state_t state;
	wlan_security_t sec;
	wlan_status_t st;
	int rssi, ch;

	if (pick_interface(svc, ifarg, name) != 0)
		return (1);

	if (wlan_status(svc, name, &state, ssid, bssid, &rssi, &ch, &sec, &st)
	    != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (1);
	}
	if (st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: %s\n", progname, status_str(st));
		return (1);
	}

	(void)printf("interface: %s\n", name);
	(void)printf("state:     %s\n", state_str(state));
	if (state == wlan_state_connected_e || ssid[0] != '\0') {
		(void)printf("ssid:      %s\n", ssid);
		(void)printf("bssid:     %s\n", bssid);
		(void)printf("signal:    %d dBm\n", rssi);
		(void)printf("channel:   %d\n", ch);
		(void)printf("security:  %s\n", security_str(sec));
	}

	/*
	 * Spell this out, because it is the single most confusing thing about
	 * 802.11 for anyone debugging a "connected but no IP" report: the link
	 * is UP at association, but DHCP is deliberately held until the 4-way
	 * handshake completes. "associated" is not "ready".
	 */
	if (state == wlan_state_associated_e)
		(void)printf("\nnote: associated but NOT yet keyed — DHCP is "
		    "held until the 4-way handshake completes.\n");

	return (0);
}

static int
cmd_connect(mach_port_t svc, const char *ifarg, const char *ssid,
    const char *pass)
{
	WLANInterfaceName name;
	WLANSSID cf_ssid;
	WLANPassphrase cf_psk;
	wlan_status_t st;

	if (pick_interface(svc, ifarg, name) != 0)
		return (1);

	(void)memset(cf_ssid, 0, sizeof(cf_ssid));
	(void)memset(cf_psk, 0, sizeof(cf_psk));
	(void)strlcpy(cf_ssid, ssid, sizeof(cf_ssid));
	if (pass != NULL)
		(void)strlcpy(cf_psk, pass, sizeof(cf_psk));

	if (wlan_connect(svc, name, cf_ssid, cf_psk, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (1);
	}
	if (st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: connect: %s\n", progname,
		    status_str(st));
		return (1);
	}
	(void)printf("associating with '%s' on %s...\n", ssid, name);
	(void)printf("(run `%s status` to watch; DHCP starts once the 4-way "
	    "handshake completes)\n", progname);
	return (0);
}

/* Everything that goes in wlan_connect_eap's fixed-size arguments. */
struct eap_args {
	WLANEAPName	identity;
	WLANEAPName	anonymous_identity;
	WLANEAPSecret	password;
	WLANEAPPath	ca_cert;
	WLANEAPName	server_name;
	WLANEAPPath	client_cert;
	WLANEAPPath	private_key;
	WLANEAPSecret	key_passphrase;
};

/* Copy an option into an RPC field, refusing to cut it short. */
static int
field_set(char *dst, size_t dst_sz, const char *src, const char *what)
{
	if (strlcpy(dst, src, dst_sz) >= dst_sz) {
		(void)fprintf(stderr, "%s: %s is too long\n", progname, what);
		return (-1);
	}
	return (0);
}

/*
 * Copy a file path into an RPC field. wland checks the file and wpa_supplicant
 * opens it, neither from this directory, so a relative path is resolved here.
 * An absolute one is passed as given: wland can read files this process can't.
 */
static int
path_set(char *dst, size_t dst_sz, const char *path, const char *what)
{
	char resolved[PATH_MAX];

	if (path[0] == '/')
		return (field_set(dst, dst_sz, path, what));
	if (realpath(path, resolved) == NULL) {
		(void)fprintf(stderr, "%s: %s: %s: %s\n", progname, what, path,
		    strerror(errno));
		return (-1);
	}
	return (field_set(dst, dst_sz, resolved, what));
}

/* Read the password: no echo from a terminal, otherwise one line of stdin. */
static int
password_read(char *buf, size_t buf_sz, const char *user)
{
	char prompt[WLAN_EAP_STR_MAX + 32];
	size_t len;

	if (isatty(STDIN_FILENO)) {
		(void)snprintf(prompt, sizeof(prompt), "802.1X password for %s: ",
		    user);
		if (readpassphrase(prompt, buf, buf_sz, RPP_REQUIRE_TTY) == NULL) {
			(void)fprintf(stderr, "%s: could not read the password: "
			    "%s\n", progname, strerror(errno));
			return (-1);
		}
	} else if (fgets(buf, (int)buf_sz, stdin) == NULL) {
		(void)fprintf(stderr, "%s: no password on standard input\n",
		    progname);
		return (-1);
	}
	len = strcspn(buf, "\r\n");
	if (buf[len] == '\0' && len == buf_sz - 1) {
		(void)fprintf(stderr, "%s: password is too long\n", progname);
		return (-1);
	}
	buf[len] = '\0';
	if (len == 0) {
		(void)fprintf(stderr, "%s: empty password\n", progname);
		return (-1);
	}
	return (0);
}

/* A PKCS#12 file holds the client certificate as well as the key. */
static bool
key_is_pkcs12(const char *path)
{
	const char *dot = strrchr(path, '.');

	return (dot != NULL &&
	    (strcasecmp(dot, ".p12") == 0 || strcasecmp(dot, ".pfx") == 0));
}

enum {
	OPT_EAP = 256,
	OPT_USER,
	OPT_PASSWORD,
	OPT_ANON,
	OPT_TTLS_INNER,
	OPT_CA_CERT,
	OPT_SERVER_NAME,
	OPT_NO_VERIFY,
	OPT_CLIENT_CERT,
	OPT_PRIVATE_KEY,
	OPT_KEY_PASSPHRASE
};

static const struct option eap_longopts[] = {
	{ "eap",		required_argument,	NULL, OPT_EAP },
	{ "user",		required_argument,	NULL, OPT_USER },
	{ "password",		required_argument,	NULL, OPT_PASSWORD },
	{ "anonymous-identity",	required_argument,	NULL, OPT_ANON },
	{ "ttls-inner",		required_argument,	NULL, OPT_TTLS_INNER },
	{ "ca-cert",		required_argument,	NULL, OPT_CA_CERT },
	{ "server-name",	required_argument,	NULL, OPT_SERVER_NAME },
	{ "no-verify-server",	no_argument,		NULL, OPT_NO_VERIFY },
	{ "client-cert",	required_argument,	NULL, OPT_CLIENT_CERT },
	{ "private-key",	required_argument,	NULL, OPT_PRIVATE_KEY },
	{ "key-passphrase",	required_argument,	NULL, OPT_KEY_PASSPHRASE },
	{ NULL,			0,			NULL, 0 }
};

/*
 * wlan connect <ssid> --eap peap|ttls|tls [options] [<interface>]
 *
 * argv[0] is the SSID and argv[1..] the rest: getopt_long treats argv[0] as the
 * program name, so an SSID beginning with '-' is not taken for an option.
 */
static int
cmd_connect_eap(mach_port_t svc, int argc, char **argv)
{
	struct eap_args a;
	WLANInterfaceName name;
	WLANSSID cf_ssid;
	wlan_status_t st;
	wlan_ttls_inner_t inner = wlan_ttls_inner_mschapv2_e;
	const char *ssid = argv[0], *ifarg;
	const char *user = NULL, *password = NULL, *anon = NULL;
	const char *ca = NULL, *server = NULL, *client = NULL, *key = NULL;
	const char *keypass = NULL, *inner_arg = NULL;
	bool no_verify = false;
	int eap_type = 0, ch, rc = 1;
	kern_return_t kr;

	(void)memset(&a, 0, sizeof(a));

	optind = 1;
	optreset = 1;
	while ((ch = getopt_long(argc, argv, "", eap_longopts, NULL)) != -1) {
		switch (ch) {
		case OPT_EAP:
			if (strcmp(optarg, "peap") == 0)
				eap_type = WLAN_EAP_TYPE_PEAP;
			else if (strcmp(optarg, "ttls") == 0)
				eap_type = WLAN_EAP_TYPE_TTLS;
			else if (strcmp(optarg, "tls") == 0)
				eap_type = WLAN_EAP_TYPE_TLS;
			else {
				(void)fprintf(stderr, "%s: --eap takes peap, "
				    "ttls or tls\n", progname);
				return (1);
			}
			break;
		case OPT_USER:		user = optarg;		break;
		case OPT_PASSWORD:	password = optarg;	break;
		case OPT_ANON:		anon = optarg;		break;
		case OPT_TTLS_INNER:	inner_arg = optarg;	break;
		case OPT_CA_CERT:	ca = optarg;		break;
		case OPT_SERVER_NAME:	server = optarg;	break;
		case OPT_NO_VERIFY:	no_verify = true;	break;
		case OPT_CLIENT_CERT:	client = optarg;	break;
		case OPT_PRIVATE_KEY:	key = optarg;		break;
		case OPT_KEY_PASSPHRASE: keypass = optarg;	break;
		default:
			return (1);	/* getopt_long said why */
		}
	}
	if (argc - optind > 1) {
		(void)fprintf(stderr, "%s: too many arguments\n", progname);
		return (1);
	}
	ifarg = (optind < argc) ? argv[optind] : NULL;

	if (user == NULL) {
		(void)fprintf(stderr, "%s: --user is required\n", progname);
		return (1);
	}
	if (inner_arg != NULL) {
		if (eap_type != WLAN_EAP_TYPE_TTLS) {
			(void)fprintf(stderr, "%s: --ttls-inner is only for "
			    "--eap ttls\n", progname);
			return (1);
		}
		if (strcmp(inner_arg, "mschapv2") == 0)
			inner = wlan_ttls_inner_mschapv2_e;
		else if (strcmp(inner_arg, "pap") == 0)
			inner = wlan_ttls_inner_pap_e;
		else if (strcmp(inner_arg, "chap") == 0)
			inner = wlan_ttls_inner_chap_e;
		else {
			(void)fprintf(stderr, "%s: --ttls-inner takes mschapv2, "
			    "pap or chap\n", progname);
			return (1);
		}
	}
	/* Before prompting, so a doomed attempt asks for nothing. */
	if (no_verify && (ca != NULL || server != NULL)) {
		(void)fprintf(stderr, "%s: --no-verify-server cannot be "
		    "combined with --ca-cert or --server-name\n", progname);
		return (1);
	}
	if (!no_verify && ca == NULL && server == NULL) {
		(void)fprintf(stderr, "%s: %s\n", progname,
		    status_str(wlan_status_no_trust_anchor_e));
		return (1);
	}
	if (eap_type == WLAN_EAP_TYPE_TLS) {
		if (key == NULL) {
			(void)fprintf(stderr, "%s: --eap tls needs "
			    "--private-key\n", progname);
			return (1);
		}
		if (client == NULL && !key_is_pkcs12(key)) {
			(void)fprintf(stderr, "%s: --eap tls needs --client-cert "
			    "(unless the key is a .p12 or .pfx file)\n",
			    progname);
			return (1);
		}
	}

	(void)memset(cf_ssid, 0, sizeof(cf_ssid));
	if (field_set(cf_ssid, sizeof(cf_ssid), ssid, "the SSID") != 0 ||
	    field_set(a.identity, sizeof(a.identity), user, "--user") != 0 ||
	    (anon != NULL && field_set(a.anonymous_identity,
	    sizeof(a.anonymous_identity), anon, "--anonymous-identity") != 0) ||
	    (server != NULL && field_set(a.server_name, sizeof(a.server_name),
	    server, "--server-name") != 0) ||
	    (client != NULL && path_set(a.client_cert, sizeof(a.client_cert),
	    client, "--client-cert") != 0) ||
	    (key != NULL && path_set(a.private_key, sizeof(a.private_key), key,
	    "--private-key") != 0) ||
	    (keypass != NULL && field_set(a.key_passphrase,
	    sizeof(a.key_passphrase), keypass, "--key-passphrase") != 0))
		goto out;
	if (ca != NULL) {
		if (strncmp(ca, "hash://", 7) == 0) {
			if (field_set(a.ca_cert, sizeof(a.ca_cert), ca,
			    "--ca-cert") != 0)
				goto out;
		} else if (path_set(a.ca_cert, sizeof(a.ca_cert), ca,
		    "--ca-cert") != 0)
			goto out;
	}

	if (eap_type != WLAN_EAP_TYPE_TLS) {
		if (password != NULL) {
			if (field_set(a.password, sizeof(a.password), password,
			    "--password") != 0)
				goto out;
		} else if (password_read(a.password, sizeof(a.password),
		    user) != 0) {
			goto out;
		}
	}

	if (pick_interface(svc, ifarg, name) != 0)
		goto out;

	kr = wlan_connect_eap(svc, name, cf_ssid, eap_type, inner, a.identity,
	    a.anonymous_identity, a.password, a.ca_cert, a.server_name,
	    a.client_cert, a.private_key, a.key_passphrase,
	    no_verify ? TRUE : FALSE, &st);
	if (kr != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		goto out;
	}
	if (st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: connect: %s\n", progname,
		    status_str(st));
		goto out;
	}
	(void)printf("associating with '%s' on %s (802.1X)...\n", ssid, name);
	(void)printf("(run `%s status` to watch; authentication errors are in "
	    "/var/log/wland.stderr)\n", progname);
	rc = 0;
out:
	explicit_bzero(&a, sizeof(a));
	return (rc);
}

static int
cmd_disconnect(mach_port_t svc, const char *ifarg)
{
	WLANInterfaceName name;
	wlan_status_t st;

	if (pick_interface(svc, ifarg, name) != 0)
		return (1);
	if (wlan_disconnect(svc, name, &st) != KERN_SUCCESS ||
	    st != wlan_status_success_e) {
		(void)fprintf(stderr, "%s: disconnect: %s\n", progname,
		    status_str(st));
		return (1);
	}
	(void)printf("disconnected %s\n", name);
	return (0);
}

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s list\n"
	    "       %s scan [<interface>]\n"
	    "       %s status [<interface>]\n"
	    "       %s connect <ssid> [passphrase] [<interface>]\n"
	    "       %s connect <ssid> --eap peap|ttls|tls --user <name>\n"
	    "            [--password <password>] [--anonymous-identity <name>]\n"
	    "            [--ttls-inner mschapv2|pap|chap]\n"
	    "            [--ca-cert <path>|hash://server/sha256/<hex>]\n"
	    "            [--server-name <name>] [--no-verify-server]\n"
	    "            [--client-cert <path>] [--private-key <path>]\n"
	    "            [--key-passphrase <passphrase>] [<interface>]\n"
	    "       %s disconnect [<interface>]\n",
	    progname, progname, progname, progname, progname, progname);
	exit(1);
}

int
main(int argc, char **argv)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	const char *cmd;

	if (argv[0] != NULL) {
		const char *slash = strrchr(argv[0], '/');

		progname = (slash != NULL) ? slash + 1 : argv[0];
	}
	if (argc < 2)
		usage();
	cmd = argv[1];

	kr = bootstrap_look_up(bootstrap_port, WLAN_SERVICE_NAME, &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		(void)fprintf(stderr,
		    "%s: cannot reach %s — is wland running?\n",
		    progname, WLAN_SERVICE_NAME);
		return (1);
	}

	if (strcmp(cmd, "list") == 0)
		return (cmd_list(svc));
	if (strcmp(cmd, "scan") == 0)
		return (cmd_scan(svc, argc > 2 ? argv[2] : NULL));
	if (strcmp(cmd, "status") == 0)
		return (cmd_status(svc, argc > 2 ? argv[2] : NULL));
	if (strcmp(cmd, "disconnect") == 0)
		return (cmd_disconnect(svc, argc > 2 ? argv[2] : NULL));
	if (strcmp(cmd, "connect") == 0) {
		int i;

		if (argc < 3)
			usage();
		/*
		 * Only --eap selects the 802.1X form. Anything else keeps its
		 * old meaning, so a passphrase may begin with "--".
		 */
		for (i = 3; i < argc; i++) {
			if (strcmp(argv[i], "--eap") == 0)
				return (cmd_connect_eap(svc, argc - 2, argv + 2));
		}
		return (cmd_connect(svc, argc > 4 ? argv[4] : NULL, argv[2],
		    argc > 3 ? argv[3] : NULL));
	}

	usage();
	return (1);
}
