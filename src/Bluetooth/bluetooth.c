/*
 * bluetooth — the Bluetooth command-line client. /usr/sbin/bluetooth
 *
 *   bluetooth status                        daemon, policy and controllers
 *   bluetooth list                          paired devices
 *   bluetooth power on|off [node]           power (persisted)
 *   bluetooth discoverable on|off [-t s] [node]
 *   bluetooth scan [-t seconds] [node]      discover nearby devices
 *   bluetooth pair <address> [pin]          pair, prompting as needed
 *   bluetooth confirm <address> yes|no|<passkey or PIN>
 *   bluetooth unpair <address>              forget a device
 *   bluetooth sdp <addr>                    a remote device's SDP records
 *
 * A pure-C MIG client for org.nextbsd.bluetooth, modelled on wlan(8):
 * bootstrap_look_up, then straight RPCs. It links only liblaunch and
 * libsystem_kernel, no CoreFoundation and no SystemConfiguration, so it is
 * also the bridge across the Mach line for anything above it: a Gershwin pane
 * runs it the way Network.prefPane runs its helper (see src/WLAN/wlan.c for
 * why no Gershwin process speaks Mach).
 *
 * The verbs live in one table. A later verb is a row and a function:
 * connect / disconnect (#213) and the diagnostics info / ping / browse
 * (#211). Usage text is generated from the table.
 *
 * Exit status: 0 ok; 1 error or RPC failure; 2 usage; 3 an answer is needed
 * (pair --no-prompt, #211).
 */
#include "bluetooth_mig_types.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "bluetooth.h"			/* MIG user stubs */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	BT_SERVICE_NAME		"org.nextbsd.bluetooth"

#define	EXIT_OK			0
#define	EXIT_ERR		1
#define	EXIT_USAGE		2
#define	EXIT_ANSWER		3

static const char *progname = "bluetooth";

static const char *
status_str(bt_status_t s)
{
	switch (s) {
	case bt_status_success_e:
		return ("success");
	case bt_status_invalid_parameter_e:
		return ("invalid parameter");
	case bt_status_no_adapter_e:
		return ("no Bluetooth controller");
	case bt_status_index_out_of_range_e:
		return ("index out of range");
	case bt_status_operation_not_supported_e:
		return ("operation not supported");
	case bt_status_powered_off_e:
		return ("Bluetooth is powered off");
	case bt_status_busy_e:
		return ("busy");
	case bt_status_not_paired_e:
		return ("device is not paired");
	case bt_status_not_connected_e:
		return ("device is not connected");
	case bt_status_no_such_device_e:
		return ("no such device");
	case bt_status_pairing_failed_e:
		return ("pairing failed");
	case bt_status_pairing_timeout_e:
		return ("pairing timed out");
	case bt_status_pairing_rejected_e:
		return ("pairing rejected");
	case bt_status_no_prompt_e:
		return ("nothing to confirm");
	case bt_status_store_error_e:
		return ("could not update the pairing store");
	default:
		return ("internal error");
	}
}

static const char *
daemon_state_str(bt_daemon_state_t s)
{
	switch (s) {
	case bt_daemon_idle_e:		return ("idle (no controller)");
	case bt_daemon_active_e:	return ("active");
	case bt_daemon_no_ioregistry_e:	return ("blind (no IORegistry)");
	default:			return ("?");
	}
}

static const char *
adapter_state_str(bt_adapter_state_t s)
{
	switch (s) {
	case bt_adapter_arrived_e:		return ("arrived");
	case bt_adapter_firmware_e:		return ("loading firmware");
	case bt_adapter_initializing_e:		return ("initializing");
	case bt_adapter_powered_on_e:		return ("on");
	case bt_adapter_powered_off_e:		return ("off");
	case bt_adapter_error_e:		return ("error");
	default:				return ("?");
	}
}

static const char *
device_state_str(bt_device_state_t s)
{
	switch (s) {
	case bt_device_disconnected_e:	return ("disconnected");
	case bt_device_connecting_e:	return ("connecting");
	case bt_device_connected_e:	return ("connected");
	case bt_device_pairing_e:	return ("pairing");
	default:			return ("?");
	}
}

static const char *
transports_str(int t)
{
	if ((t & BT_TRANSPORT_BREDR) != 0 && (t & BT_TRANSPORT_LE) != 0)
		return ("dual");
	if ((t & BT_TRANSPORT_LE) != 0)
		return ("LE");
	if ((t & BT_TRANSPORT_BREDR) != 0)
		return ("BR/EDR");
	return ("-");
}

static const char *
firmware_state_str(bt_firmware_state_t s)
{
	switch (s) {
	case bt_firmware_checking_e:	return ("checking");
	case bt_firmware_downloading_e:	return ("downloading");
	case bt_firmware_done_e:	return ("loaded");
	case bt_firmware_operational_e:	return ("not needed");
	case bt_firmware_failed_e:	return ("FAILED");
	default:			return ("-");
	}
}

/* A crude signal bar, as wlan(8)'s. RSSI in dBm; 127 means unknown. */
static const char *
signal_bar(int rssi)
{
	if (rssi == 127)	return ("    ");
	if (rssi >= -50)	return ("****");
	if (rssi >= -60)	return ("*** ");
	if (rssi >= -70)	return ("**  ");
	if (rssi >= -80)	return ("*   ");
	return ("    ");
}

/* The Class of Device's major class, and the minor where it matters. */
static const char *
class_str(int cod)
{
	int major = (cod >> 8) & 0x1f, minor = (cod >> 2) & 0x3f;

	switch (major) {
	case 0x01:	return ("computer");
	case 0x02:	return ("phone");
	case 0x03:	return ("network");
	case 0x04:
		switch (minor) {
		case 0x01:	return ("headset");
		case 0x02:	return ("hands-free");
		case 0x05:	return ("speaker");
		case 0x06:	return ("headphones");
		case 0x0a:	return ("hifi audio");
		default:	return ("audio");
		}
	case 0x05:
		switch (minor & 0x30) {
		case 0x10:	return ("keyboard");
		case 0x20:	return ("mouse");
		case 0x30:	return ("keyboard+mouse");
		default:	return ("peripheral");
		}
	case 0x06:	return ("imaging");
	case 0x07:	return ("wearable");
	case 0x08:	return ("toy");
	case 0x09:	return ("health");
	default:	return ("-");
	}
}

/* "on" / "off" -> 1 / 0, else -1. */
static int
parse_onoff(const char *s)
{
	if (s == NULL)
		return (-1);
	if (strcmp(s, "on") == 0)
		return (1);
	if (strcmp(s, "off") == 0)
		return (0);
	return (-1);
}

/* Errors common to every controller verb, with the ticket's wording. */
static int
fail(const char *verb, bt_status_t st)
{
	if (st == bt_status_no_adapter_e)
		(void)fprintf(stderr, "%s: no Bluetooth adapters\n", progname);
	else if (st == bt_status_powered_off_e)
		(void)fprintf(stderr, "%s: adapter is powered off\n", progname);
	else
		(void)fprintf(stderr, "%s: %s: %s\n", progname, verb,
		    status_str(st));
	return (EXIT_ERR);
}

/* Sleep `ms` milliseconds between polls of the daemon. */
static void
nap(int ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000;
	(void)nanosleep(&ts, NULL);
}

static int
cmd_status(mach_port_t svc, int argc, char **argv)
{
	bt_daemon_state_t state;
	int nadapters, npaired, power, disc, autoc, i;

	(void)argc;
	(void)argv;

	if (bt_status(svc, &state, &nadapters, &npaired, &power, &disc,
	    &autoc) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}

	(void)printf("state:         %s\n", daemon_state_str(state));
	(void)printf("power:         %s\n", power ? "on" : "off");
	(void)printf("discoverable:  %s\n", disc ? "on" : "off");
	(void)printf("auto-connect:  %s\n", autoc ? "on" : "off");
	(void)printf("paired:        %d\n", npaired);

	for (i = 0; ; i++) {
		BTUSBDeviceName dev;
		bt_firmware_state_t fst;
		bt_status_t st;
		int vendor, product;

		if (bt_firmware_info(svc, i, dev, &vendor, &product, &fst,
		    &st) != KERN_SUCCESS || st != bt_status_success_e)
			break;
		(void)printf("firmware:      %s %04x:%04x %s\n", dev, vendor,
		    product, firmware_state_str(fst));
	}

	if (nadapters == 0) {
		(void)printf("controllers:   none\n");
		return (EXIT_OK);
	}
	for (i = 0; i < nadapters; i++) {
		BTNodeName node;
		BTTransportName transport;
		BTAddressString address;
		BTDeviceName name;
		bt_adapter_state_t ast;
		bt_status_t st;
		int vendor, product;

		if (bt_adapter_info(svc, i, node, transport, &vendor, &product,
		    &ast, address, name, &st) != KERN_SUCCESS ||
		    st != bt_status_success_e)
			continue;
		(void)printf("\ncontroller:    %s\n", node);
		(void)printf("  transport:   %s %04x:%04x\n", transport,
		    vendor, product);
		(void)printf("  state:       %s\n", adapter_state_str(ast));
		if (address[0] != '\0')
			(void)printf("  address:     %s\n", address);
		if (name[0] != '\0')
			(void)printf("  name:        %s\n", name);
		{
			int pw, dsc, conn, scn, feat;

			if (bt_adapter_flags(svc, i, &pw, &dsc, &conn, &scn,
			    &feat, &st) == KERN_SUCCESS &&
			    st == bt_status_success_e) {
				(void)printf("  powered:     %s\n",
				    pw ? "on" : "off");
				(void)printf("  discoverable:%s%s\n",
				    dsc ? " on" : " off",
				    scn ? " (scanning)" : "");
				(void)printf("  features:    %s%s%s\n",
				    (feat & BT_ADAPTER_FEAT_SSP) ?
				    "SSP" : "legacy pairing",
				    (feat & BT_ADAPTER_FEAT_LE) ? " LE" : "",
				    (feat & BT_ADAPTER_FEAT_EIR) ? " EIR" : "");
			}
		}
	}
	return (EXIT_OK);
}

static int
cmd_list(mach_port_t svc, int argc, char **argv)
{
	bt_status_t st;
	int count, i;

	(void)argc;
	(void)argv;

	if (bt_paired_count(svc, &count, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e) {
		(void)fprintf(stderr, "%s: list: %s\n", progname,
		    status_str(st));
		return (EXIT_ERR);
	}
	if (count == 0) {
		(void)printf("no paired devices\n");
		return (EXIT_OK);
	}

	(void)printf("%-24s %-24s %-6s %-8s %-4s %-12s %s\n",
	    "ADDRESS", "NAME", "CLASS", "LINK", "AUTO", "STATE", "PROFILES");
	for (i = 0; i < count; i++) {
		BTAddressString address;
		BTDeviceName name;
		BTProfileList profiles;
		bt_device_state_t dst;
		int cod, transports, autoc;

		if (bt_paired_entry(svc, i, address, name, &cod, &transports,
		    profiles, &autoc, &dst, &st) != KERN_SUCCESS ||
		    st != bt_status_success_e)
			continue;
		(void)printf("%-24s %-24.24s %06x %-8s %-4s %-12s %s\n",
		    address, name[0] != '\0' ? name : "-", cod & 0xffffff,
		    transports_str(transports), autoc ? "yes" : "no",
		    device_state_str(dst),
		    profiles[0] != '\0' ? profiles : "-");
	}
	return (EXIT_OK);
}

static int
cmd_power(mach_port_t svc, int argc, char **argv)
{
	BTNodeName node;
	bt_status_t st;
	int on;

	on = parse_onoff(argc > 1 ? argv[1] : NULL);
	if (on < 0 || argc > 3)
		return (-1);
	(void)memset(node, 0, sizeof(node));
	if (argc > 2)
		(void)strlcpy(node, argv[2], sizeof(node));
	if (bt_adapter_power(svc, node, on, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("power", st));
	return (EXIT_OK);
}

static int
cmd_discoverable(mach_port_t svc, int argc, char **argv)
{
	BTNodeName node;
	bt_status_t st;
	int on, i, seconds = 0;

	if (argc < 2 || (on = parse_onoff(argv[1])) < 0)
		return (-1);
	(void)memset(node, 0, sizeof(node));
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (node[0] == '\0')
			(void)strlcpy(node, argv[i], sizeof(node));
		else
			return (-1);
	}
	if (bt_adapter_discoverable(svc, node, on, seconds, &st) !=
	    KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("discoverable", st));
	return (EXIT_OK);
}

static int
cmd_scan(mach_port_t svc, int argc, char **argv)
{
	BTNodeName node;
	bt_status_t st;
	int i, count, scanning, seconds = 10, waited;

	(void)memset(node, 0, sizeof(node));
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (node[0] == '\0')
			(void)strlcpy(node, argv[i], sizeof(node));
		else
			return (-1);
	}
	if (seconds <= 0 || seconds > 60)
		seconds = 10;

	if (bt_scan(svc, node, seconds, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("scan", st));

	/*
	 * The daemon answers at once and scans on its own; follow it until it
	 * says it is done (inquiry, then the names), with an upper bound.
	 */
	(void)fprintf(stderr, "scanning for %d seconds...\n", seconds);
	count = scanning = 0;
	for (waited = 0; waited < (seconds + 20) * 1000; waited += 250) {
		nap(250);
		if (bt_scan_count(svc, node, &count, &scanning, &st) !=
		    KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
		if (st != bt_status_success_e)
			return (fail("scan", st));
		if (!scanning && waited >= 500)
			break;
	}

	if (count == 0) {
		(void)printf("no devices found\n");
		return (EXIT_OK);
	}
	(void)printf("%-24s %-28s %-15s %-12s %s\n", "ADDRESS", "NAME",
	    "CLASS", "SIGNAL", "PAIRED");
	for (i = 0; i < count; i++) {
		BTAddressString address;
		BTDeviceName name;
		int cod, rssi, transports, appearance, paired, age;
		char sig[16];

		if (bt_scan_entry(svc, node, i, address, name, &cod, &rssi,
		    &transports, &appearance, &paired, &age, &st) !=
		    KERN_SUCCESS || st != bt_status_success_e)
			continue;
		if (rssi == 127)
			(void)snprintf(sig, sizeof(sig), "-");
		else
			(void)snprintf(sig, sizeof(sig), "%s %d",
			    signal_bar(rssi), rssi);
		(void)printf("%-24s %-28.28s %-15s %-12s %s\n", address,
		    name[0] != '\0' ? name : "-", class_str(cod), sig,
		    paired ? "yes" : "no");
	}
	return (EXIT_OK);
}

/* Read one line from the terminal. */
static bool
ask(const char *question, char *buf, size_t len)
{
	char *nl;

	(void)printf("%s", question);
	(void)fflush(stdout);
	if (fgets(buf, (int)len, stdin) == NULL)
		return (false);
	if ((nl = strchr(buf, '\n')) != NULL)
		*nl = '\0';
	return (true);
}

static bool
all_digits(const char *s)
{
	if (*s == '\0')
		return (false);
	for (; *s != '\0'; s++) {
		if (!isdigit((unsigned char)*s))
			return (false);
	}
	return (true);
}

/*
 * Answer a prompt: "yes" / "no", a passkey, or a PIN. Shared by pair (when
 * it asks on the terminal) and confirm.
 */
static int
answer(mach_port_t svc, const char *address, bt_pair_method_t method,
    const char *reply)
{
	BTAddressString addr;
	BTPinString pin;
	bt_status_t st;
	int accept = 1, passkey = 0;

	(void)memset(addr, 0, sizeof(addr));
	(void)strlcpy(addr, address, sizeof(addr));
	(void)memset(pin, 0, sizeof(pin));
	if (strcmp(reply, "no") == 0 || strcmp(reply, "n") == 0)
		accept = 0;
	else if (method == bt_pair_pin_e && strcmp(reply, "yes") != 0)
		(void)strlcpy(pin, reply, sizeof(pin));
	else if (method == bt_pair_passkey_entry_e) {
		if (!all_digits(reply) || strlen(reply) > 6) {
			(void)fprintf(stderr, "%s: a passkey is up to six "
			    "digits\n", progname);
			return (EXIT_USAGE);
		}
		passkey = atoi(reply);
	} else if (strcmp(reply, "yes") != 0 && strcmp(reply, "y") != 0 &&
	    !all_digits(reply)) {
		(void)fprintf(stderr, "%s: answer yes or no\n", progname);
		return (EXIT_USAGE);
	} else if (all_digits(reply))
		passkey = atoi(reply);

	if (bt_pair_confirm(svc, addr, accept, passkey, pin, &st) !=
	    KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	(void)memset(pin, 0, sizeof(pin));
	if (st != bt_status_success_e)
		return (fail("confirm", st));
	return (EXIT_OK);
}

static void
show_prompt(const char *address, bt_pair_method_t m, int passkey)
{
	switch (m) {
	case bt_pair_numeric_comparison_e:
		(void)printf("%s shows %06d? Confirm with: %s confirm %s "
		    "yes|no\n", address, passkey, progname, address);
		break;
	case bt_pair_passkey_entry_e:
		(void)printf("Enter the passkey %s shows: %s confirm %s "
		    "<passkey>\n", address, progname, address);
		break;
	case bt_pair_passkey_display_e:
		(void)printf("Type %06d on %s, then press Enter on it\n",
		    passkey, address);
		break;
	case bt_pair_pin_e:
		(void)printf("Enter the PIN for %s: %s confirm %s <pin>\n",
		    address, progname, address);
		break;
	default:
		(void)printf("Accept pairing with %s? %s confirm %s yes|no\n",
		    address, progname, address);
		break;
	}
}

static int
cmd_pair(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	BTPinString pin;
	bt_pair_state_t state;
	bt_pair_method_t method;
	bt_status_t st, result;
	int passkey, expires, waited, shown_passkey = -1;
	bool interactive = isatty(STDIN_FILENO) != 0;

	if (argc < 2 || argc > 3)
		return (-1);
	(void)memset(addr, 0, sizeof(addr));
	(void)strlcpy(addr, argv[1], sizeof(addr));
	(void)memset(pin, 0, sizeof(pin));
	if (argc > 2)
		(void)strlcpy(pin, argv[2], sizeof(pin));

	if (bt_pair(svc, addr, pin, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	(void)memset(pin, 0, sizeof(pin));
	if (st != bt_status_success_e)
		return (fail("pair", st));

	/* Follow the pairing; answer prompts on the terminal if there is one. */
	for (waited = 0; waited < 150 * 1000; waited += 250) {
		if (bt_pair_status(svc, addr, &state, &method, &passkey,
		    &expires, &result, &st) != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
		if (state == bt_pair_state_succeeded_e) {
			(void)printf("paired with %s\n", addr);
			return (EXIT_OK);
		}
		if (state == bt_pair_state_failed_e || state ==
		    bt_pair_state_none_e)
			return (fail("pair", state == bt_pair_state_none_e ?
			    bt_status_pairing_failed_e : result));
		if (state == bt_pair_state_waiting_e) {
			char line[64];

			if (method == bt_pair_passkey_display_e) {
				/* Nothing to answer here; the peer types it. */
				if (passkey != shown_passkey) {
					show_prompt(addr, method, passkey);
					shown_passkey = passkey;
				}
			} else if (!interactive) {
				show_prompt(addr, method, passkey);
				return (EXIT_ANSWER);
			} else {
				char q[160];
				int rc;

				if (method == bt_pair_numeric_comparison_e)
					(void)snprintf(q, sizeof(q), "Does "
					    "%s show %06d? [yes/no] ", addr,
					    passkey);
				else if (method == bt_pair_passkey_entry_e)
					(void)snprintf(q, sizeof(q), "Passkey "
					    "shown on %s: ", addr);
				else if (method == bt_pair_pin_e)
					(void)snprintf(q, sizeof(q), "PIN for "
					    "%s: ", addr);
				else
					(void)snprintf(q, sizeof(q), "Pair "
					    "with %s? [yes/no] ", addr);
				if (!ask(q, line, sizeof(line)))
					(void)strlcpy(line, "no", sizeof(line));
				rc = answer(svc, addr, method, line);
				(void)memset(line, 0, sizeof(line));
				if (rc != EXIT_OK && rc != EXIT_USAGE)
					return (rc);
			}
		}
		nap(250);
	}
	(void)fprintf(stderr, "%s: pair: %s\n", progname,
	    status_str(bt_status_pairing_timeout_e));
	return (EXIT_ERR);
}

static int
cmd_confirm(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	bt_pair_state_t state;
	bt_pair_method_t method;
	bt_status_t st, result;
	int passkey, expires;

	if (argc != 3)
		return (-1);
	(void)memset(addr, 0, sizeof(addr));
	(void)strlcpy(addr, argv[1], sizeof(addr));
	if (bt_pair_status(svc, addr, &state, &method, &passkey, &expires,
	    &result, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("confirm", st));
	if (state != bt_pair_state_waiting_e)
		return (fail("confirm", bt_status_no_prompt_e));
	return (answer(svc, addr, method, argv[2]));
}

static int
cmd_unpair(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	bt_status_t st;

	if (argc != 2)
		return (-1);
	(void)memset(addr, 0, sizeof(addr));
	(void)strlcpy(addr, argv[1], sizeof(addr));
	if (bt_unpair(svc, addr, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("unpair", st));
	return (EXIT_OK);
}

/*
 * Browse a remote device's SDP records (replaces sdpcontrol browse). The
 * daemon connects, reads and renders; the CLI fetches the lines one by one.
 */
static int
cmd_sdp(mach_port_t svc, int argc, char **argv)
{
	BTAddressString address;
	bt_status_t st;
	int records, lines, error, i;

	if (argc != 2) {
		(void)fprintf(stderr, "usage: %s sdp <address>\n", progname);
		return (EXIT_USAGE);
	}
	(void)memset(address, 0, sizeof(address));
	(void)strlcpy(address, argv[1], sizeof(address));

	if (bt_sdp_browse(svc, address, &records, &lines, &error, &st) !=
	    KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e) {
		if (error != 0)
			(void)fprintf(stderr, "%s: sdp %s: %s (%s)\n",
			    progname, argv[1], status_str(st),
			    strerror(error));
		else
			(void)fprintf(stderr, "%s: sdp %s: %s\n", progname,
			    argv[1], status_str(st));
		return (EXIT_ERR);
	}
	if (records == 0) {
		(void)printf("no service records\n");
		return (EXIT_OK);
	}
	for (i = 0; i < lines; i++) {
		BTSDPLine line;

		if (bt_sdp_browse_line(svc, i, line, &st) != KERN_SUCCESS ||
		    st != bt_status_success_e)
			break;
		line[sizeof(line) - 1] = '\0';
		(void)printf("%s\n", line);
	}
	return (EXIT_OK);
}

/*
 * The verbs. `args` is the synopsis after the verb, for usage(); a later
 * ticket adds a row here and its cmd_ function above.
 */
static const struct verb {
	const char	*name;
	const char	*args;
	int		(*fn)(mach_port_t svc, int argc, char **argv);
} verbs[] = {
	{ "status",	"",				cmd_status },
	{ "list",	"",				cmd_list },
	{ "power",	"on|off [node]",		cmd_power },
	{ "discoverable", "on|off [-t seconds] [node]",	cmd_discoverable },
	{ "scan",	"[-t seconds] [node]",		cmd_scan },
	{ "pair",	"address [pin]",		cmd_pair },
	{ "confirm",	"address yes|no|passkey|pin",	cmd_confirm },
	{ "unpair",	"address",			cmd_unpair },
	{ "sdp",	"<address>",			cmd_sdp },
};

#define	NVERBS	(sizeof(verbs) / sizeof(verbs[0]))

static void
usage(void)
{
	size_t i;

	for (i = 0; i < NVERBS; i++)
		(void)fprintf(stderr, "%s %s %s%s%s\n",
		    i == 0 ? "usage:" : "      ", progname, verbs[i].name,
		    verbs[i].args[0] != '\0' ? " " : "", verbs[i].args);
	exit(EXIT_USAGE);
}

int
main(int argc, char **argv)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	size_t i;

	if (argv[0] != NULL) {
		const char *slash = strrchr(argv[0], '/');

		progname = (slash != NULL) ? slash + 1 : argv[0];
	}
	if (argc < 2)
		usage();

	for (i = 0; i < NVERBS; i++) {
		if (strcmp(argv[1], verbs[i].name) == 0)
			break;
	}
	if (i == NVERBS)
		usage();

	kr = bootstrap_look_up(bootstrap_port, BT_SERVICE_NAME, &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		(void)fprintf(stderr,
		    "%s: cannot reach %s — is bluetoothd running?\n",
		    progname, BT_SERVICE_NAME);
		return (EXIT_ERR);
	}

	/* The verb sees its own arguments: argv[0] is the verb. */
	{
		int rc = verbs[i].fn(svc, argc - 1, argv + 1);

		if (rc < 0)
			usage();	/* a verb rejected its arguments */
		return (rc);
	}
}
