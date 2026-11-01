/*
 * DAClient.h — declarations shared by libDiskArbitration's own files
 * (nextbsd-userland#197); Apple keeps the same set as __private_extern__
 * prototypes at the top of DiskArbitration.c and DADisk.c.
 */
#ifndef DISKARBITRATION_DACLIENT_H
#define DISKARBITRATION_DACLIENT_H

#include <DiskArbitration/DiskArbitration.h>
#include <DiskArbitration/DiskArbitrationPrivate.h>

#include <mach/mach.h>

#include <stdbool.h>

#include "DAInternal.h"

#define DA_PRIVATE	__attribute__((visibility("hidden")))

/* DiskArbitration.c */
DA_PRIVATE void		_DAInitialize(void);
DA_PRIVATE void		_DADispatchCallback(DASessionRef session,
			    void *address, void *context, _DACallbackKind kind,
			    CFTypeRef argument0, CFTypeRef argument1);

/* DADisk.c */
DA_PRIVATE void		_DADiskInitialize(void);
DA_PRIVATE char		*_DADiskGetID(DADiskRef disk);
DA_PRIVATE mach_port_t	_DADiskGetSessionID(DADiskRef disk);

/* DASession.c */
DA_PRIVATE void		_DASessionInitialize(void);
DA_PRIVATE mach_port_t	_DASessionGetID(DASessionRef session);
DA_PRIVATE bool		_DASessionIsKeepAlive(DASessionRef session);
DA_PRIVATE DAReturn	_DASessionRecreate(DASessionRef session);
/*
 * The session's callback table: what the daemon knows as an "address" is
 * an index into it, never a function pointer (Apple's register).
 */
DA_PRIVATE CFMutableDictionaryRef DACallbackCreate(CFAllocatorRef allocator,
			    mach_vm_offset_t address, mach_vm_offset_t context,
			    _DACallbackKind kind, CFIndex order,
			    CFDataRef match, CFDataRef watch);
DA_PRIVATE SInt32	DAAddCallbackToSession(DASessionRef session,
			    CFMutableDictionaryRef callback);
DA_PRIVATE void		DARemoveCallbackFromSessionWithKey(
			    DASessionRef session, SInt32 index);
DA_PRIVATE SInt32	DARemoveCallbackFromSession(DASessionRef session,
			    mach_vm_offset_t address, mach_vm_offset_t context);
/* A copy of the table entry, or NULL; caller releases. */
DA_PRIVATE CFDictionaryRef DACopyCallbackFromSession(DASessionRef session,
			    SInt32 index);

/* Ask the daemon again if the session lost it and keeps alive. */
#define DA_SESSION_REVIVE(session, fail)				\
	do {								\
		if (_DASessionGetID(session) == MACH_PORT_NULL &&	\
		    _DASessionIsKeepAlive(session) &&			\
		    _DASessionRecreate(session) != kDAReturnSuccess)	\
			fail;						\
	} while (0)

#endif /* DISKARBITRATION_DACLIENT_H */
