/*
 * powermanagement_stubs.c — interim server routines for powermanagement.defs.
 *
 * pmconfigd serves com.apple.PowerManagement.control (MIG subsystem 73000,
 * libIOKit/IOKit/pwr_mgt/powermanagement.defs). On macOS each _io_*
 * routine is implemented in the pmconfigd source file that owns that
 * feature: assertions in PMAssertions.c, IOPMConnection in PMConnection.m,
 * power sources in BatteryTimeRemaining.m, scheduled events in
 * AutoWakeScheduler.c, and so on. Not all of those files are built yet.
 * (The io_ps_* power-source routines are served by
 * BatteryTimeRemaining_freebsd.c.)
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

#include "PrivateLib_freebsd.h"

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

/*
 * Apple's selectors here (pmconfigd.m) are not built yet. NextBSD's
 * kIOPMSetDisplaySleepNow is pmset displaysleepnow: powerd is the display
 * wrangler (RootDomainIdle_freebsd.c), and any user may ask it to turn the
 * display off, as any user may set IORequestIdle on xnu's IODisplayWrangler.
 */
kern_return_t
_io_pm_set_value_int(mach_port_t server, audit_token_t token, int selector,
    int value, int *return_val)
{
	(void)server; (void)token;
	if (selector == kIOPMSetDisplaySleepNow && value != 0) {
		pmIdleDisplaySleepNow();
		*return_val = kIOReturnSuccess;
		return (KERN_SUCCESS);
	}
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
_io_pm_set_sleepservice_wake_time_cap(mach_port_t server, audit_token_t token,
    int cap_time, int *return_val)
{
	(void)server; (void)token; (void)cap_time;
	*return_val = PM_UNSUPPORTED;
	return (KERN_SUCCESS);
}
