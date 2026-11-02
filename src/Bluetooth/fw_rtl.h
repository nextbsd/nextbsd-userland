/*
 * fw_rtl.h — Realtek firmware download (a port of rtlbtfw). See fw_rtl.c.
 */
#ifndef _BLUETOOTHD_FW_RTL_H_
#define _BLUETOOTHD_FW_RTL_H_

#include <stdbool.h>

#include <libusb.h>

/* Is interface 0 a Bluetooth HCI (e0/01/01)? For VID 0x0bda devices. */
bool	fw_rtl_is_bluetooth(libusb_device *dev);

/*
 * Download the patch (and config) for the chip behind `hdl` from
 * `firmware_dir`. Returns a FW_RESULT_* (firmware.h). On FW_RESULT_DOWNLOADED
 * the caller resets the device, so it re-enumerates and ng_ubt attaches.
 */
int	fw_rtl_run(libusb_device_handle *hdl, const char *firmware_dir);

#endif /* _BLUETOOTHD_FW_RTL_H_ */
