/*
 * DAFileSystem.h — the filesystem bundle list and its probe candidates
 * (E15 C4, nextbsd-userland#194).
 *
 * Apple's DAFileSystemListRefresh (DASupport.m) reads every *.fs bundle
 * under /System/Library/Filesystems and /Library/Filesystems and turns each
 * FSMediaTypes entry into a probe candidate, sorted by FSProbeOrder. A disk
 * is probed with the candidates whose FSMediaProperties match its
 * description (DAProbe.c).
 */
#ifndef DISKARBITRATION_DAFILESYSTEM_H
#define DISKARBITRATION_DAFILESYSTEM_H

#include "DADisk.h"

struct da_probe_candidate {
	char		*bundle;	/* /System/Library/Filesystems/msdos.fs */
	char		*kind;		/* CFBundleName: the vfs type, "msdosfs" */
	char		*probe_exec;	/* absolute path of FSProbeExecutable */
	char		*probe_args;	/* FSProbeArguments, "-p" */
	char		*repair_exec;	/* first personality's FSRepairExecutable */
	char		*mount_exec;	/* first personality's FSMountExecutable */
	int		user_impl;	/* FSImplementation says "user" (FUSE) */
	int		order;		/* FSProbeOrder */
	int		autodiskmount;	/* 0 when the entry says autodiskmount=false */
	CFDictionaryRef	properties;	/* FSMediaProperties */
};

/*
 * Re-read the bundle directories if either changed since the last read
 * (directory mtime, as Apple gates it). Runs on the daemon queue.
 */
void	DAFileSystemListRefresh(void);

/*
 * The candidates that match `disk`, in probe order. Returns a malloc'd
 * array of copies (free with DAFileSystemCandidatesFree) and its length.
 */
struct da_probe_candidate *DAFileSystemCopyCandidates(
	    const struct da_disk *disk, size_t *count);
void	DAFileSystemCandidatesFree(struct da_probe_candidate *c, size_t count);

/*
 * One malloc'd copy of `c` (free with DAFileSystemCandidatesFree(copy, 1)):
 * the probe keeps the winning entry on the disk, as Apple's
 * DADiskSetFileSystem does, for the mount stage.
 */
struct da_probe_candidate *DAFileSystemCandidateCopy(
	    const struct da_probe_candidate *c);

#endif /* DISKARBITRATION_DAFILESYSTEM_H */
