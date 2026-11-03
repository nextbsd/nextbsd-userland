/*
 * xpc_globals.c — the global objects and constants <xpc/xpc.h>,
 * <xpc/connection.h> and <xpc/activity.h> declare XPC_EXPORT
 * (nextbsd-userland#562).
 *
 * The headers declared them, but nothing in libxpc defined them, so a
 * library or program that referred to one (XPC_ERROR_CONNECTION_INVALID
 * compared against an event, XPC_ERROR_KEY_DESCRIPTION read from one)
 * linked with --allow-shlib-undefined and then failed to load: rtld cannot
 * bind a data relocation, and the process exits before main().
 *
 * As on Darwin:
 *  - XPC_ERROR_CONNECTION_INTERRUPTED, XPC_ERROR_CONNECTION_INVALID and
 *    XPC_ERROR_TERMINATION_IMMINENT are immutable error objects:
 *    xpc_get_type() is XPC_TYPE_ERROR, and each is a dictionary holding
 *    one string, its description, under XPC_ERROR_KEY_DESCRIPTION. They are
 *    compared by address; xpc_retain() and xpc_release() leave them alone.
 *  - XPC_ERROR_KEY_DESCRIPTION is "XPCErrorDescription" and
 *    XPC_EVENT_KEY_NAME is "XPCEventName".
 *  - The XPC_ACTIVITY_* criteria keys are the key names of a launchd
 *    property list's com.apple.xpc.activity criteria, and the interval
 *    constants are seconds. XPC_ACTIVITY_CHECK_IN is a token compared by
 *    address. NextBSD's libxpc has no xpc_activity_register() yet; the
 *    constants are here so a client that names them still loads.
 */

#include <sys/types.h>
#include <mach/mach.h>
#include <xpc/launchd.h>
#include "xpc_internal.h"

const char *const _xpc_error_key_description = "XPCErrorDescription";
const char *const _xpc_event_key_name = "XPCEventName";

/*
 * An immutable dictionary with one string entry. The pair and the string
 * live in the same object, so the whole thing is one constant.
 */
struct _xpc_dictionary_s {
	struct xpc_object	xo;
	struct xpc_dict_pair	pair;
	struct xpc_object	value;
};

#define	_XPC_GLOBAL_STRING(s) {						\
	.xo_xpc_type = _XPC_TYPE_STRING,				\
	.xo_flags = _XPC_GLOBAL,					\
	.xo_refcnt = 1,							\
	.xo_size = sizeof(s) - 1,					\
	.xo_u = { .str = (s) },						\
}

#define	_XPC_GLOBAL_ERROR(name, desc)					\
const struct _xpc_dictionary_s name = {					\
	.xo = {								\
		.xo_xpc_type = _XPC_TYPE_ERROR,				\
		.xo_flags = _XPC_GLOBAL,				\
		.xo_refcnt = 1,						\
		.xo_size = 1,						\
		.xo_u = { .dict = {					\
			.tqh_first = (struct xpc_dict_pair *)&name.pair,\
			.tqh_last = (struct xpc_dict_pair **)		\
			    &name.pair.xo_link.tqe_next,		\
		} },							\
	},								\
	.pair = {							\
		.key = "XPCErrorDescription",				\
		.value = (struct xpc_object *)&name.value,		\
		.xo_link = {						\
			.tqe_next = NULL,				\
			.tqe_prev = (struct xpc_dict_pair **)		\
			    &name.xo.xo_u.dict.tqh_first,		\
		},							\
	},								\
	.value = _XPC_GLOBAL_STRING(desc),				\
}

_XPC_GLOBAL_ERROR(_xpc_error_connection_interrupted, "Connection interrupted");
_XPC_GLOBAL_ERROR(_xpc_error_connection_invalid, "Connection invalid");
_XPC_GLOBAL_ERROR(_xpc_error_termination_imminent, "Process will terminate soon");

/* <xpc/activity.h> */
const char *XPC_ACTIVITY_INTERVAL = "Interval";
const char *XPC_ACTIVITY_REPEATING = "Repeating";
const char *XPC_ACTIVITY_DELAY = "Delay";
const char *XPC_ACTIVITY_GRACE_PERIOD = "GracePeriod";
const int64_t XPC_ACTIVITY_INTERVAL_1_MIN = 60;
const int64_t XPC_ACTIVITY_INTERVAL_5_MIN = 5 * 60;
const int64_t XPC_ACTIVITY_INTERVAL_15_MIN = 15 * 60;
const int64_t XPC_ACTIVITY_INTERVAL_30_MIN = 30 * 60;
const int64_t XPC_ACTIVITY_INTERVAL_1_HOUR = 60 * 60;
const int64_t XPC_ACTIVITY_INTERVAL_4_HOURS = 4 * 60 * 60;
const int64_t XPC_ACTIVITY_INTERVAL_8_HOURS = 8 * 60 * 60;
const int64_t XPC_ACTIVITY_INTERVAL_1_DAY = 24 * 60 * 60;
const int64_t XPC_ACTIVITY_INTERVAL_7_DAYS = 7 * 24 * 60 * 60;
const char *XPC_ACTIVITY_PRIORITY = "Priority";
const char *XPC_ACTIVITY_PRIORITY_MAINTENANCE = "Maintenance";
const char *XPC_ACTIVITY_PRIORITY_UTILITY = "Utility";
const char *XPC_ACTIVITY_ALLOW_BATTERY = "AllowBattery";
const char *XPC_ACTIVITY_REQUIRE_SCREEN_SLEEP = "RequireScreenSleep";
const char *XPC_ACTIVITY_REQUIRE_BATTERY_LEVEL = "RequireBatteryLevel";
const char *XPC_ACTIVITY_REQUIRE_HDD_SPINNING = "RequireHDDSpinning";

static const struct xpc_object _xpc_activity_check_in =
    _XPC_GLOBAL_STRING("CheckIn");
const xpc_object_t XPC_ACTIVITY_CHECK_IN =
    (xpc_object_t)(uintptr_t)&_xpc_activity_check_in;
