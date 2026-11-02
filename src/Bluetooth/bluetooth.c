/*
 * bluetooth — the Bluetooth command-line client. /usr/sbin/bluetooth
 *
 *   bluetooth status            daemon, policy and controllers
 *   bluetooth list              paired devices
 *   bluetooth sdp <addr>        a remote device's SDP records
 *
 * A pure-C MIG client for org.nextbsd.bluetooth, modelled on wlan(8):
 * bootstrap_look_up, then straight RPCs. It links only liblaunch and
 * libsystem_kernel, no CoreFoundation and no SystemConfiguration, so it is
 * also the bridge across the Mach line for anything above it: a Gershwin pane
 * runs it the way Network.prefPane runs its helper (see src/WLAN/wlan.c for
 * why no Gershwin process speaks Mach).
 *
 * The verbs live in one table. A later verb is a row and a function:
 * scan (#208), pair / confirm / unpair (#209), connect / disconnect (#213),
 * power / discoverable (#207), and the diagnostics info / ping / browse
 * (#211). Usage text is generated from the table.
 *
 * Exit status: 0 ok; 1 error or RPC failure; 2 usage; 3 an answer is needed
 * (pair --no-prompt, #211).
 */
#include "bluetooth_mig_types.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "bluetooth.h"			/* MIG user stubs */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	{ "status",	"",	cmd_status },
	{ "list",	"",	cmd_list },
	{ "sdp",	"<address>",	cmd_sdp },
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
	return (verbs[i].fn(svc, argc - 1, argv + 1));
}
