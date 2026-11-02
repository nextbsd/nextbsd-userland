/*
 * fw_intel.h — Intel firmware download (a port of iwmbtfw). See fw_intel.c.
 */
#ifndef _BLUETOOTHD_FW_INTEL_H_
#define _BLUETOOTHD_FW_INTEL_H_

#include <libusb.h>

#include "fw_ids.h"

/*
 * Identify the controller behind `hdl`, and download and boot its firmware
 * from `firmware_dir` if it is in its bootloader. Returns a FW_RESULT_*
 * (firmware.h). On FW_RESULT_DOWNLOADED the caller resets the device, so it
 * re-enumerates and ng_ubt attaches.
 */
int	fw_intel_run(libusb_device_handle *hdl, enum fw_intel_gen gen,
	    const char *firmware_dir);

#endif /* _BLUETOOTHD_FW_INTEL_H_ */
