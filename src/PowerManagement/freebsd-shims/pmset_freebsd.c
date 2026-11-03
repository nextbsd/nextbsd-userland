/*
 * pmset_freebsd.c — builds Apple's pmset/pmset.m as C (nextbsd-userland#235,
 * E17 C1).
 *
 * pmset.m is C in an Objective-C file: on an Intel Mac its only
 * Objective-C is a SkyLight import that is not taken. NextBSD's base has no
 * Objective-C runtime, and bsd.prog.mk would compile a .m source as
 * Objective-C, so this file includes it as C, the way PMSettings_freebsd.c
 * builds pmconfigd's PMSettings.m. pmset_freebsd.h lists what stands in for
 * the Darwin headers and frameworks.
 */
#include "../pmset/pmset.m"

/*
 * ---- common/CommonLib.c, verbatim --------------------------------------
 * The two CommonLib.c functions pmset calls. Apple builds CommonLib.c into
 * pmset too; the rest of it is powerd's (getRootDomain() and the
 * settings code need powerd's headers).
 */
__private_extern__ CFAbsoluteTime _CFAbsoluteTimeFromPMEventTimeStamp(uint64_t kernelPackedTime)
{
    uint32_t    cal_sec = (uint32_t)(kernelPackedTime >> 32);
    uint32_t    cal_micro = (uint32_t)(kernelPackedTime & 0xFFFFFFFF);
    CFAbsoluteTime timeKernelEpoch = (CFAbsoluteTime)(double)cal_sec + (double)cal_micro/1000.0;

    // Adjust from kernel 1970 epoch to CF 2001 epoch
    timeKernelEpoch -= kCFAbsoluteTimeIntervalSince1970;

    return timeKernelEpoch;
}

__private_extern__  asl_object_t open_pm_asl_store(char *store)
{
    asl_object_t        response = NULL;
    size_t              endMessageID;

    if (!store) {
        return NULL;
    }
    asl_object_t query = asl_new(ASL_TYPE_LIST);
    if (query != NULL)
    {
		asl_object_t cq = asl_new(ASL_TYPE_QUERY);
		if (cq != NULL)
		{
			asl_set_query(cq, ASL_KEY_FACILITY, kPMFacility, ASL_QUERY_OP_EQUAL);
			asl_append(query, cq);
			asl_release(cq);

			asl_object_t pmstore = asl_open_path(store, 0);
			if (pmstore != NULL) {
				response = asl_match(pmstore, query, &endMessageID, 0, 0, 0, ASL_MATCH_DIRECTION_FORWARD);
			}
			asl_release(pmstore);
		}
		asl_release(query);
    }

    return response;
}
