/*
 * DiskArbitrationPrivate.h — the private DiskArbitration entry points
 * NextBSD clients use (nextbsd-userland#197).
 *
 * A subset of Apple's DiskArbitration/DiskArbitrationPrivate.h
 * (DiskArbitration-535.120.1.0.1), same names and signatures: the idle and
 * disk-list-complete callbacks a desktop needs to know when the initial
 * stream of disks is done, the keep-alive session, the dissenter's process
 * ID and the two serialization helpers. The legacy DiskArb* API, the
 * block variants and the Apple-only calls (mkdir/rmdir, adoption,
 * encodings) are left out.
 */
#ifndef __DISKARBITRATION_DISKARBITRATIONPRIVATE__
#define __DISKARBITRATION_DISKARBITRATIONPRIVATE__

#include <DiskArbitration/DiskArbitration.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern const CFStringRef kDADiskDescriptionAppearanceTimeKey;	/* CFNumber */
extern const CFStringRef kDADiskDescriptionMediaMatchKey;

#ifndef __DISKARBITRATIOND__

/* A disk from a serialized description (what callbacks carry). */
extern DADiskRef _DADiskCreateFromSerialization(CFAllocatorRef allocator,
    DASessionRef session, CFDataRef serialization);
extern DASessionRef _DADiskGetSession(DADiskRef disk);

/* The process that dissented (EBUSY on unmount names it, #198). */
extern pid_t DADissenterGetProcessID(DADissenterRef dissenter);

typedef void (*DAIdleCallback)(void *context);

/*
 * Called when the daemon has no disk left to stage: at once if it is idle
 * when this is registered, and then once after each burst of activity.
 */
extern void DARegisterIdleCallback(DASessionRef session,
    DAIdleCallback callback, void *context);

typedef void (*DADiskListCompleteCallback)(void *context);

/*
 * Called once the disks that existed when DARegisterDiskAppearedCallback
 * was registered have all been reported. Register it first.
 */
extern void DARegisterDiskListCompleteCallback(DASessionRef session,
    DADiskListCompleteCallback callback, void *context);

/*
 * Keep the session across a daemon restart: when diskarbitrationd exits,
 * the session waits for it to come back
 * (com.apple.diskarbitrationd.launched) and registers its callbacks again.
 * `queue` is where that happens; pass the session's dispatch queue.
 */
extern DAReturn DASessionKeepAlive(DASessionRef session,
    dispatch_queue_t queue);

#endif /* !__DISKARBITRATIOND__ */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !__DISKARBITRATION_DISKARBITRATIONPRIVATE__ */
