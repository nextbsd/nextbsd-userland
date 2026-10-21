/*
 * FreeBSD shim for macOS <sys/kern_event.h>.
 *
 * launchd.c's Darwin monitor_networking_state() opens a PF_SYSTEM /
 * SYSPROTO_EVENT socket and arms a SIOCSKEVFILT filter to receive
 * Apple's kernel network-state events (interface up/down). NextBSD's
 * kernel has no PF_SYSTEM domain (yet), so on NextBSD launchd.c listens
 * on a PF_ROUTE socket instead (RTM_IFINFO / RTM_NEWADDR / RTM_DELADDR)
 * and none of the PF_SYSTEM code path is compiled.
 *
 * This shim still supplies the types and constants so that the header
 * include and any remaining Darwin-only references compile. PF_SYSTEM
 * is not a real NextBSD domain; socket(PF_SYSTEM, ...) would fail with
 * EAFNOSUPPORT.
 */
#ifndef _FREEBSD_SHIM_SYS_KERN_EVENT_H_
#define _FREEBSD_SHIM_SYS_KERN_EVENT_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#ifndef PF_SYSTEM
#define PF_SYSTEM	32	/* not a real FreeBSD domain — socket() fails EAFNOSUPPORT */
#endif
#ifndef AF_SYSTEM
#define AF_SYSTEM	PF_SYSTEM
#endif
#ifndef SYSPROTO_EVENT
#define SYSPROTO_EVENT	1
#endif

#define KEV_VENDOR_APPLE	1
#define KEV_NETWORK_CLASS	1

struct kev_request {
	u_int32_t	vendor_code;
	u_int32_t	kev_class;
	u_int32_t	kev_subclass;
};

struct kern_event_msg {
	u_int32_t	total_size;
	u_int32_t	vendor_code;
	u_int32_t	kev_class;
	u_int32_t	kev_subclass;
	u_int32_t	id;
	u_int32_t	event_code;
	u_int32_t	event_data[1];
};

#define SIOCSKEVFILT	_IOW('e', 2, struct kev_request)
#define SIOCGKEVID	_IOR('e', 3, u_int32_t)

#endif /* _FREEBSD_SHIM_SYS_KERN_EVENT_H_ */
