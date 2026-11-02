/*
 * fw_ids.h — which USB devices need firmware from bluetoothd, and which file.
 * The tables, with their upstream sources, are in fw_ids.c.
 */
#ifndef _BLUETOOTHD_FW_IDS_H_
#define _BLUETOOTHD_FW_IDS_H_

#include <stdint.h>

/* Distinct vendor ids the IOUSBHostDevice watch registers for. */
#define	FW_IDS_MAX_VENDORS	32

enum fw_kind {
	FW_KIND_NONE = 0,
	FW_KIND_INTEL,
	FW_KIND_REALTEK,		/* an OEM VID/PID from rtlbt_list[] */
	FW_KIND_REALTEK_GENERIC		/* VID 0x0bda: check the interface */
};

/* Intel generations, as iwmbtfw's enum iwmbt_device. */
enum fw_intel_gen {
	FW_INTEL_UNKNOWN = 0,
	FW_INTEL_7260,		/* 7260/7265: bseq patch in manufacturer mode */
	FW_INTEL_8260,		/* 8260/8265/9x60 legacy: secure boot, .sfi */
	FW_INTEL_9260		/* TLV version: secure boot, .sfi */
};

/* Realtek ROM LMP subversions (rtlbt_fw.h). */
#define	FW_RTL_LMP_8703B	0x8703
#define	FW_RTL_LMP_8723A	0x1200
#define	FW_RTL_LMP_8723B	0x8723
#define	FW_RTL_LMP_8821A	0x8821
#define	FW_RTL_LMP_8761A	0x8761
#define	FW_RTL_LMP_8822B	0x8822
#define	FW_RTL_LMP_8852A	0x8852
#define	FW_RTL_LMP_8851B	0x8851
#define	FW_RTL_LMP_8922A	0x8922

/* One Realtek chip and its firmware (rtlbt_fw.h struct rtlbt_id_table). */
struct fw_rtl_ic {
	uint16_t	lmp_subversion;
	uint16_t	hci_revision;
	uint8_t		hci_version;
	uint8_t		flags;
#define	FW_RTL_IC_SIMPLE	(0 << 1)
#define	FW_RTL_IC_CONFIG	(1 << 1)	/* a _config.bin is required */
#define	FW_RTL_IC_MSFT		(2 << 1)
	const char	*fw_name;	/* "rtl8761bu" */
	const char	*fw_suffix;	/* NULL: "_fw.bin" */
};

/* What the device is; for Intel, its generation by PID. */
enum fw_kind	fw_ids_lookup(uint16_t vendor, uint16_t product,
		    enum fw_intel_gen *gen);

/* The vendor ids to watch. Returns how many were written. */
int		fw_ids_vendors(uint16_t *out, int max);

/* The Realtek chip a Read_Local_Version describes, or NULL. */
const struct fw_rtl_ic *fw_rtl_ic_lookup(uint16_t lmp_subversion,
		    uint16_t hci_revision, uint8_t hci_version);

#endif /* _BLUETOOTHD_FW_IDS_H_ */
