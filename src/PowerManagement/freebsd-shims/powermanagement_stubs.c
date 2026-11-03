/*
 * powermanagement_stubs.c — interim server routines for powermanagement.defs.
 *
 * pmconfigd serves com.apple.PowerManagement.control (MIG subsystem 73000,
 * freebsd-shims/IOKit/pwr_mgt/powermanagement.defs). On macOS each _io_*
 * routine is implemented in the pmconfigd source file that owns that
 * feature: assertions in PMAssertions.c, IOPMConnection in PMConnection.m,
 * power sources in BatteryTimeRemaining.m, scheduled events in
 * AutoWakeScheduler.c, and so on. Not all of those files are built yet.
 *
 * Until each is, its routines live here. Every stub answers at once with
 * kIOReturnUnsupported in the routine's own return code, so an IOPMLib
 * client gets a prompt, well-formed "not supported" reply rather than a
 * hung mach_msg. The stubs take care of what MIG hands a successful
 * server routine:
 *   - in-line OOL buffers (pointer_t in) belong to the server and are
 *     vm_deallocate()d;
 *   - a port received as an argument (notify_port) is released;
 *   - OOL outputs are returned empty (address 0, count 0).
 *
 * When a ticket compiles the Apple file that implements a routine, it
 * deletes that routine from this file; the linker then catches a routine
 * defined twice or not at all.
 */
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>		/* kIOReturnUnsupported */
#include <string.h>

#include "PrivateLib_freebsd.h"		/* copyScheduledPowerEvents() ... */

#include "powermanagementServer.h"	/* MIG: the _io_* prototypes */

#define	PM_UNSUPPORTED	((int)kIOReturnUnsupported)

static void
pm_ool_release(vm_offset_t addr, mach_msg_type_number_t cnt)
{
	if (addr != 0 && cnt != 0)
		(void)vm_deallocate(mach_task_self(), addr, cnt);
}

static void
pm_ool_empty(vm_offset_t *addr, mach_msg_type_number_t *cnt)
{
	*addr = 0;
	*cnt = 0;
}

/* ---- PrivateLib.m / PMSettings.m: value get/set, active settings -------- */

kern_return_t
_io_pm_get_value_int(mach_port_t server, audit_token_t token, int selector,
    int *value)
{
	(void)server; (void)token; (void)selector;
	*value = 0;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_set_value_int(mach_port_t server, audit_token_t token, int selector,
    int value, int *return_val)
{
	(void)server; (void)token; (void)selector; (void)value;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_force_active_settings(mach_port_t server, audit_token_t token,
    vm_offset_t profiles, mach_msg_type_number_t profilesCnt, int *return_val)
{
	(void)server; (void)token;
	pm_ool_release(profiles, profilesCnt);
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/* ---- pmconfigd.m: bookkeeping ------------------------------------------ */

kern_return_t
_io_pm_last_wake_time(mach_port_t server, vm_offset_t *wakeData,
    mach_msg_type_number_t *wakeDataCnt, vm_offset_t *deltaData,
    mach_msg_type_number_t *deltaDataCnt, int *return_val)
{
	(void)server;
	pm_ool_empty(wakeData, wakeDataCnt);
	pm_ool_empty(deltaData, deltaDataCnt);
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/* ---- PMAssertions.c: IOPMAssertion* ------------------------------------ */

kern_return_t
_io_pm_assertion_create(mach_port_t server, audit_token_t token,
    vm_offset_t props, mach_msg_type_number_t propsCnt, int *assertion_id,
    int *disableAppSleep, int *enTrIntensity, int *return_code)
{
	(void)server; (void)token;
	pm_ool_release(props, propsCnt);
	*assertion_id = 0;
	*disableAppSleep = 0;
	*enTrIntensity = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_assertion_set_properties(mach_port_t server, audit_token_t token,
    int assertion_id, vm_offset_t props, mach_msg_type_number_t propsCnt,
    int *disableAppSleep, int *enableAppSleep, int *enTrIntensity,
    int *return_code)
{
	(void)server; (void)token; (void)assertion_id;
	pm_ool_release(props, propsCnt);
	*disableAppSleep = 0;
	*enableAppSleep = 0;
	*enTrIntensity = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_assertion_retain_release(mach_port_t server, audit_token_t token,
    int assertion_id, int action, int *retainCnt, int *disableAppSleep,
    int *enableAppSleep, int *return_code)
{
	(void)server; (void)token; (void)assertion_id; (void)action;
	*retainCnt = 0;
	*disableAppSleep = 0;
	*enableAppSleep = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/*
 * io_pm_assertion_copy_details also carries the scheduled power events:
 * IOPMCopyScheduledPowerEvents() and IOPMCopyRepeatingPowerEvents() (what
 * `pmset -g sched` prints) ask for kIOPMPowerEventsMIGCopyScheduledEvents
 * and kIOPMPowerEventsMIGCopyRepeatEvents. Those two selectors are served
 * here as PMAssertions.c serves them, from AutoWakeScheduler.c and
 * RepeatingAutoWake.c; the assertion selectors stay unsupported.
 */
kern_return_t
_io_pm_assertion_copy_details(mach_port_t server, audit_token_t token,
    int assertion_id, int whichData, vm_offset_t props,
    mach_msg_type_number_t propsCnt, vm_offset_t *assertions,
    mach_msg_type_number_t *assertionsCnt, int *return_val)
{
	CFTypeRef theCollection = NULL;
	CFDataRef serializedDetails;

	(void)server; (void)token; (void)assertion_id;
	pm_ool_release(props, propsCnt);
	pm_ool_empty(assertions, assertionsCnt);

	if (whichData == kIOPMPowerEventsMIGCopyScheduledEvents)
		theCollection = copyScheduledPowerEvents();
	else if (whichData == kIOPMPowerEventsMIGCopyRepeatEvents)
		theCollection = copyRepeatPowerEvents();
	else {
		*return_val = PM_UNSUPPORTED;
		return (KERN_SUCCESS);
	}

	/* PMAssertions.c: nothing to copy is success with no data. */
	*return_val = kIOReturnSuccess;
	if (theCollection == NULL)
		return (KERN_SUCCESS);

	serializedDetails = CFPropertyListCreateData(0, theCollection,
	    kCFPropertyListBinaryFormat_v1_0, 0, NULL);
	CFRelease(theCollection);
	if (serializedDetails == NULL) {
		*return_val = kIOReturnInternalError;
		return (KERN_SUCCESS);
	}

	*assertionsCnt =
	    (mach_msg_type_number_t)CFDataGetLength(serializedDetails);
	if (vm_allocate(mach_task_self(), (vm_address_t *)assertions,
	    *assertionsCnt, TRUE) != KERN_SUCCESS) {
		pm_ool_empty(assertions, assertionsCnt);
		*return_val = kIOReturnNoMemory;
	} else {
		memcpy((void *)*assertions, CFDataGetBytePtr(serializedDetails),
		    *assertionsCnt);
	}
	CFRelease(serializedDetails);
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_declare_system_active(mach_port_t server, audit_token_t token,
    int *state, vm_offset_t props, mach_msg_type_number_t propsCnt,
    int *assertion_id, int *return_code)
{
	(void)server; (void)token; (void)state; (void)assertion_id;
	pm_ool_release(props, propsCnt);
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_declare_user_active(mach_port_t server, audit_token_t token,
    int user_type, vm_offset_t props, mach_msg_type_number_t propsCnt,
    int *assertion_id, int *disableAppSleep, int *return_code)
{
	(void)server; (void)token; (void)user_type; (void)assertion_id;
	pm_ool_release(props, propsCnt);
	*disableAppSleep = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_declare_network_client_active(mach_port_t server, audit_token_t token,
    vm_offset_t props, mach_msg_type_number_t propsCnt, int *assertion_id,
    int *disableAppSleep, int *return_code)
{
	(void)server; (void)token; (void)assertion_id;
	pm_ool_release(props, propsCnt);
	*disableAppSleep = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_ctl_assertion_type(mach_port_t server, audit_token_t token,
    string_t assertion, int action, int *return_code)
{
	(void)server; (void)token; (void)assertion; (void)action;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_assertion_notify(mach_port_t server, audit_token_t token,
    string_t name, int req_type, int *return_code)
{
	(void)server; (void)token; (void)name; (void)req_type;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_assertion_activity_log(mach_port_t server, audit_token_t token,
    vm_offset_t *log, mach_msg_type_number_t *logCnt, uint32_t *entryCnt,
    uint32_t *overflow, int *return_code)
{
	(void)server; (void)token;
	pm_ool_empty(log, logCnt);
	*entryCnt = 0;
	*overflow = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_assertion_activity_aggregate(mach_port_t server, audit_token_t token,
    vm_offset_t *statsData, mach_msg_type_number_t *statsDataCnt,
    int *return_code)
{
	(void)server; (void)token;
	pm_ool_empty(statsData, statsDataCnt);
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_set_exception_limits(mach_port_t server, audit_token_t token,
    vm_offset_t props, mach_msg_type_number_t propsCnt, int *return_code)
{
	(void)server; (void)token;
	pm_ool_release(props, propsCnt);
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/* ---- PrivateLib.m: sleep/wake UUID ------------------------------------- */

kern_return_t
_io_pm_get_uuid(mach_port_t server, int selector, string_t out_uuid,
    int *return_val)
{
	(void)server; (void)selector;
	out_uuid[0] = '\0';
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/* ---- PMConnection.m: IOPMConnection ------------------------------------ */

kern_return_t
_io_pm_connection_create(mach_port_t server, audit_token_t token,
    string_t name, int interests, uint32_t *connection_id, int *return_code)
{
	(void)server; (void)token; (void)name; (void)interests;
	*connection_id = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_connection_schedule_notification(mach_port_t server,
    audit_token_t token, uint32_t connection_id, mach_port_t notify_port,
    int disable, int *return_code)
{
	(void)server; (void)token; (void)connection_id; (void)disable;
	if (MACH_PORT_VALID(notify_port))
		(void)mach_port_deallocate(mach_task_self(), notify_port);
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_connection_release(mach_port_t server, uint32_t connection_id,
    int *return_code)
{
	(void)server; (void)connection_id;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_connection_acknowledge_event(mach_port_t server, audit_token_t token,
    uint32_t connection_id, int messageToken, vm_offset_t options,
    mach_msg_type_number_t optionsCnt, int *return_code)
{
	(void)server; (void)token; (void)connection_id; (void)messageToken;
	pm_ool_release(options, optionsCnt);
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/* ---- BatteryTimeRemaining.m: power sources ----------------------------- */

kern_return_t
_io_ps_new_pspowersource(mach_port_t server, audit_token_t token, int *psid,
    int *return_code)
{
	(void)server; (void)token;
	*psid = 0;
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_ps_update_pspowersource(mach_port_t server, audit_token_t token,
    int psid, vm_offset_t psdetails, mach_msg_type_number_t psdetailsCnt,
    int *return_code)
{
	(void)server; (void)token; (void)psid;
	pm_ool_release(psdetails, psdetailsCnt);
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_ps_release_pspowersource(mach_port_t server, audit_token_t token,
    int psid)
{
	(void)server; (void)token; (void)psid;
	return (KERN_SUCCESS);
}

kern_return_t
_io_ps_copy_powersources_info(mach_port_t server, audit_token_t token,
    int pstype, vm_offset_t *powersources,
    mach_msg_type_number_t *powersourcesCnt, int *return_code)
{
	(void)server; (void)token; (void)pstype;
	pm_ool_empty(powersources, powersourcesCnt);
	*return_code = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/* ---- PMSystemEvents.c / HID activity ----------------------------------- */

kern_return_t
_io_pm_hid_event_report_activity(mach_port_t server, audit_token_t token,
    int _action, int *allow)
{
	(void)server; (void)token; (void)_action;
	*allow = 1;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_hid_event_copy_history(mach_port_t server, vm_offset_t *eventArray,
    mach_msg_type_number_t *eventArrayCnt, int *return_val)
{
	(void)server;
	pm_ool_empty(eventArray, eventArrayCnt);
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

/* ---- PrivateLib.m / PMAssertions.c: debug and tuning knobs ------------- */

kern_return_t
_io_pm_set_debug_flags(mach_port_t server, audit_token_t token,
    uint32_t newFlags, uint32_t setMode, uint32_t *oldFlags, int *return_val)
{
	(void)server; (void)token; (void)newFlags; (void)setMode;
	*oldFlags = 0;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_set_bt_wake_interval(mach_port_t server, audit_token_t token,
    uint32_t newInterval, uint32_t *oldInterval, int *return_val)
{
	(void)server; (void)token; (void)newInterval;
	*oldInterval = 0;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_set_dw_linger_interval(mach_port_t server, audit_token_t token,
    uint32_t newInterval, int *return_val)
{
	(void)server; (void)token; (void)newInterval;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_change_sa_assertion_behavior(mach_port_t server, audit_token_t token,
    uint32_t newFlags, uint32_t *oldFlags, int *return_val)
{
	(void)server; (void)token; (void)newFlags;
	*oldFlags = 0;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_set_sleepservice_wake_time_cap(mach_port_t server, audit_token_t token,
    int cap_time, int *return_val)
{
	(void)server; (void)token; (void)cap_time;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}

kern_return_t
_io_pm_get_capability_bits(mach_port_t server, audit_token_t token,
    uint32_t *capBits, int *return_val)
{
	(void)server; (void)token;
	*capBits = 0;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}
