/*
 * DADevice.h — the FreeBSD side of unmount and eject (nextbsd-userland#198).
 *
 * Two things Apple's DARequest.c gets from XNU that NextBSD does
 * differently:
 *   - who keeps a volume busy: Apple's proc_listpidspath(...
 *     PROC_LISTPIDSPATH_PATH_IS_VOLUME ...); here fuser -c's walk over
 *     libprocstat;
 *   - the eject itself: Apple's ioctl(DKIOCEJECT); here CDIOCEJECT for
 *     cd(4) and a CAM START STOP UNIT with the eject bit for da(4), what
 *     `camcontrol eject` sends.
 * These run on a DAThread worker and touch nothing of the disk model.
 * Kept apart from DARequest.c so libprocstat's and CAM's headers stay out of
 * the files that include the Mach ones.
 */
#ifndef DISKARBITRATION_DADEVICE_H
#define DISKARBITRATION_DADEVICE_H

#include <sys/types.h>
#include <stddef.h>

/*
 * The first process (other than this one) with a file, current directory,
 * root or executable on the filesystem mounted at `path`; 0 if none.
 */
pid_t	DADeviceFindProcessOnVolume(const char *path);

/*
 * The first process (other than this one) with one of the device nodes
 * `names` open ("da0", "da0p1", without /dev/); 0 if none.
 */
pid_t	DADeviceFindProcessWithDevice(char *const *names, int count);

/*
 * Eject the whole disk `id` ("/dev/da0"). 0 on success, else an errno.
 * A disk with nothing to eject (vtbd, md, nvd, ada: Apple's ENOTTY) is a
 * success. `how` gets a short description of what was done or why it
 * failed, for the log.
 */
int	DADeviceEject(const char *id, char *how, size_t len);

#endif /* DISKARBITRATION_DADEVICE_H */
