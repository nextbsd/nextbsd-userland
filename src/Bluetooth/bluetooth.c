/*
 * bluetooth — the Bluetooth command-line client. /usr/sbin/bluetooth
 *
 *   bluetooth status                        daemon, policy, controllers, links
 *   bluetooth list                          paired devices
 *   bluetooth power on|off [node]           power (persisted)
 *   bluetooth discoverable on|off [-t s] [node]
 *   bluetooth scan [-t seconds] [node]      discover nearby devices
 *   bluetooth pair <device> [pin] [--no-prompt]
 *   bluetooth confirm <device> yes|no|<passkey or PIN>
 *   bluetooth unpair <device>               forget a device
 *   bluetooth connect <device>              connect, and follow it
 *   bluetooth disconnect <device>           disconnect
 *   bluetooth device <device>               one device's state
 *   bluetooth autoconnect <device> on|off   reconnect at power-up or not
 *   bluetooth audio                         audio policy and devices
 *   bluetooth audio <device> on|off         play to a headset or not
 *   bluetooth audio <device> volume <0-127>
 *   bluetooth audio <device> hfp [cvsd]     open its microphone (hands-free)
 *   bluetooth audio <device> a2dp           close it: back to stereo
 *   bluetooth audio autoswitch on|off       a headset takes /dev/dsp
 *   bluetooth audio sink on|off             be a Bluetooth speaker
 *   bluetooth info [node]                   controller version, buffers, links
 *   bluetooth hci [-n node] ogf ocf [byte ...]   one raw HCI command (root)
 *   bluetooth ping [-c n] [-s size] [-i s] <device>   L2CAP echo
 *   bluetooth sdp <device>                  a remote device's SDP records
 *   bluetooth browse <device>               the same
 *
 * <device> is an address (AA:BB:CC:DD:EE:FF, "/random" for an LE random
 * address) or the start of a device's name, unique among the paired devices,
 * or failing that among the last scan's: `bluetooth pair "MX Keys"`.
 *
 * A pure-C MIG client for org.nextbsd.bluetooth, modelled on wlan(8):
 * bootstrap_look_up, then straight RPCs. It links only liblaunch and
 * libsystem_kernel, no CoreFoundation and no SystemConfiguration, so it is
 * also the bridge across the Mach line for anything above it: a Gershwin pane
 * runs it the way Network.prefPane runs its helper (see src/WLAN/wlan.c for
 * why no Gershwin process speaks Mach). That is what `pair --no-prompt` and
 * exit status 3 are for.
 *
 * It is also the only Bluetooth diagnostic tool: info, hci and ping cover
 * what hccontrol and l2ping gave, and sdp what sdpcontrol browse gave. All of
 * them run inside bluetoothd and come back over MIG.
 *
 * The verbs live in one table. A later verb is a row and a function; usage
 * text is generated from the table.
 *
 * Exit status: 0 ok; 1 error or RPC failure; 2 usage; 3 an answer is needed
 * (pair without a terminal, or pair --no-prompt).
 */
#include "bluetooth_mig_types.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "bluetooth.h"			/* MIG user stubs */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define	BT_SERVICE_NAME		"org.nextbsd.bluetooth"

#define	EXIT_OK			0
#define	EXIT_ERR		1
#define	EXIT_USAGE		2
#define	EXIT_ANSWER		3

static const char *progname = "bluetooth";

static int	print_audio(mach_port_t svc, bool header);
/* Set by SIGINT while pair or ping runs, so they can finish cleanly. */
static volatile sig_atomic_t interrupted;

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

/*
 * An LE device's GAP Appearance (Assigned Numbers §2.6): the category is the
 * top ten bits, the subcategory the low six.
 */
static const char *
appearance_str(int appearance)
{
	int cat = (appearance >> 6) & 0x3ff, sub = appearance & 0x3f;

	switch (cat) {
	case 0x001:	return ("phone");
	case 0x002:	return ("computer");
	case 0x003:	return ("watch");
	case 0x00f:
		switch (sub) {
		case 0x01:	return ("keyboard");
		case 0x02:	return ("mouse");
		case 0x03:	return ("joystick");
		case 0x04:	return ("gamepad");
		case 0x05:	return ("tablet");
		default:	return ("peripheral");
		}
	case 0x021:	return ("speaker");
	case 0x025:	return ("headphones");
	default:	return ("-");
	}
}

/* What a device is: its Class of Device, or for LE its Appearance. */
static const char *
kind_str(int cod, int appearance)
{
	if ((cod & 0xffffff) == 0 && appearance != 0)
		return (appearance_str(appearance));
	return (class_str(cod));
}

/* HCI and LMP version numbers (Assigned Numbers §2.1) as Core versions. */
static const char *
version_str(int v)
{
	static const char *const t[] = {
		"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1",
		"4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0"
	};

	return (v >= 0 && v < (int)(sizeof(t) / sizeof(t[0])) ? t[v] : "?");
}

/* The company identifiers of the controllers NextBSD is likely to meet. */
static const char *
manufacturer_str(int m)
{
	switch (m) {
	case 2:		return ("Intel");
	case 10:	return ("Qualcomm (CSR)");
	case 13:	return ("Texas Instruments");
	case 15:	return ("Broadcom");
	case 29:	return ("Qualcomm");
	case 69:	return ("Atheros");
	case 70:	return ("MediaTek");
	case 93:	return ("Realtek");
	case 305:	return ("Cypress");
	default:	return ("");
	}
}

/* HCI error codes (Core v5.4, Vol 1, Part F, §1.3). */
static const char *
hci_status_str(int code)
{
	static const char *const t[] = {
		"success",
		"unknown HCI command",
		"unknown connection identifier",
		"hardware failure",
		"page timeout",
		"authentication failure",
		"PIN or key missing",
		"memory capacity exceeded",
		"connection timeout",
		"connection limit exceeded",
		"synchronous connection limit exceeded",
		"connection already exists",
		"command disallowed",
		"rejected: limited resources",
		"rejected: security reasons",
		"rejected: unacceptable address",
		"connection accept timeout",
		"unsupported feature or parameter value",
		"invalid HCI command parameters",
		"remote user terminated connection",
		"remote device terminated: low resources",
		"remote device terminated: power off",
		"connection terminated by local host",
		"repeated attempts",
		"pairing not allowed",
		"unknown LMP PDU",
		"unsupported remote feature",
		"SCO offset rejected",
		"SCO interval rejected",
		"SCO air mode rejected",
		"invalid LMP / LL parameters",
		"unspecified error",
		"unsupported LMP / LL parameter value",
		"role change not allowed",
		"LMP / LL response timeout",
		"LMP error transaction collision",
		"LMP PDU not allowed",
		"encryption mode not acceptable",
		"link key cannot be changed",
		"requested QoS not supported",
		"instant passed",
		"pairing with unit key not supported",
		"different transaction collision",
		"reserved (0x2b)",
		"QoS unacceptable parameter",
		"QoS rejected",
		"channel classification not supported",
		"insufficient security",
		"parameter out of mandatory range",
		"reserved (0x31)",
		"role switch pending",
		"reserved (0x33)",
		"reserved slot violation",
		"role switch failed",
		"extended inquiry response too large",
		"secure simple pairing not supported by host",
		"host busy: pairing",
		"no suitable channel found",
		"controller busy",
		"unacceptable connection parameters",
		"advertising timeout",
		"connection terminated: MIC failure",
		"connection failed to be established",
		"reserved (0x3f)",
		"coarse clock adjustment rejected",
		"type0 submap not defined",
		"unknown advertising identifier",
		"limit reached",
		"operation cancelled by host",
		"packet too long"
	};

	return (code >= 0 && code < (int)(sizeof(t) / sizeof(t[0])) ?
	    t[code] : "unknown error");
}

/* ng_hci's link types (NG_HCI_LINK_*). */
static const char *
link_type_str(int t)
{
	switch (t) {
	case 0:		return ("SCO");
	case 1:		return ("ACL");
	case 2:
	case 3:		return ("LE");
	default:	return ("?");
	}
}

static const char *
link_mode_str(int m)
{
	switch (m) {
	case 0:		return ("active");
	case 1:		return ("hold");
	case 2:		return ("sniff");
	case 3:		return ("park");
	default:	return ("?");
	}
}

/* A pairing method as `pair --no-prompt` prints it: method=<this>. */
static const char *
method_name(bt_pair_method_t m)
{
	switch (m) {
	case bt_pair_just_works_e:		return ("just-works");
	case bt_pair_numeric_comparison_e:	return ("numeric-comparison");
	case bt_pair_passkey_entry_e:		return ("passkey-entry");
	case bt_pair_passkey_display_e:		return ("passkey-display");
	case bt_pair_pin_e:			return ("pin");
	default:				return ("none");
	}
}

static void
on_sigint(int sig)
{
	(void)sig;
	interrupted = 1;
}

/*
 * Catch the first Ctrl-C, so pair can say no to the device and ping can print
 * its summary; the handler resets itself, so a second one kills as usual. Not
 * SA_RESTART: a read from the terminal returns at once.
 */
static void
catch_sigint(void)
{
	struct sigaction sa;

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint;
	sa.sa_flags = SA_RESETHAND;
	(void)sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGINT, &sa, NULL);
}

/* "AA:BB:CC:DD:EE:FF" or "aa-bb-cc-dd-ee-ff", with any suffix after it. */
static bool
looks_like_address(const char *s)
{
	int i;

	for (i = 0; i < 17; i++) {
		if (s[i] == '\0')
			return (false);
		if (i % 3 == 2) {
			if (s[i] != ':' && s[i] != '-')
				return (false);
		} else if (!isxdigit((unsigned char)s[i]))
			return (false);
	}
	return (s[17] == '\0' || s[17] == '/' || s[17] == '-');
}

/* Case-insensitive: does `name` start with `prefix`? */
static bool
name_matches(const char *name, const char *prefix)
{
	return (name[0] != '\0' && strncasecmp(name, prefix,
	    strlen(prefix)) == 0);
}

/*
 * One candidate list for resolve(): an exact name wins over prefixes, and more
 * than one prefix match is ambiguous.
 */
struct match {
	int			n, exact;
	BTAddressString		addr, exact_addr;
	char			names[256];
};

static void
match_add(struct match *m, const char *want, const char *addr,
    const char *name)
{
	if (!name_matches(name, want))
		return;
	if (strcasecmp(name, want) == 0) {
		m->exact++;
		(void)strlcpy(m->exact_addr, addr, sizeof(m->exact_addr));
	}
	m->n++;
	(void)strlcpy(m->addr, addr, sizeof(m->addr));
	if (m->names[0] != '\0')
		(void)strlcat(m->names, ", ", sizeof(m->names));
	(void)strlcat(m->names, name, sizeof(m->names));
}

/* 1: one device picked into `out`; 0: none; -1: ambiguous (reported). */
static int
match_pick(const struct match *m, const char *want, BTAddressString out)
{
	if (m->exact == 1) {
		(void)strlcpy(out, m->exact_addr, sizeof(BTAddressString));
		return (1);
	}
	if (m->n == 1) {
		(void)strlcpy(out, m->addr, sizeof(BTAddressString));
		return (1);
	}
	if (m->n > 1) {
		(void)fprintf(stderr, "%s: \"%s\" matches more than one device: "
		    "%s\n", progname, want, m->names);
		return (-1);
	}
	return (0);
}

/*
 * A device argument to an address: an address as given (the daemon checks
 * it), else the paired device whose name starts with it, else the device from
 * the last scan whose name does. 0 on success, else an exit status.
 */
static int
resolve(mach_port_t svc, const char *arg, BTAddressString out)
{
	struct match m;
	bt_status_t st;
	int count, scanning, i, r;

	(void)memset(out, 0, sizeof(BTAddressString));
	if (looks_like_address(arg)) {
		(void)strlcpy(out, arg, sizeof(BTAddressString));
		return (0);
	}
	if (arg[0] == '\0')
		return (EXIT_USAGE);

	(void)memset(&m, 0, sizeof(m));
	if (bt_paired_count(svc, &count, &st) == KERN_SUCCESS &&
	    st == bt_status_success_e) {
		for (i = 0; i < count; i++) {
			BTAddressString address;
			BTDeviceName name;
			BTProfileList profiles;
			bt_device_state_t dst;
			int cod, transports, autoc;

			if (bt_paired_entry(svc, i, address, name, &cod,
			    &transports, profiles, &autoc, &dst, &st) ==
			    KERN_SUCCESS && st == bt_status_success_e)
				match_add(&m, arg, address, name);
		}
	}
	if ((r = match_pick(&m, arg, out)) != 0)
		return (r > 0 ? 0 : EXIT_ERR);

	(void)memset(&m, 0, sizeof(m));
	{
		BTNodeName node;

		(void)memset(node, 0, sizeof(node));
		if (bt_scan_count(svc, node, &count, &scanning, &st) ==
		    KERN_SUCCESS && st == bt_status_success_e) {
			for (i = 0; i < count; i++) {
				BTAddressString address;
				BTDeviceName name;
				int cod, rssi, tr, app, paired, age;

				if (bt_scan_entry(svc, node, i, address, name,
				    &cod, &rssi, &tr, &app, &paired, &age,
				    &st) == KERN_SUCCESS &&
				    st == bt_status_success_e)
					match_add(&m, arg, address, name);
			}
		}
	}
	if ((r = match_pick(&m, arg, out)) != 0)
		return (r > 0 ? 0 : EXIT_ERR);

	(void)fprintf(stderr, "%s: no device called \"%s\"; give its address, "
	    "or run \"%s scan\" first\n", progname, arg, progname);
	return (EXIT_ERR);
}

/* Parse a hexadecimal number, with or without 0x, up to `max`. */
static bool
parse_hex(const char *s, unsigned long max, unsigned long *out)
{
	char *end;

	errno = 0;
	*out = strtoul(s, &end, 16);
	return (s[0] != '\0' && *end == '\0' && errno == 0 && *out <= max);
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
	(void)print_audio(svc, false);

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

	/*
	 * The devices connected now, with what runs over each link: the HID
	 * host, HID over GATT, the audio roles.
	 */
	for (i = 0; i < npaired; i++) {
		BTAddressString address;
		BTDeviceName name;
		BTProfileList profiles;
		bt_device_state_t dst;
		bt_status_t st;
		int cod, transports, ac;

		if (bt_paired_entry(svc, i, address, name, &cod, &transports,
		    profiles, &ac, &dst, &st) != KERN_SUCCESS ||
		    st != bt_status_success_e || dst != bt_device_connected_e)
			continue;
		(void)printf("connected:     %s %s (%s)\n",
		    name[0] != '\0' ? name : "-", address,
		    profiles[0] != '\0' ? profiles : transports_str(transports));
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
	(void)printf("%-24s %-28s %-15s %-6s %-12s %s\n", "ADDRESS", "NAME",
	    "CLASS", "LINK", "SIGNAL", "PAIRED");
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
		(void)printf("%-24s %-28.28s %-15s %-6s %-12s %s\n", address,
		    name[0] != '\0' ? name : "-", kind_str(cod, appearance),
		    transports_str(transports), sig, paired ? "yes" : "no");
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

/*
 * The machine-readable line `pair --no-prompt` prints before exiting 3, for a
 * pane to show and then answer with `bluetooth confirm`.
 */
static void
show_needed(const char *address, bt_pair_method_t m, int passkey, int expires)
{
	if (m == bt_pair_numeric_comparison_e || m == bt_pair_passkey_display_e)
		(void)printf("address=%s method=%s passkey=%06d expires=%d\n",
		    address, method_name(m), passkey, expires);
	else
		(void)printf("address=%s method=%s passkey= expires=%d\n",
		    address, method_name(m), expires);
}

static int
cmd_pair(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	BTPinString pin;
	bt_pair_state_t state;
	bt_pair_method_t method;
	bt_status_t st, result;
	int passkey, expires, waited, rc, i, shown_passkey = -1;
	const char *device = NULL, *pinarg = NULL;
	bool no_prompt = false, interactive;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-prompt") == 0)
			no_prompt = true;
		else if (device == NULL)
			device = argv[i];
		else if (pinarg == NULL)
			pinarg = argv[i];
		else
			return (-1);
	}
	if (device == NULL)
		return (-1);
	interactive = !no_prompt && isatty(STDIN_FILENO) != 0;
	if ((rc = resolve(svc, device, addr)) != 0)
		return (rc);
	(void)memset(pin, 0, sizeof(pin));
	if (pinarg != NULL)
		(void)strlcpy(pin, pinarg, sizeof(pin));

	if (bt_pair(svc, addr, pin, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	(void)memset(pin, 0, sizeof(pin));
	if (st != bt_status_success_e)
		return (fail("pair", st));
	if (!no_prompt)
		(void)fprintf(stderr, "pairing with %s...\n", addr);

	/*
	 * Follow the pairing; answer prompts on the terminal if there is one.
	 * Ctrl-C says no to the device rather than leaving it waiting.
	 */
	catch_sigint();
	for (waited = 0; waited < 150 * 1000; waited += 250) {
		if (interrupted) {
			BTPinString none;
			bt_status_t cst;

			(void)memset(none, 0, sizeof(none));
			(void)bt_pair_confirm(svc, addr, 0, 0, none, &cst);
			(void)fprintf(stderr, "\n%s: pair: cancelled\n",
			    progname);
			return (EXIT_ERR);
		}
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

			if (method == bt_pair_just_works_e) {
				/*
				 * Nothing to compare: running `pair` was the
				 * yes. A mouse, a keyboard without a display
				 * step, most headphones.
				 */
				rc = answer(svc, addr, method, "yes");
				if (rc != EXIT_OK)
					return (rc);
			} else if (no_prompt) {
				show_needed(addr, method, passkey, expires);
				return (EXIT_ANSWER);
			} else if (method == bt_pair_passkey_display_e) {
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

				if (method == bt_pair_numeric_comparison_e)
					(void)snprintf(q, sizeof(q), "Does "
					    "%s show %06d? [yes/no] ", addr,
					    passkey);
				else if (method == bt_pair_passkey_entry_e)
					(void)snprintf(q, sizeof(q), "Passkey "
					    "shown on %s: ", addr);
				else
					(void)snprintf(q, sizeof(q), "PIN for "
					    "%s: ", addr);
				if (!ask(q, line, sizeof(line))) {
					if (interrupted)
						continue;  /* cancelled above */
					(void)strlcpy(line, "no", sizeof(line));
				}
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
	int passkey, expires, rc;

	if (argc != 3)
		return (-1);
	if ((rc = resolve(svc, argv[1], addr)) != 0)
		return (rc);
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
	int rc;

	if (argc != 2)
		return (-1);
	if ((rc = resolve(svc, argv[1], addr)) != 0)
		return (rc);
	if (bt_unpair(svc, addr, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("unpair", st));
	return (EXIT_OK);
}

#define	CONNECT_WAIT_S		30

/*
 * Connect, then follow the device until it is connected or the attempt has
 * ended: the daemon answers as soon as it has started paging.
 */
static int
cmd_connect(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr, a2;
	BTDeviceName name;
	BTNodeName node;
	BTProfileList profiles;
	bt_device_state_t state = bt_device_disconnected_e;
	bt_status_t st;
	int cod, rssi, paired, waited, rc;

	if (argc != 2)
		return (-1);
	if ((rc = resolve(svc, argv[1], addr)) != 0)
		return (rc);
	if (bt_connect(svc, addr, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("connect", st));
	for (waited = 0; waited < CONNECT_WAIT_S * 1000; waited += 250) {
		(void)memcpy(a2, addr, sizeof(a2));
		if (bt_device_status(svc, a2, name, &cod, &state, node, &rssi,
		    profiles, &paired, &st) != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
		if (st == bt_status_success_e &&
		    state == bt_device_connected_e) {
			if (profiles[0] != '\0')
				(void)printf("connected to %s (%s)\n", addr,
				    profiles);
			else
				(void)printf("connected to %s\n", addr);
			return (EXIT_OK);
		}
		if (waited >= 1000 && (st != bt_status_success_e ||
		    state == bt_device_disconnected_e))
			break;
		nap(250);
	}
	(void)fprintf(stderr, "%s: could not connect to %s\n", progname,
	    addr);
	return (EXIT_ERR);
}

static int
cmd_disconnect(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	bt_status_t st;
	int rc;

	if (argc != 2)
		return (-1);
	if ((rc = resolve(svc, argv[1], addr)) != 0)
		return (rc);
	if (bt_disconnect(svc, addr, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("disconnect", st));
	return (EXIT_OK);
}

static int
cmd_device(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	BTDeviceName name;
	BTNodeName node;
	BTProfileList profiles;
	bt_device_state_t state;
	bt_status_t st;
	int cod, rssi, paired, rc;

	if (argc != 2)
		return (-1);
	if ((rc = resolve(svc, argv[1], addr)) != 0)
		return (rc);
	if (bt_device_status(svc, addr, name, &cod, &state, node, &rssi,
	    profiles, &paired, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("device", st));
	(void)printf("%s\n", addr);
	(void)printf("  name:        %s\n", name[0] != '\0' ? name : "-");
	(void)printf("  class:       %06x %s\n", cod & 0xffffff,
	    class_str(cod));
	(void)printf("  paired:      %s\n", paired ? "yes" : "no");
	(void)printf("  state:       %s\n", device_state_str(state));
	if (node[0] != '\0')
		(void)printf("  controller:  %s\n", node);
	if (rssi != 127)
		(void)printf("  signal:      %s %d dBm\n", signal_bar(rssi),
		    rssi);
	if (profiles[0] != '\0')
		(void)printf("  profiles:    %s\n", profiles);
	return (EXIT_OK);
}

static int
cmd_autoconnect(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	bt_status_t st;
	int on, rc;

	if (argc != 3 || (on = parse_onoff(argv[2])) < 0)
		return (-1);
	if ((rc = resolve(svc, argv[1], addr)) != 0)
		return (rc);
	if (bt_device_autoconnect(svc, addr, on, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("autoconnect", st));
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
	int records, lines, error, i, rc;

	if (argc != 2)
		return (-1);
	if ((rc = resolve(svc, argv[1], address)) != 0)
		return (rc);

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

/* A headset's hands-free line, under its audio line; nothing without one. */
static void
print_hfp(mach_port_t svc, BTAddressString addr, const char *node)
{
	BTTransportName codec;
	BTNodeName device;
	bt_status_t st;
	int conn, audio, pending, feat, vgs, vgm, batt;

	if (bt_hfp_status(svc, addr, &conn, &audio, &pending, codec, device,
	    &feat, &vgs, &vgm, &batt, &st) != KERN_SUCCESS ||
	    st != bt_status_success_e)
		return;
	(void)printf("      hands-free %s", !conn ? "connecting" :
	    audio ? "microphone on" : pending ? "opening the microphone" :
	    "connected, microphone off");
	if (audio)
		(void)printf(" (%s, record from /dev/%s)", codec,
		    node[0] != '\0' ? node : "dsp");
	if (batt >= 0)
		(void)printf(", battery %d%%", batt);
	(void)printf("\n");
	(void)device;
	(void)feat;
	(void)vgs;
	(void)vgm;
}

/* The audio devices and policy, as `status` and `audio` print them. */
static int
print_audio(mach_port_t svc, bool header)
{
	bt_status_t st;
	int autosw, sink, count, i;

	if (bt_audio_policy(svc, -1, -1, &autosw, &sink, &st) !=
	    KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (header)
		(void)printf("\n");
	(void)printf("audio:         headset takes /dev/dsp %s, "
	    "speaker (A2DP sink) %s\n", autosw ? "on" : "off",
	    sink ? "on" : "off");
	if (bt_audio_count(svc, &count, &st) != KERN_SUCCESS ||
	    st != bt_status_success_e)
		return (EXIT_OK);
	for (i = 0; i < count; i++) {
		BTAddressString addr;
		BTNodeName node;
		BTTransportName codec;
		int role, route, active, avrcp, defout, volume;

		if (bt_audio_entry(svc, i, addr, &role, &route, &active,
		    &avrcp, &defout, &volume, node, codec, &st) !=
		    KERN_SUCCESS || st != bt_status_success_e)
			break;
		(void)printf("  %s  %s", addr, role == 1 ? "playing to it" :
		    role == 2 ? "playing to us" : "remote control only");
		if (role == 1)
			(void)printf(" via /dev/%s%s", node,
			    defout ? " (and /dev/dsp)" : "");
		if (route == 2)
			(void)printf(", on HFP");
		if (codec[0] != '\0')
			(void)printf(", %s", codec);
		if (avrcp)
			(void)printf(", AVRCP");
		if (volume >= 0)
			(void)printf(", volume %d/127", volume);
		(void)printf("\n");
		print_hfp(svc, addr, node);
	}
	return (EXIT_OK);
}

static int
cmd_audio(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	bt_status_t st;
	int on, a = -1, b = -1, x, y, rc;
	char *end;
	long v;

	if (argc == 1)
		return (print_audio(svc, false));
	if (argc == 3 && (strcmp(argv[1], "autoswitch") == 0 ||
	    strcmp(argv[1], "sink") == 0)) {
		if ((on = parse_onoff(argv[2])) < 0)
			return (-1);
		if (strcmp(argv[1], "autoswitch") == 0)
			a = on;
		else
			b = on;
		if (bt_audio_policy(svc, a, b, &x, &y, &st) != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
		return (st == bt_status_success_e ? EXIT_OK :
		    fail("audio", st));
	}
	if (argc < 3)
		return (-1);
	if ((rc = resolve(svc, argv[1], addr)) != 0)
		return (rc);
	if (argc == 3 && (on = parse_onoff(argv[2])) >= 0) {
		if (bt_audio_set(svc, addr, on, &st) != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
	} else if ((argc == 3 || argc == 4) && (strcmp(argv[2], "hfp") == 0 ||
	    strcmp(argv[2], "a2dp") == 0)) {
		if (argc == 4 && (strcmp(argv[2], "hfp") != 0 ||
		    strcmp(argv[3], "cvsd") != 0))
			return (-1);
		/* 1 A2DP, 2 HFP, 3 HFP held to CVSD (bluetooth.defs 31085) */
		if (bt_audio_route(svc, addr, strcmp(argv[2], "a2dp") == 0 ? 1 :
		    argc == 4 ? 3 : 2, &st) != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
	} else if (argc == 4 && strcmp(argv[2], "volume") == 0) {
		v = strtol(argv[3], &end, 10);
		if (*argv[3] == '\0' || *end != '\0' || v < 0 || v > 127)
			return (-1);
		if (bt_audio_volume(svc, addr, (int)v, &st) != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
	} else
		return (-1);
	return (st == bt_status_success_e ? EXIT_OK : fail("audio", st));
}

/*
 * What hccontrol's read_local_version_information, read_buffer_size,
 * read_local_supported_features, read_node_stat and read_connection_list gave,
 * for one controller.
 */
static int
cmd_info(mach_port_t svc, int argc, char **argv)
{
	BTNodeName node, node_out;
	BTAddressString address;
	BTFeatures features;
	bt_status_t st;
	int hv, hr, lv, ls, mf, acl_mtu, acl_pkts, sco_mtu, sco_pkts;
	int le_mtu, le_pkts, cmd_sent, events, acl_recv, acl_sent, sco_recv;
	int sco_sent, bytes_recv, bytes_sent, cmd_free, acl_free, sco_free;
	int count, i;
	const uint8_t *f = (const uint8_t *)features;

	if (argc > 2)
		return (-1);
	(void)memset(node, 0, sizeof(node));
	if (argc > 1)
		(void)strlcpy(node, argv[1], sizeof(node));

	if (bt_adapter_details(svc, node, node_out, address, &hv, &hr, &lv,
	    &ls, &mf, features, &acl_mtu, &acl_pkts, &sco_mtu, &sco_pkts,
	    &le_mtu, &le_pkts, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e)
		return (fail("info", st));

	(void)printf("controller:    %s\n", node_out);
	if (address[0] != '\0')
		(void)printf("address:       %s\n", address);
	(void)printf("manufacturer:  %s%s%d\n", manufacturer_str(mf),
	    manufacturer_str(mf)[0] != '\0' ? " " : "", mf);
	(void)printf("HCI version:   %s (0x%02x) revision 0x%04x\n",
	    version_str(hv), hv, hr);
	(void)printf("LMP version:   %s (0x%02x) subversion 0x%04x\n",
	    version_str(lv), lv, ls);
	(void)printf("features:      %02x %02x %02x %02x %02x %02x %02x %02x"
	    " (%s%s%s)\n", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
	    (f[6] & 0x08) != 0 ? "SSP" : "legacy pairing",
	    (f[4] & 0x40) != 0 ? ", LE" : "",
	    (f[6] & 0x01) != 0 ? ", EIR" : "");
	(void)printf("ACL buffers:   %d x %d bytes\n", acl_pkts, acl_mtu);
	(void)printf("SCO buffers:   %d x %d bytes\n", sco_pkts, sco_mtu);
	if (le_mtu != 0)
		(void)printf("LE buffers:    %d x %d bytes\n", le_pkts, le_mtu);
	else if ((f[4] & 0x40) != 0)
		(void)printf("LE buffers:    shared with ACL\n");

	if (bt_adapter_stats(svc, node, &cmd_sent, &events, &acl_recv,
	    &acl_sent, &sco_recv, &sco_sent, &bytes_recv, &bytes_sent,
	    &cmd_free, &acl_free, &sco_free, &st) == KERN_SUCCESS &&
	    st == bt_status_success_e) {
		(void)printf("free credits:  %d command, %d ACL, %d SCO\n",
		    cmd_free, acl_free, sco_free);
		(void)printf("commands:      %u sent, %u events received\n",
		    (unsigned)cmd_sent, (unsigned)events);
		(void)printf("ACL packets:   %u in, %u out\n",
		    (unsigned)acl_recv, (unsigned)acl_sent);
		(void)printf("SCO packets:   %u in, %u out\n",
		    (unsigned)sco_recv, (unsigned)sco_sent);
		(void)printf("bytes:         %u in, %u out\n",
		    (unsigned)bytes_recv, (unsigned)bytes_sent);
	}

	if (bt_link_count(svc, node, &count, &st) != KERN_SUCCESS ||
	    st != bt_status_success_e)
		return (EXIT_OK);
	if (count == 0) {
		(void)printf("links:         none\n");
		return (EXIT_OK);
	}
	(void)printf("\n%-24s %-4s %-6s %-10s %-9s %-6s %s\n", "ADDRESS",
	    "LINK", "HANDLE", "ROLE", "ENCRYPTED", "MODE", "PENDING");
	for (i = 0; i < count; i++) {
		BTAddressString la;
		int type, handle, role, enc, mode, pending;

		if (bt_link_entry(svc, node, i, la, &type, &handle, &role,
		    &enc, &mode, &pending, &st) != KERN_SUCCESS ||
		    st != bt_status_success_e)
			break;		/* the list shrank under us */
		(void)printf("%-24s %-4s 0x%04x %-10s %-9s %-6s %d\n", la,
		    link_type_str(type), handle,
		    role == 0 ? "central" : "peripheral",
		    enc != 0 ? "yes" : "no", link_mode_str(mode), pending);
	}
	return (EXIT_OK);
}

/*
 * Send one raw HCI command: what hccontrol's named commands did, by number.
 * Everything is hexadecimal: `bluetooth hci 04 0001` is
 * Read_Local_Version_Information.
 */
static int
cmd_hci(mach_port_t svc, int argc, char **argv)
{
	BTNodeName node;
	BTHCIData params, rparams;
	bt_status_t st;
	unsigned long ogf, ocf, b;
	int i, plen = 0, event, hst, rplen, err;

	(void)memset(node, 0, sizeof(node));
	(void)memset(params, 0, sizeof(params));
	i = 1;
	if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
		(void)strlcpy(node, argv[i + 1], sizeof(node));
		i += 2;
	}
	if (argc - i < 2)
		return (-1);
	if (!parse_hex(argv[i], 0x3f, &ogf) ||
	    !parse_hex(argv[i + 1], 0x3ff, &ocf)) {
		(void)fprintf(stderr, "%s: hci: OGF is 00..3f and OCF 000..3ff, "
		    "in hexadecimal\n", progname);
		return (EXIT_USAGE);
	}
	for (i += 2; i < argc; i++) {
		if (plen >= BT_HCI_DATA_MAX || !parse_hex(argv[i], 0xff, &b)) {
			(void)fprintf(stderr, "%s: hci: parameters are up to "
			    "%d bytes, each 00..ff in hexadecimal\n", progname,
			    BT_HCI_DATA_MAX);
			return (EXIT_USAGE);
		}
		params[plen++] = (char)b;
	}

	if (bt_hci_command(svc, node, (int)((ogf << 10) | ocf), params, plen,
	    &event, &hst, rparams, &rplen, &err, &st) != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: RPC failed\n", progname);
		return (EXIT_ERR);
	}
	if (st != bt_status_success_e) {
		if (err == EPERM)
			(void)fprintf(stderr, "%s: hci: only root can send raw "
			    "HCI commands\n", progname);
		else if (err == ETIMEDOUT)
			(void)fprintf(stderr, "%s: hci: the controller did not "
			    "answer\n", progname);
		else if (err != 0)
			(void)fprintf(stderr, "%s: hci: %s\n", progname,
			    strerror(err));
		else
			return (fail("hci", st));
		return (EXIT_ERR);
	}

	(void)printf("opcode:        0x%04lx (OGF 0x%02lx, OCF 0x%03lx)\n",
	    (ogf << 10) | ocf, ogf, ocf);
	(void)printf("event:         %s\n", event == 0x0e ?
	    "Command Complete" : "Command Status (the command goes on in the "
	    "controller)");
	(void)printf("status:        0x%02x %s\n", hst, hci_status_str(hst));
	if (rplen > 0) {
		(void)printf("return:       ");
		for (i = 0; i < rplen; i++) {
			if (i > 0 && i % 16 == 0)
				(void)printf("\n              ");
			(void)printf(" %02x", (unsigned char)rparams[i]);
		}
		(void)printf("\n");
	}
	return (hst == 0 ? EXIT_OK : EXIT_ERR);
}

/*
 * An Echo Request rides the signalling channel, whose MTU is at least 48
 * bytes on every BR/EDR device and 672 on most; less the 4-byte command
 * header, 44 always fits and 668 usually does.
 */
#define	PING_SIZE_DEFAULT	44
#define	PING_SIZE_MAX		668

/*
 * L2CAP echo, as l2ping did: one Echo Request per interval and its round
 * trip. Pages the device if there is no link yet, so the first one is slow.
 */
static int
cmd_ping(mach_port_t svc, int argc, char **argv)
{
	BTAddressString addr;
	bt_status_t st;
	const char *device = NULL;
	int i, rc, count = 5, size = PING_SIZE_DEFAULT, wait_s = 1;
	int sent = 0, received = 0, rtt, result, err;
	long min_us = -1, max_us = 0, sum_us = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			count = atoi(argv[++i]);
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			size = atoi(argv[++i]);
		else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
			wait_s = atoi(argv[++i]);
		else if (device == NULL)
			device = argv[i];
		else
			return (-1);
	}
	if (device == NULL || count < 0 || size < 0 || size > PING_SIZE_MAX ||
	    wait_s < 0)
		return (-1);
	if ((rc = resolve(svc, device, addr)) != 0)
		return (rc);

	(void)printf("L2CAP echo to %s, %d bytes of data\n", addr, size);
	catch_sigint();
	while (!interrupted && (count == 0 || sent < count)) {
		if (bt_l2cap_ping(svc, addr, size, &rtt, &result, &err,
		    &st) != KERN_SUCCESS) {
			(void)fprintf(stderr, "%s: RPC failed\n", progname);
			return (EXIT_ERR);
		}
		sent++;
		if (st == bt_status_success_e) {
			received++;
			(void)printf("%d bytes from %s seq=%d time=%d.%03d ms\n",
			    size, addr, sent, rtt / 1000, rtt % 1000);
			if (min_us < 0 || rtt < min_us)
				min_us = rtt;
			if (rtt > max_us)
				max_us = rtt;
			sum_us += rtt;
		} else if (st == bt_status_not_connected_e) {
			(void)printf("seq=%d: L2CAP result 0x%04x\n", sent,
			    result);
		} else if (err == ETIMEDOUT || err == EHOSTDOWN) {
			(void)printf("seq=%d: no response\n", sent);
		} else if (err != 0) {
			(void)printf("seq=%d: %s\n", sent, strerror(err));
		} else {
			return (fail("ping", st));	/* LE, powered off */
		}
		if (!interrupted && (count == 0 || sent < count))
			nap(wait_s * 1000);
	}

	(void)printf("--- %s ---\n%d sent, %d received", addr, sent, received);
	if (received > 0)
		(void)printf(", round trip min/avg/max %ld.%03ld/%ld.%03ld/"
		    "%ld.%03ld ms", min_us / 1000, min_us % 1000,
		    (sum_us / received) / 1000, (sum_us / received) % 1000,
		    max_us / 1000, max_us % 1000);
	(void)printf("\n");
	return (received > 0 ? EXIT_OK : EXIT_ERR);
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
	{ "pair",	"device [pin] [--no-prompt]",	cmd_pair },
	{ "confirm",	"device yes|no|passkey|pin",	cmd_confirm },
	{ "unpair",	"device",			cmd_unpair },
	{ "connect",	"device",			cmd_connect },
	{ "disconnect",	"device",			cmd_disconnect },
	{ "device",	"device",			cmd_device },
	{ "autoconnect", "device on|off",		cmd_autoconnect },
	{ "audio",	"[device on|off | device volume 0-127 | "
			"device hfp [cvsd] | device a2dp | "
			"autoswitch on|off | sink on|off]",	cmd_audio },
	{ "info",	"[node]",			cmd_info },
	{ "hci",	"[-n node] ogf ocf [byte ...]",	cmd_hci },
	{ "ping",	"[-c count] [-s size] [-i seconds] device", cmd_ping },
	{ "sdp",	"device",			cmd_sdp },
	{ "browse",	"device",			cmd_sdp },
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
