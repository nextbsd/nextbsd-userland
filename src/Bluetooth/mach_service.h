/*
 * mach_service.h — bluetoothd's Mach service thread.
 *
 * Owns the receive right for org.nextbsd.bluetooth and runs the MIG demux for
 * bluetooth.defs. Same shape as wland's and ipconfigd's mach_service.c.
 */
#ifndef _BLUETOOTHD_MACH_SERVICE_H_
#define _BLUETOOTHD_MACH_SERVICE_H_

int	mach_service_start(void);
void	mach_service_join(void);

#endif /* _BLUETOOTHD_MACH_SERVICE_H_ */
