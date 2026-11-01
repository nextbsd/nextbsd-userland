/*
 * DADevice.c — the FreeBSD side of unmount and eject (nextbsd-userland#198).
 *
 * DADeviceFindProcessOnVolume is fuser -c (usr.bin/fstat/fuser.c): every
 * process's files from libprocstat, matched on the vnode's fsid against the
 * st_dev of the mount point. It stands in for Apple's
 * proc_listpidspath(PROC_ALL_PIDS, 0, path,
 * PROC_LISTPIDSPATH_PATH_IS_VOLUME, ...) in __DARequestUnmountGetProcessID.
 * DADeviceFindProcessWithDevice is the device-node half, for
 * __DARequestEjectGetProcessID.
 *
 * DADeviceEject replaces Apple's __DARequestEjectEject (open the whole
 * disk, ioctl(DKIOCEJECT), ENOTTY counts as success):
 *   cd<N>  CDIOCALLOW (the tray may be locked), then CDIOCEJECT, on the
 *          disk itself; scsi_cd.c turns it into cdstopunit(periph, 1);
 *   da<N>  CAM START STOP UNIT, start=0 load_eject=1, through the disk's
 *          pass(4) peripheral (cam_open_device finds it), as
 *          sbin/camcontrol/camcontrol.c scsistart() does;
 *   other  vtbd, md, nvd, nda, ada, mmcsd: nothing to eject, success.
 */
#include "DADevice.h"

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/cdio.h>
#include <sys/ioctl.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libprocstat.h>
#include <limits.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <camlib.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>

/* Match one process's files; `test` says whether a vnode is the one. */
typedef int (*vnode_test)(const struct vnstat *vn, void *context);

static pid_t
find_process(vnode_test test, void *context)
{
	struct procstat *ps;
	struct kinfo_proc *procs;
	struct filestat_list *files;
	struct filestat *fst;
	struct vnstat vn;
	char errbuf[_POSIX2_LINE_MAX];
	unsigned int count, i;
	pid_t self = getpid(), found = 0;

	ps = procstat_open_sysctl();
	if (ps == NULL)
		return (0);
	procs = procstat_getprocs(ps, KERN_PROC_PROC, 0, &count);
	if (procs == NULL) {
		procstat_close(ps);
		return (0);
	}
	for (i = 0; i < count && found == 0; i++) {
		if (procs[i].ki_pid == self || procs[i].ki_pid == 0)
			continue;
		files = procstat_getfiles(ps, &procs[i], 0);
		if (files == NULL)
			continue;
		STAILQ_FOREACH(fst, files, next) {
			if (fst->fs_type != PS_FST_TYPE_VNODE)
				continue;
			if (procstat_get_vnode_info(ps, fst, &vn, errbuf) != 0)
				continue;
			if (test(&vn, context)) {
				found = procs[i].ki_pid;
				break;
			}
		}
		procstat_freefiles(ps, files);
	}
	procstat_freeprocs(ps, procs);
	procstat_close(ps);
	return (found);
}

static int
on_volume(const struct vnstat *vn, void *context)
{
	const dev_t *fsid = context;

	return (vn->vn_fsid == (uint64_t)*fsid);
}

pid_t
DADeviceFindProcessOnVolume(const char *path)
{
	struct stat sb;
	dev_t fsid;

	if (path == NULL || stat(path, &sb) != 0)
		return (0);
	fsid = sb.st_dev;
	return (find_process(on_volume, &fsid));
}

struct device_names {
	char *const	*names;
	int		 count;
};

static int
is_device(const struct vnstat *vn, void *context)
{
	const struct device_names *d = context;
	int i;

	if (vn->vn_type != PS_FST_VTYPE_VCHR)
		return (0);
	for (i = 0; i < d->count; i++)
		if (strcmp(vn->vn_devname, d->names[i]) == 0)
			return (1);
	return (0);
}

pid_t
DADeviceFindProcessWithDevice(char *const *names, int count)
{
	struct device_names d = { names, count };

	if (names == NULL || count <= 0)
		return (0);
	return (find_process(is_device, &d));
}

/* "cd" for "/dev/cd0": the driver name in front of the unit number. */
static int
device_is(const char *id, const char *driver)
{
	const char *name = id;
	size_t n = strlen(driver);

	if (strncmp(name, _PATH_DEV, sizeof(_PATH_DEV) - 1) == 0)
		name += sizeof(_PATH_DEV) - 1;
	return (strncmp(name, driver, n) == 0 &&
	    isdigit((unsigned char)name[n]));
}

static int
eject_cd(const char *id, char *how, size_t len)
{
	int fd, status = 0;

	fd = open(id, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		status = errno;
		(void)snprintf(how, len, "open: %s", strerror(status));
		return (status);
	}
	(void)ioctl(fd, CDIOCALLOW);
	if (ioctl(fd, CDIOCEJECT) != 0) {
		status = errno == ENOTTY ? 0 : errno;
		(void)snprintf(how, len, "CDIOCEJECT: %s", strerror(errno));
	} else
		(void)snprintf(how, len, "CDIOCEJECT");
	(void)close(fd);
	return (status);
}

static int
eject_da(const char *id, char *how, size_t len)
{
	struct cam_device *dev;
	union ccb *ccb;
	int status = 0;

	dev = cam_open_device(id, O_RDWR);
	if (dev == NULL) {
		/* No pass(4) peripheral: nothing CAM can eject (ENOTTY). */
		(void)snprintf(how, len, "no pass device (%s); nothing to "
		    "eject", cam_errbuf);
		return (0);
	}
	ccb = cam_getccb(dev);
	if (ccb == NULL) {
		cam_close_device(dev);
		(void)snprintf(how, len, "cam_getccb failed");
		return (ENOMEM);
	}
	CCB_CLEAR_ALL_EXCEPT_HDR(&ccb->csio);
	scsi_start_stop(&ccb->csio,
	    /* retries */ 1,
	    /* cbfcnp */ NULL,
	    /* tag_action */ MSG_SIMPLE_Q_TAG,
	    /* start */ 0,
	    /* load_eject */ 1,
	    /* immediate */ 0,
	    /* sense_len */ SSD_FULL_SIZE,
	    /* timeout */ 120000);
	ccb->ccb_h.flags |= CAM_DEV_QFRZDIS | CAM_PASS_ERR_RECOVER;
	if (cam_send_ccb(dev, ccb) < 0) {
		status = errno != 0 ? errno : EIO;
		(void)snprintf(how, len, "START STOP UNIT via %s%u: %s",
		    dev->device_name, (unsigned)dev->dev_unit_num, strerror(status));
	} else if ((ccb->ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
		status = EIO;
		(void)snprintf(how, len, "START STOP UNIT via %s%u: CAM "
		    "status 0x%x", dev->device_name, (unsigned)dev->dev_unit_num,
		    (unsigned)(ccb->ccb_h.status & CAM_STATUS_MASK));
	} else
		(void)snprintf(how, len, "START STOP UNIT (eject) via %s%u",
		    dev->device_name, (unsigned)dev->dev_unit_num);
	cam_freeccb(ccb);
	cam_close_device(dev);
	return (status);
}

int
DADeviceEject(const char *id, char *how, size_t len)
{
	how[0] = '\0';
	if (device_is(id, "cd"))
		return (eject_cd(id, how, len));
	if (device_is(id, "da"))
		return (eject_da(id, how, len));
	(void)snprintf(how, len, "no eject for this device (ENOTTY)");
	return (0);
}
