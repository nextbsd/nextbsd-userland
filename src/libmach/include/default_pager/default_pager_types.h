/*
 * default_pager/default_pager_types.h — macx_triggers() flags
 * (nextbsd/nextbsd-userland#178).
 *
 * The user-space half of the kernel's <sys/mach/default_pager_types.h>
 * (nextbsd-kernel#218). Darwin installs the same names as
 * <default_pager/default_pager_types.h> (xnu-2422
 * osfmk/default_pager/default_pager_types.h), and dynamic_pager includes
 * it from there; the flag values are Darwin's.
 *
 * macx_triggers(hi_water, low_water, flags, alert_port) registers
 * alert_port for HI_WAT_ALERT and/or LO_WAT_ALERT. The kernel then sends it
 * one default_pager_space_alert (subsystem default_pager_alerts, msgh_id
 * 2295, generated from default_pager_alerts.defs) per registration:
 *
 *	HI_WAT_ALERT	free swap fell below vm.nswap_lowat (grow swap)
 *	LO_WAT_ALERT	free swap rose back above vm.nswap_hiwat (swap may
 *			shrink)
 *
 * SWAP_FILE_CREATION_ERROR is accepted and its port released, as in xnu.
 * The encryption, compaction, PROC_RESUME and emergency-swap-file flags
 * belong to xnu's default pager; NextBSD accepts them and does nothing.
 */
#ifndef _MACH_DEFAULT_PAGER_TYPES_H_
#define _MACH_DEFAULT_PAGER_TYPES_H_

#define	HI_WAT_ALERT			0x01
#define	LO_WAT_ALERT			0x02
#define	SWAP_ENCRYPT_ON			0x04
#define	SWAP_ENCRYPT_OFF		0x08
#define	SWAP_COMPACT_DISABLE		0x10
#define	SWAP_COMPACT_ENABLE		0x20
#define	PROC_RESUME			0x40
#define	SWAP_FILE_CREATION_ERROR	0x80
#define	USE_EMERGENCY_SWAP_FILE_FIRST	0x100

/* default_pager_alerts subsystem base; default_pager_space_alert is +0. */
#define	DEFAULT_PAGER_SPACE_ALERT_MSGID	2295

#endif /* _MACH_DEFAULT_PAGER_TYPES_H_ */
