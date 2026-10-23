/*
 * wlan — the WLAN command-line client. /usr/sbin/wlan
 *
 *   wlan list                       list WLAN interfaces
 *   wlan scan [<if>]                scan and print networks
 *   wlan status [<if>]              current association
 *   wlan connect <ssid> [pass]      associate (and remember the network)
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	WLAN_SERVICE_NAME	"org.nextbsd.wlan"

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
	wlan_status_t st;
	int count, i;

	if (pick_interface(svc, ifarg, name) != 0)
		return (1);

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
	 * would be rude. Give it a moment, then read the cache.
	 */
	(void)sleep(3);

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
	    "       %s disconnect [<interface>]\n",
	    progname, progname, progname, progname, progname);
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
		if (argc < 3)
			usage();
		return (cmd_connect(svc, argc > 4 ? argv[4] : NULL, argv[2],
		    argc > 3 ? argv[3] : NULL));
	}

	usage();
	return (1);
}
