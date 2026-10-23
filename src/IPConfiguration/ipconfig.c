/*
 * ipconfig — Apple's IPConfiguration CLI, ported to NextBSD.
 *
 * A Mach RPC client of `com.apple.IPConfiguration` mirroring Apple's
 * bootp/ipconfig.tproj/client.c shape — a `commands[]` lookup table
 * with one dispatch function per subcommand:
 *
 *   ipconfig waitall [timeout]		→  SCDynamicStore Plugin:IPConfiguration
 *   ipconfig getifaddr <if>		→  ipconfig_if_addr
 *   ipconfig ifcount			→  ipconfig_if_count
 *   ipconfig getoption <if|""> <opt>	→  ipconfig_get_option
 *   ipconfig getsummary <if>		→  ipconfig_get_summary
 *   ipconfig set <if> <method> [args]	→  ipconfig_set
 *
 * Apple's client.c covers ~25 subcommands (getpacket, getv6packet,
 * getra, getdhcpduid, addService, BSDP, NetBoot, CLAT46, …). The rest
 * grow by appending rows to commands[] + turning the matching `skip;`
 * in ipconfig.defs back into a routine.
 *
 * `set` builds Apple's method dictionary for every method Apple
 * accepts, so the client is complete; which methods ipconfigd can act
 * on is the daemon's call. Today that is DHCP and NONE — MANUAL,
 * INFORM and BOOTP need the static service type (#58), and the rest
 * answer "operation not supported".
 *
 * Install: /usr/sbin/ipconfig.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <sys/time.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include "ipconfig_mig_types.h"
#include "ipconfig.h"		/* MIG: ipconfig_* client stubs */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define METHOD_LIST_V4			"BOOTP, MANUAL, DHCP, INFORM"
#define METHOD_LIST_V4_WITH_NONE	METHOD_LIST_V4 ", NONE"
#define METHOD_LIST_V6			"AUTOMATIC-V6, MANUAL-V6, 6TO4"
#define METHOD_LIST_V6_WITH_NONE	METHOD_LIST_V6 ", NONE-V6"
#define METHOD_LIST_WITH_NONE		METHOD_LIST_V4_WITH_NONE ", " \
					METHOD_LIST_V6_WITH_NONE

/* The SCSchema strings Apple's client takes from SCSchemaDefinitions.h. */
#define	kIPCfgEntIPv4			CFSTR("IPv4")
#define	kIPCfgEntIPv6			CFSTR("IPv6")
#define	kIPCfgConfigMethod		CFSTR("ConfigMethod")
#define	kIPCfgAddresses			CFSTR("Addresses")
#define	kIPCfgSubnetMasks		CFSTR("SubnetMasks")
#define	kIPCfgPrefixLength		CFSTR("PrefixLength")
#define	kIPCfgFailoverAddressTimeout	CFSTR("FailoverAddressTimeout")

#define	STARTUP_KEY			CFSTR("Plugin:IPConfiguration")

#define	WAIT_ALL_DEFAULT_TIMEOUT	90
#define	WAIT_ALL_MAX_TIMEOUT		120

static const char *progname = "ipconfig";
static const char *command_name;

/* Apple's ipconfig_status_string(), bootplib/ipconfig_types.c. */
static const char *
ipconfig_status_string(ipconfig_status_t status)
{
	static const char *str[] = {
		"success",
		"permission denied",
		"interface doesn't exist",
		"invalid parameter",
		"invalid operation",
		"allocation failed",
		"internal error",
		"operation not supported",
		"address in use",
		"no server",
		"server not responding",
		"lease terminated",
		"media inactive",
		"server error",
		"no such service",
		"duplicate service",
		"address timed out",
		"not found",
		"resource unavailable",
		"network changed",
		"lease expired",
		"DHCP waiting"
	};

	if ((unsigned)status >= sizeof(str) / sizeof(str[0]))
		return ("<unknown>");
	return (str[status]);
}

static void
InterfaceNameInit(InterfaceName name, const char *ifname)
{
	(void)memset(name, 0, sizeof(InterfaceName));
	(void)strlcpy(name, ifname, sizeof(InterfaceName));
}

/*
 * waitall — block until ipconfigd has finished configuring the
 * interfaces it found at startup, i.e. until it sets the
 * Plugin:IPConfiguration key, or until the timeout. Exactly Apple's:
 * no server port, and exit 0 either way.
 */
static void
on_alarm(int sig)
{
	(void)sig;
	exit(0);
}

static void
key_appeared(SCDynamicStoreRef session, CFArrayRef changes, void *arg)
{
	(void)session;
	(void)changes;
	(void)arg;
	exit(0);
}

static int
S_wait_all(mach_port_t server, int argc, char **argv)
{
	CFMutableArrayRef keys;
	SCDynamicStoreRef session;
	dispatch_queue_t queue;
	unsigned long t = WAIT_ALL_DEFAULT_TIMEOUT;
	CFPropertyListRef value;
	struct itimerval v;

	(void)server;
	if (argc > 0) {
		t = strtoul(argv[0], 0, 0);
		if (t > WAIT_ALL_MAX_TIMEOUT)
			t = WAIT_ALL_MAX_TIMEOUT;
	}

	session = SCDynamicStoreCreate(NULL, CFSTR("ipconfig command"),
	    key_appeared, NULL);
	if (session == NULL) {
		(void)fprintf(stderr, "SCDynamicStoreCreate failed: %s\n",
		    SCErrorString(SCError()));
		return (0);
	}
	keys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(keys, STARTUP_KEY);
	(void)SCDynamicStoreSetNotificationKeys(session, keys, NULL);
	CFRelease(keys);

	/*
	 * Apple schedules the session on a CFRunLoop; the dispatch-queue
	 * delivery is the one every SCDynamicStore watcher in this tree
	 * (ipconfigd's link watch, hostnamed) runs on.
	 */
	queue = dispatch_queue_create("com.apple.ipconfig.waitall", NULL);
	if (queue == NULL || !SCDynamicStoreSetDispatchQueue(session, queue)) {
		(void)fprintf(stderr, "SCDynamicStoreSetDispatchQueue "
		    "failed: %s\n", SCErrorString(SCError()));
		return (0);
	}

	(void)signal(SIGALRM, on_alarm);
	(void)memset(&v, 0, sizeof(v));
	v.it_value.tv_sec = (time_t)t;
	if (setitimer(ITIMER_REAL, &v, NULL) < 0) {
		perror("setitimer");
		return (0);
	}
	value = SCDynamicStoreCopyValue(session, STARTUP_KEY);
	if (value == NULL)
		dispatch_main();	/* exits from key_appeared / on_alarm */
	CFRelease(value);
	CFRelease(session);
	return (0);
}

static int
S_getifaddr(mach_port_t svc, int argc, char **argv)
{
	InterfaceName name;
	ip_address_t addr = 0;
	ipconfig_status_t status = ipconfig_status_internal_error_e;
	kern_return_t kr;
	struct in_addr in_a;
	char buf[INET_ADDRSTRLEN];

	(void)argc;
	InterfaceNameInit(name, argv[0]);

	kr = ipconfig_if_addr(svc, name, &addr, &status);
	if (kr != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: getifaddr %s: mach 0x%x\n",
		    progname, name, (unsigned)kr);
		return (1);
	}
	if (status != ipconfig_status_success_e) {
		(void)fprintf(stderr, "%s: getifaddr %s: status %d\n",
		    progname, name, (int)status);
		return (1);
	}

	in_a.s_addr = addr;
	if (inet_ntop(AF_INET, &in_a, buf, sizeof(buf)) == NULL) {
		(void)fprintf(stderr, "%s: getifaddr %s: inet_ntop failed\n",
		    progname, name);
		return (1);
	}
	(void)printf("%s\n", buf);
	return (0);
}

static int
S_ifcount(mach_port_t svc, int argc, char **argv)
{
	int count = -1;
	kern_return_t kr;

	(void)argc;
	(void)argv;

	kr = ipconfig_if_count(svc, &count);
	if (kr != KERN_SUCCESS) {
		(void)fprintf(stderr, "%s: ifcount: mach 0x%x\n", progname,
		    (unsigned)kr);
		return (1);
	}
	(void)printf("%d\n", count);
	return (0);
}

/*
 * The DHCP option names `getoption` accepts — Apple's dhcptag names for
 * the options a lease commonly carries — and how each prints. Apple's
 * dhcptag_to_str() prints only the first element of a multi-valued
 * option, so router and domain_name_server print one address.
 */
enum opt_type { OPT_IP, OPT_UINT32, OPT_STRING };

static const struct opt_info {
	int		tag;
	const char	*name;
	enum opt_type	type;
} options[] = {
	{  1, "subnet_mask",			OPT_IP },
	{  3, "router",				OPT_IP },
	{  6, "domain_name_server",		OPT_IP },
	{ 12, "host_name",			OPT_STRING },
	{ 15, "domain_name",			OPT_STRING },
	{ 28, "broadcast_address",		OPT_IP },
	{ 51, "lease_time",			OPT_UINT32 },
	{ 54, "server_identifier",		OPT_IP },
	{ 58, "renewal_t1_time_value",		OPT_UINT32 },
	{ 59, "rebinding_t2_time_value",	OPT_UINT32 },
	{  0, NULL,				OPT_IP },
};

static const struct opt_info *
option_with_tag(int tag)
{
	int i;

	for (i = 0; options[i].name != NULL; i++) {
		if (options[i].tag == tag)
			return (&options[i]);
	}
	return (NULL);
}

/*
 * Render option `tag` as Apple's dhcptag_to_str() does. Returns false
 * if the tag is unknown or the data is too short for its type.
 */
static bool
option_to_str(char *buf, size_t bufsz, int tag, const uint8_t *opt,
    mach_msg_type_number_t len)
{
	const struct opt_info *info;
	struct in_addr ip;
	uint32_t u32;

	info = option_with_tag(tag);
	if (info == NULL)
		return (false);
	switch (info->type) {
	case OPT_IP:
		if (len < 4)
			return (false);
		(void)memcpy(&ip, opt, 4);
		(void)snprintf(buf, bufsz, "%s", inet_ntoa(ip));
		return (true);
	case OPT_UINT32:
		if (len < 4)
			return (false);
		(void)memcpy(&u32, opt, 4);
		(void)snprintf(buf, bufsz, "%u", ntohl(u32));
		return (true);
	case OPT_STRING:
		(void)snprintf(buf, bufsz, "%.*s", (int)len,
		    (const char *)opt);
		return (true);
	}
	return (false);
}

static int
S_get_option(mach_port_t svc, int argc, char **argv)
{
	char buf[1024];
	InterfaceName name;
	dataOut_t option = NULL;
	mach_msg_type_number_t option_cnt = 0;
	ipconfig_status_t status = ipconfig_status_internal_error_e;
	kern_return_t kr;
	bool success;
	int i, tag = -1;

	(void)argc;
	InterfaceNameInit(name, argv[0]);
	for (i = 0; options[i].name != NULL; i++) {
		if (strcmp(argv[1], options[i].name) == 0) {
			tag = options[i].tag;
			break;
		}
	}
	if (tag == -1)
		tag = atoi(argv[1]);

	kr = ipconfig_get_option(svc, name, tag, &option, &option_cnt,
	    &status);
	if (kr != KERN_SUCCESS) {
		(void)fprintf(stderr, "ipconfig_get_option failed, 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	if (status != ipconfig_status_success_e)
		return (1);
	success = option_to_str(buf, sizeof(buf), tag, option, option_cnt);
	if (option != NULL)
		(void)vm_deallocate(mach_task_self(), (vm_address_t)option,
		    option_cnt);
	if (success) {
		(void)printf("%s\n", buf);
		return (0);
	}
	(void)fprintf(stderr, "couldn't convert the option, tag %d\n", tag);
	return (1);
}

/*
 * Print a property list the way Apple's SCPrint("%@") does — the
 * SystemConfiguration description format, keys sorted:
 *
 *   <dictionary> {
 *     IPv4 : <array> {
 *       0 : <dictionary> {
 *   ...
 */
static CFComparisonResult
key_compare(const void *a, const void *b, void *ctx)
{
	(void)ctx;
	return (CFStringCompare(a, b, 0));
}

static void
print_plist(CFTypeRef obj, int indent)
{
	CFTypeID type = CFGetTypeID(obj);
	char buf[512];
	CFIndex i, n;

	if (type == CFDictionaryGetTypeID()) {
		CFMutableArrayRef sorted;
		const void **keys;

		n = CFDictionaryGetCount(obj);
		keys = calloc((size_t)n + 1, sizeof(*keys));
		if (keys == NULL)
			return;
		CFDictionaryGetKeysAndValues(obj, keys, NULL);
		sorted = CFArrayCreateMutable(NULL, n, &kCFTypeArrayCallBacks);
		for (i = 0; i < n; i++)
			CFArrayAppendValue(sorted, keys[i]);
		free(keys);
		CFArraySortValues(sorted, CFRangeMake(0, n), key_compare,
		    NULL);
		(void)printf("<dictionary> {\n");
		for (i = 0; i < n; i++) {
			CFStringRef k = CFArrayGetValueAtIndex(sorted, i);

			if (!CFStringGetCString(k, buf, sizeof(buf),
			    kCFStringEncodingUTF8))
				(void)strlcpy(buf, "?", sizeof(buf));
			(void)printf("%*s%s : ", indent + 2, "", buf);
			print_plist(CFDictionaryGetValue(obj, k), indent + 2);
			(void)printf("\n");
		}
		CFRelease(sorted);
		(void)printf("%*s}", indent, "");
	} else if (type == CFArrayGetTypeID()) {
		n = CFArrayGetCount(obj);
		(void)printf("<array> {\n");
		for (i = 0; i < n; i++) {
			(void)printf("%*s%ld : ", indent + 2, "", (long)i);
			print_plist(CFArrayGetValueAtIndex(obj, i), indent + 2);
			(void)printf("\n");
		}
		(void)printf("%*s}", indent, "");
	} else if (type == CFStringGetTypeID()) {
		if (!CFStringGetCString(obj, buf, sizeof(buf),
		    kCFStringEncodingUTF8))
			(void)strlcpy(buf, "?", sizeof(buf));
		(void)printf("%s", buf);
	} else if (type == CFBooleanGetTypeID()) {
		(void)printf("%s", CFBooleanGetValue(obj) ? "TRUE" : "FALSE");
	} else if (type == CFNumberGetTypeID()) {
		long long ll = 0;

		(void)CFNumberGetValue(obj, kCFNumberLongLongType, &ll);
		(void)printf("%lld", ll);
	} else if (type == CFDataGetTypeID()) {
		const UInt8 *p = CFDataGetBytePtr(obj);

		n = CFDataGetLength(obj);
		(void)printf("<data> 0x");
		for (i = 0; i < n; i++)
			(void)printf("%02x", p[i]);
	} else {
		(void)printf("<%ld>", (long)type);
	}
}

static int
S_get_summary(mach_port_t svc, int argc, char **argv)
{
	InterfaceName name;
	xmlDataOut_t summary_data = NULL;
	mach_msg_type_number_t summary_data_len = 0;
	ipconfig_status_t status = ipconfig_status_internal_error_e;
	CFPropertyListRef summary = NULL;
	CFDataRef data;
	kern_return_t kr;

	(void)argc;
	InterfaceNameInit(name, argv[0]);
	kr = ipconfig_get_summary(svc, name, &summary_data, &summary_data_len,
	    &status);
	if (kr != KERN_SUCCESS) {
		(void)fprintf(stderr, "ipconfig_get_summary failed, 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	if (summary_data != NULL) {
		data = CFDataCreate(NULL, summary_data, summary_data_len);
		if (data != NULL) {
			summary = CFPropertyListCreateWithData(NULL, data,
			    kCFPropertyListImmutable, NULL, NULL);
			CFRelease(data);
		}
		(void)vm_deallocate(mach_task_self(),
		    (vm_address_t)summary_data, summary_data_len);
		if (summary == NULL)
			(void)fprintf(stderr, "failed to decode summary\n");
	}
	if (status != ipconfig_status_success_e) {
		(void)fprintf(stderr, "ipconfig_get_summary(%s) failed: %s\n",
		    name, ipconfig_status_string(status));
		if (summary != NULL)
			CFRelease(summary);
		return (1);
	}
	if (summary != NULL) {
		print_plist(summary, 0);
		(void)printf("\n");
		CFRelease(summary);
	}
	return (0);
}

/* Set `prop` to a one-element array holding `value` (Apple's helper). */
static void
dict_set_as_array(CFMutableDictionaryRef dict, CFStringRef prop,
    CFTypeRef value)
{
	CFArrayRef arr;

	arr = CFArrayCreate(NULL, &value, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(dict, prop, arr);
	CFRelease(arr);
}

static void
dict_set_cstr_as_array(CFMutableDictionaryRef dict, CFStringRef prop,
    const char *s)
{
	CFStringRef str;

	str = CFStringCreateWithCString(NULL, s, kCFStringEncodingASCII);
	dict_set_as_array(dict, prop, str);
	CFRelease(str);
}

static CFStringRef
IPv4ConfigMethodGet(const char *m)
{
	if (strcasecmp("bootp", m) == 0)
		return (CFSTR("BOOTP"));
	if (strcasecmp("dhcp", m) == 0)
		return (CFSTR("DHCP"));
	if (strcasecmp("manual", m) == 0)
		return (CFSTR("Manual"));
	if (strcasecmp("inform", m) == 0)
		return (CFSTR("INFORM"));
	if (strcasecmp("linklocal", m) == 0)
		return (CFSTR("LinkLocal"));
	if (strcasecmp("failover", m) == 0)
		return (CFSTR("Failover"));
	return (NULL);
}

static CFDictionaryRef
IPv4ConfigDictCreate(const char *ifname, int argc, char **argv,
    const char *cmd, const char *method_name, CFStringRef config_method)
{
	CFMutableDictionaryRef dict;
	bool failover = CFEqual(config_method, CFSTR("Failover"));
	struct in_addr ip;
	char buf[INET_ADDRSTRLEN];

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(dict, kIPCfgConfigMethod, config_method);
	if (failover || CFEqual(config_method, CFSTR("INFORM")) ||
	    CFEqual(config_method, CFSTR("Manual"))) {
		if (failover && (argc < 1 || argc > 3)) {
			(void)fprintf(stderr, "usage: ipconfig %s %s %s "
			    "<ip-address> [ <subnet-mask> [ <timeout> ] ]\n",
			    cmd, ifname, method_name);
			goto failed;
		} else if (!failover && (argc < 1 || argc > 2)) {
			(void)fprintf(stderr, "usage: ipconfig %s %s %s "
			    "<ip-address> [ <subnet-mask> ]\n",
			    cmd, ifname, method_name);
			goto failed;
		}
		if (inet_aton(argv[0], &ip) == 0) {
			(void)fprintf(stderr, "Invalid IP address %s\n",
			    argv[0]);
			goto failed;
		}
		dict_set_cstr_as_array(dict, kIPCfgAddresses,
		    inet_ntop(AF_INET, &ip, buf, sizeof(buf)));
		if (argc >= 2) {
			if (inet_aton(argv[1], &ip) != 1) {
				(void)fprintf(stderr, "Invalid IP mask %s\n",
				    argv[1]);
				goto failed;
			}
			dict_set_cstr_as_array(dict, kIPCfgSubnetMasks,
			    inet_ntop(AF_INET, &ip, buf, sizeof(buf)));
		}
		if (argc >= 3) {
			int32_t timeout = (int32_t)strtol(argv[2], NULL, 0);

			if (timeout > 0) {
				CFNumberRef num;

				num = CFNumberCreate(NULL, kCFNumberSInt32Type,
				    &timeout);
				CFDictionarySetValue(dict,
				    kIPCfgFailoverAddressTimeout, num);
				CFRelease(num);
			}
		}
	} else if (argc != 0) {
		(void)fprintf(stderr, "too many arguments for method\n");
		goto failed;
	}
	return (dict);

failed:
	CFRelease(dict);
	return (NULL);
}

static CFStringRef
IPv6ConfigMethodGet(const char *m)
{
	if (strcasecmp(m, "automatic-v6") == 0)
		return (CFSTR("Automatic"));
	if (strcasecmp(m, "manual-v6") == 0)
		return (CFSTR("Manual"));
	if (strcasecmp(m, "6to4") == 0)
		return (CFSTR("6to4"));
	if (strcasecmp(m, "linklocal-v6") == 0)
		return (CFSTR("LinkLocal"));
	return (NULL);
}

static CFDictionaryRef
IPv6ConfigDictCreate(const char *ifname, int argc, char **argv,
    const char *cmd, const char *method_name, CFStringRef config_method)
{
	CFMutableDictionaryRef ipv6_dict;
	CFDictionaryRef dict;
	CFStringRef ipv6_key = kIPCfgEntIPv6;
	struct in6_addr ip;
	char buf[INET6_ADDRSTRLEN];

	ipv6_dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(ipv6_dict, kIPCfgConfigMethod, config_method);
	if (strcasecmp(method_name, "manual-v6") == 0) {
		int prefix_length;
		CFNumberRef num;

		if (argc != 2) {
			(void)fprintf(stderr, "usage: ipconfig %s %s "
			    "%s6 <ipv6-address> <prefix-length>\n",
			    cmd, ifname, method_name);
			goto failed;
		}
		if (inet_pton(AF_INET6, argv[0], &ip) != 1) {
			(void)fprintf(stderr, "Invalid IPv6 address %s\n",
			    argv[0]);
			goto failed;
		}
		dict_set_cstr_as_array(ipv6_dict, kIPCfgAddresses,
		    inet_ntop(AF_INET6, &ip, buf, sizeof(buf)));
		prefix_length = (int)strtol(argv[1], NULL, 0);
		if (prefix_length < 0 || prefix_length > 128) {
			(void)fprintf(stderr, "Invalid prefix_length %s\n",
			    argv[1]);
			goto failed;
		}
		num = CFNumberCreate(NULL, kCFNumberIntType, &prefix_length);
		dict_set_as_array(ipv6_dict, kIPCfgPrefixLength, num);
		CFRelease(num);
	} else if (argc != 0) {
		(void)fprintf(stderr, "too many arguments for method\n");
		goto failed;
	}
	dict = CFDictionaryCreate(NULL, (const void **)&ipv6_key,
	    (const void **)&ipv6_dict, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFRelease(ipv6_dict);
	return (dict);

failed:
	CFRelease(ipv6_dict);
	return (NULL);
}

static CFDictionaryRef
ConfigDictCreate(const char *ifname, int argc, char **argv, const char *cmd,
    const char *method_name)
{
	CFStringRef config_method;

	config_method = IPv4ConfigMethodGet(method_name);
	if (config_method != NULL)
		return (IPv4ConfigDictCreate(ifname, argc, argv, cmd,
		    method_name, config_method));
	config_method = IPv6ConfigMethodGet(method_name);
	if (config_method == NULL) {
		(void)fprintf(stderr, "ipconfig %s: invalid method '%s'\n"
		    "<method> is one of %s\n", cmd, method_name,
		    METHOD_LIST_WITH_NONE);
		return (NULL);
	}
	return (IPv6ConfigDictCreate(ifname, argc, argv, cmd, method_name,
	    config_method));
}

/*
 * set <if> <method> [args] — Apple's S_set(). NONE sends no method
 * data; NONE-V4 / NONE-V6 send an empty IPv4 / IPv6 dictionary; every
 * other method sends its config dictionary as a binary plist.
 */
static int
S_set(mach_port_t svc, int argc, char **argv)
{
	CFDataRef data = NULL;
	CFDictionaryRef dict = NULL;
	const char *method_name;
	InterfaceName name;
	kern_return_t kr;
	ipconfig_status_t status = ipconfig_status_success_e;
	const uint8_t *xml_data_ptr = NULL;	/* NONE: no method data */
	CFIndex xml_data_len = 0;

	InterfaceNameInit(name, argv[0]);
	method_name = argv[1];
	argv += 2;
	argc -= 2;

	if (strcasecmp(method_name, "NONE") == 0) {
		/* nothing to do, NONE implies NULL method data */
	} else if (strcasecmp(method_name, "NONE-V6") == 0 ||
	    strcasecmp(method_name, "NONE-V4") == 0) {
		CFDictionaryRef empty_dict;
		CFStringRef ip_key;

		/* NONE-V{4,6} is represented as an empty IPv{4,6} dictionary */
		empty_dict = CFDictionaryCreate(NULL, NULL, NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		ip_key = (strcasecmp(method_name, "NONE-V6") == 0)
		    ? kIPCfgEntIPv6 : kIPCfgEntIPv4;
		dict = CFDictionaryCreate(NULL, (const void **)&ip_key,
		    (const void **)&empty_dict, 1,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		CFRelease(empty_dict);
	} else {
		dict = ConfigDictCreate(name, argc, argv, command_name,
		    method_name);
		if (dict == NULL)
			return (1);
	}
	if (dict != NULL) {
		data = CFPropertyListCreateData(NULL, dict,
		    kCFPropertyListBinaryFormat_v1_0, 0, NULL);
		CFRelease(dict);
		if (data == NULL) {
			(void)fprintf(stderr, "failed to allocate memory\n");
			return (1);
		}
		xml_data_ptr = CFDataGetBytePtr(data);
		xml_data_len = CFDataGetLength(data);
	}
	kr = ipconfig_set(svc, name, (xmlData_t)(uintptr_t)xml_data_ptr,
	    (mach_msg_type_number_t)xml_data_len, &status);
	if (data != NULL)
		CFRelease(data);
	if (kr != KERN_SUCCESS) {
		mach_error("ipconfig_set failed", kr);
		return (1);
	}
	if (status != ipconfig_status_success_e) {
		(void)fprintf(stderr, "ipconfig_set %s %s failed: %s\n",
		    name, method_name, ipconfig_status_string(status));
		return (1);
	}
	return (0);
}

static const struct command_info {
	const char	*name;
	int		(*func)(mach_port_t, int, char **);
	int		min_argc;
	const char	*usage;
	int		no_server;	/* doesn't talk to ipconfigd's port */
} commands[] = {
	{ "waitall",	S_wait_all,	0, "[ timeout secs ]", 1 },
	{ "getifaddr",	S_getifaddr,	1, "<interface name>", 0 },
	{ "ifcount",	S_ifcount,	0, "", 0 },
	{ "getoption",	S_get_option,	2,
	  " <interface name | \"\" > <option name> | <option code>", 0 },
	{ "getsummary",	S_get_summary,	1, " <interface name>", 0 },
	{ "set",	S_set,		2,
	  "<interface name> <method> <method args>\n"
	  "<method> is one of " METHOD_LIST_WITH_NONE, 0 },
	{ NULL,		NULL,		0, NULL, 0 },
};

static void
usage(void)
{
	int i;

	(void)fprintf(stderr, "usage: %s <command> <args>\n", progname);
	(void)fprintf(stderr, "where <command> is one of ");
	for (i = 0; commands[i].name != NULL; i++) {
		(void)fprintf(stderr, "%s%s", i == 0 ? "" : ", ",
		    commands[i].name);
	}
	(void)fprintf(stderr, "\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	int i;

	if (argv[0] != NULL) {
		const char *slash = strrchr(argv[0], '/');

		progname = (slash != NULL) ? slash + 1 : argv[0];
	}

	if (argc < 2)
		usage();
	command_name = argv[1];

	for (i = 0; commands[i].name != NULL; i++) {
		if (strcasecmp(command_name, commands[i].name) != 0)
			continue;
		if ((argc - 2) < commands[i].min_argc) {
			(void)fprintf(stderr, "usage: ipconfig %s %s\n",
			    commands[i].name, commands[i].usage);
			return (1);
		}
		if (!commands[i].no_server) {
			kr = bootstrap_look_up(bootstrap_port,
			    "com.apple.IPConfiguration", &svc);
			if (kr == BOOTSTRAP_UNKNOWN_SERVICE) {
				(void)fprintf(stderr,
				    "ipconfig server not active\n");
				return (1);
			}
			if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
				(void)fprintf(stderr,
				    "%s: bootstrap_look_up failed: 0x%x\n",
				    progname, (unsigned)kr);
				return (1);
			}
		}
		return (commands[i].func(svc, argc - 2, &argv[2]));
	}

	usage();
	return (1);
}
