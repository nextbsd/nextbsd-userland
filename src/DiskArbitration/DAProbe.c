/*
 * DAProbe.c — the probe stage (E15 C4, nextbsd-userland#194).
 *
 * Port of Apple's DAProbe (DAProbe.c) and the probe half of DAFileSystem.c:
 *
 *   for each candidate bundle, in FSProbeOrder:
 *       <probe exec> -p <bsdname> removable readonly
 *           FSUR_RECOGNIZED   -> stdout is the volume name; stop
 *           otherwise         -> next candidate
 *   <probe exec> -k <bsdname>    -> FSUR_IO_SUCCESS, stdout = volume UUID
 *   <repair exec> -q <devpath>   -> 0 clean, else kDADiskStateRequireRepair
 *
 * As in Apple's DAStage, the clean check is skipped (counted clean) when
 * the media is not writable, and also when the volume is already mounted:
 * a UFS volume mounted read-write has fs_clean = 0 by design.
 *
 * The helpers run on a serial background queue so a slow device never
 * stalls the daemon queue; the result is applied on the daemon queue if the
 * disk is still the one the probe was started for (probe_token).
 *
 * The stage machine (DAStage.c, #195) starts each probe with the disk
 * marked kDADiskStateCommandActive; applying the result clears it, keeps
 * the winning bundle entry on the disk for the mount stage (Apple's
 * DADiskSetFileSystem) and signals the next stage.
 */
#include "DAProbe.h"
#include "DACommand.h"
#include "DAFileSystem.h"
#include "DAStage.h"
#include "loadable_fs.h"

#include <sys/param.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static dispatch_queue_t	g_da_queue;
static dispatch_queue_t	g_probe_queue;
static uint64_t		g_next_token;
static int		g_probe_ok_logged;

struct da_probe_job {
	char				*id;
	char				*bsdname;
	char				*media_uuid;	/* GPT partition UUID or NULL */
	uint64_t			 token;
	int				 writable;
	int				 mounted;
	struct da_probe_candidate	*cands;
	size_t				 ncands;

	/* Result. */
	int				 recognized;
	size_t				 which;		/* candidate that won */
	char				 label[512];
	char				 uuid[128];
	int				 clean;
};

static void
job_free(struct da_probe_job *job)
{
	DAFileSystemCandidatesFree(job->cands, job->ncands);
	free(job->id);
	free(job->bsdname);
	free(job->media_uuid);
	free(job);
}

/* A bundle helper's status and first output line (DACommandRun). */
static int
run_helper(char *const argv[], const char *media_uuid, char *out, size_t len)
{
	char envuuid[160];

	if (media_uuid == NULL)
		return (DACommandRun(argv, NULL, kDACommandExecuteOptionDefault,
		    out, len));
	(void)snprintf(envuuid, sizeof(envuuid), "DA_PARTITION_UUID=%s",
	    media_uuid);
	return (DACommandRun(argv, envuuid, kDACommandExecuteOptionDefault,
	    out, len));
}

/* Split FSProbeArguments ("-p", possibly "-p -x") into argv words. */
static int
split_args(char *args, char **words, int max)
{
	char *w;
	int n = 0;

	while (n < max && (w = strsep(&args, " \t")) != NULL)
		if (*w != '\0')
			words[n++] = w;
	return (n);
}

static void apply_result(void *ctx);

/* On the probe queue. */
static void
run_probe(void *ctx)
{
	struct da_probe_job *job = ctx;
	char out[512], devpath[MAXPATHLEN];
	size_t i;
	int st;

	for (i = 0; i < job->ncands && !job->recognized; i++) {
		struct da_probe_candidate *c = &job->cands[i];
		char *argv[12], *args;
		int n = 0;

		args = strdup(c->probe_args);
		if (args == NULL)
			continue;
		argv[n++] = c->probe_exec;
		n += split_args(args, argv + n, 6);
		argv[n++] = job->bsdname;
		argv[n++] = "removable";	/* literal, as Apple passes */
		argv[n++] = "readonly";
		argv[n] = NULL;
		st = run_helper(argv, NULL, out, sizeof(out));
		free(args);
		if (st == FSUR_RECOGNIZED) {
			job->recognized = 1;
			job->which = i;
			(void)strlcpy(job->label, out, sizeof(job->label));
		}
	}
	if (!job->recognized)
		goto done;

	/* -k: the volume UUID. */
	{
		struct da_probe_candidate *c = &job->cands[job->which];
		char *argv[] = { c->probe_exec, "-k", job->bsdname, NULL };

		st = run_helper(argv, job->media_uuid, out, sizeof(out));
		if (st == FSUR_IO_SUCCESS)
			(void)strlcpy(job->uuid, out, sizeof(job->uuid));
	}

	/* -q: is it clean? */
	job->clean = 1;
	if (job->writable && !job->mounted &&
	    job->cands[job->which].repair_exec != NULL) {
		struct da_probe_candidate *c = &job->cands[job->which];
		char *argv[4];

		(void)snprintf(devpath, sizeof(devpath), "/dev/%s",
		    job->bsdname);
		argv[0] = c->repair_exec;
		argv[1] = "-q";
		argv[2] = devpath;
		argv[3] = NULL;
		st = run_helper(argv, NULL, out, sizeof(out));
		job->clean = st == 0;
	}
done:
	dispatch_async_f(g_da_queue, job, apply_result);
}

/* On the daemon queue. */
static void
apply_result(void *ctx)
{
	struct da_probe_job *job = ctx;
	struct da_probe_candidate *c;
	struct da_disk *disk;
	CFStringRef s;
	CFUUIDRef uuid = NULL;

	disk = DADiskListFind(job->id);
	if (disk == NULL || disk->volume_only ||
	    disk->probe_token != job->token) {
		job_free(job);			/* the disk went or changed */
		DAStageSignal();
		return;
	}
	disk->state |= kDADiskStateStagedProbe;
	disk->state &= ~kDADiskStateCommandActive;
	if (!job->recognized) {
		/* No candidate at all (a disk with a table): debug only. */
		(job->ncands != 0 ? DALog : DALogDebug)(
		    "DA-PROBE: %s unrecognized (%zu candidates)", disk->id,
		    job->ncands);
		disk->state |= kDADiskStateRequireReprobe;
		job_free(job);
		DAStageSignal();
		return;
	}

	c = &job->cands[job->which];
	disk->state &= ~kDADiskStateRequireReprobe;
	DAFileSystemCandidatesFree(disk->filesystem, 1);
	disk->filesystem = DAFileSystemCandidateCopy(c);
	DADiskSetDescriptionString(disk, kDADiskDescriptionVolumeKindKey,
	    c->kind);
	if (job->label[0] != '\0')
		DADiskSetDescriptionString(disk,
		    kDADiskDescriptionVolumeNameKey, job->label);
	if (job->uuid[0] != '\0') {
		s = CFStringCreateWithCString(kCFAllocatorDefault, job->uuid,
		    kCFStringEncodingUTF8);
		if (s != NULL) {
			uuid = CFUUIDCreateFromString(kCFAllocatorDefault, s);
			CFRelease(s);
		}
	}
	DADiskSetDescription(disk, kDADiskDescriptionVolumeUUIDKey, uuid);
	DADiskSetDescriptionBool(disk, kDADiskDescriptionVolumeMountableKey, 1);
	if (job->clean)
		disk->state &= ~kDADiskStateRequireRepair;
	else
		disk->state |= kDADiskStateRequireRepair;
	disk->autodiskmount = c->autodiskmount;
	/* Apple's DAProbe: autodiskmount=false clears the automatic bits. */
	if (!c->autodiskmount)
		disk->state &= ~(_kDADiskStateMountAutomatic |
		    _kDADiskStateMountAutomaticNoDefer);

	DALog("DA-PROBE: %s kind=%s name=\"%s\" uuid=%s clean=%d "
	    "autodiskmount=%d bundle=%s", disk->id, c->kind, job->label,
	    uuid != NULL ? job->uuid : "-", job->clean, c->autodiskmount,
	    strrchr(c->bundle, '/') + 1);
	if (uuid != NULL)
		CFRelease(uuid);
	if (!g_probe_ok_logged && strcmp(c->kind, "ufs") == 0) {
		g_probe_ok_logged = 1;
		DALog("DA-PROBE-OK: %s probed as ufs through %s", disk->id,
		    strrchr(c->bundle, '/') + 1);
	}
	job_free(job);
	DAStageSignal();
}

void
DAProbeInit(dispatch_queue_t queue)
{
	g_da_queue = queue;
	g_probe_queue = dispatch_queue_create("org.nextbsd.diskarbitrationd.probe",
	    NULL);
}

int
DAProbeDisk(struct da_disk *disk)
{
	struct da_probe_job *job;
	char name[256], uuid[64];
	CFTypeRef w;

	if (g_probe_queue == NULL || disk->volume_only)
		return (-1);
	DAFileSystemListRefresh();

	job = calloc(1, sizeof(*job));
	if (job == NULL)
		return (-1);
	job->cands = DAFileSystemCopyCandidates(disk, &job->ncands);
	if (DADiskGetDescriptionCString(disk, kDADiskDescriptionMediaBSDNameKey,
	    name, sizeof(name)) != 0 || job->cands == NULL) {
		job_free(job);
		return (-1);
	}
	job->id = strdup(disk->id);
	job->bsdname = strdup(name);
	if (DADiskGetDescriptionCString(disk, kDADiskDescriptionMediaUUIDKey,
	    uuid, sizeof(uuid)) == 0)
		job->media_uuid = strdup(uuid);
	w = DADiskGetDescription(disk, kDADiskDescriptionMediaWritableKey);
	job->writable = w != NULL && CFGetTypeID(w) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue(w);
	job->mounted = DADiskGetDescription(disk,
	    kDADiskDescriptionVolumePathKey) != NULL;
	job->token = disk->probe_token = ++g_next_token;
	if (job->id == NULL || job->bsdname == NULL) {
		job_free(job);
		return (-1);
	}
	dispatch_async_f(g_probe_queue, job, run_probe);
	return (0);
}
