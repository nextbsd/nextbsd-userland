/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 Adrian Chadd <adrian@freebsd.org>
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright (c) 2023 Future Crew LLC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * fw_ids.c — every device id and firmware name bluetoothd's firmware loader
 * (#202) knows, in one place, so each FreeBSD import can be re-diffed here.
 *
 * Sources, releng/15.1:
 *   usr.sbin/bluetooth/iwmbtfw/iwmbtfw.conf   the devd rule's Intel PIDs
 *   usr.sbin/bluetooth/iwmbtfw/main.c         iwmbt_list[]: PID -> generation
 *   usr.sbin/bluetooth/rtlbtfw/rtlbtfw.conf   the devd rules: VID 0x0bda on an
 *                                             e0/01/01 interface 0, plus OEMs
 *   usr.sbin/bluetooth/rtlbtfw/main.c         rtlbt_list[]: the OEM VID/PIDs
 *   usr.sbin/bluetooth/rtlbtfw/rtlbt_fw.c     rtlbt_ic_id_table[]: chip ->
 *                                             firmware file name
 *
 * NextBSD has no devd: these tables are matched against IOUSBHostDevice
 * arrivals (iokit_watch.c) instead of devd's USB ATTACH events. Intel firmware
 * names are computed from the controller's version, so they have no table;
 * see fw_intel.c.
 */
#include "fw_ids.h"

#include <stddef.h>

#define	nitems_(x)	(sizeof(x) / sizeof((x)[0]))

#define	FW_VENDOR_INTEL		0x8087
#define	FW_VENDOR_REALTEK	0x0bda

/* iwmbtfw main.c iwmbt_list[], identical to iwmbtfw.conf's product match. */
static const struct {
	uint16_t		product;
	enum fw_intel_gen	gen;
} fw_intel_ids[] = {
	/* Intel Wireless 7260/7265 and successors */
	{ 0x07dc, FW_INTEL_7260 },
	{ 0x0a2a, FW_INTEL_7260 },
	{ 0x0aa7, FW_INTEL_7260 },

	/* Intel Wireless 8260/8265 and successors (ThinkPad T460s: 0a2b) */
	{ 0x0a2b, FW_INTEL_8260 },
	{ 0x0aaa, FW_INTEL_8260 },
	{ 0x0025, FW_INTEL_8260 },
	{ 0x0026, FW_INTEL_8260 },
	{ 0x0029, FW_INTEL_8260 },

	/* Intel Wireless 9260/9560 and successors */
	{ 0x0032, FW_INTEL_9260 },
	{ 0x0033, FW_INTEL_9260 },
	{ 0x0035, FW_INTEL_9260 },
	{ 0x0036, FW_INTEL_9260 },
};

/*
 * rtlbtfw main.c rtlbt_list[]: OEM devices with Realtek chips. Realtek's own
 * VID 0x0bda is matched by its interface class instead (fw_rtl.c checks for
 * e0/01/01 on interface 0, as rtlbtfw.conf does).
 */
static const struct {
	uint16_t	vendor;
	uint16_t	product;
} fw_rtl_oem_ids[] = {
	/* Realtek 8821CE Bluetooth devices */
	{ 0x13d3, 0x3529 },

	/* Realtek 8851BE Bluetooth devices */
	{ 0x13d3, 0x3600 },

	/* Realtek 8852AE Bluetooth devices */
	{ 0x04c5, 0x165c },
	{ 0x04ca, 0x4006 },
	{ 0x0cb8, 0xc549 },

	/* Realtek 8852CE Bluetooth devices */
	{ 0x04ca, 0x4007 },
	{ 0x04c5, 0x1675 },
	{ 0x0cb8, 0xc558 },
	{ 0x13d3, 0x3587 },
	{ 0x13d3, 0x3586 },
	{ 0x13d3, 0x3592 },
	{ 0x13d3, 0x3612 },
	{ 0x0489, 0xe122 },

	/* Realtek 8852BE Bluetooth devices */
	{ 0x0cb8, 0xc559 },
	{ 0x13d3, 0x3570 },
	{ 0x13d3, 0x3571 },
	{ 0x13d3, 0x3572 },
	{ 0x13d3, 0x3591 },
	{ 0x0489, 0xe123 },
	{ 0x0489, 0xe125 },

	/* Realtek 8922AE Bluetooth devices */
	{ 0x13d3, 0x3617 },
	{ 0x13d3, 0x3616 },
	{ 0x0489, 0xe130 },

	/* Realtek 8723AE Bluetooth devices */
	{ 0x0930, 0x021d },
	{ 0x13d3, 0x3394 },

	/* Realtek 8723BE Bluetooth devices */
	{ 0x0489, 0xe085 },
	{ 0x0489, 0xe08b },
	{ 0x04f2, 0xb49f },
	{ 0x13d3, 0x3410 },
	{ 0x13d3, 0x3416 },
	{ 0x13d3, 0x3459 },
	{ 0x13d3, 0x3494 },

	/* Realtek 8723BU Bluetooth devices */
	{ 0x7392, 0xa611 },

	/* Realtek 8723DE Bluetooth devices */
	{ 0x2ff8, 0xb011 },

	/* Realtek 8761BUV Bluetooth devices */
	{ 0x2c4e, 0x0115 },
	{ 0x2357, 0x0604 },
	{ 0x0b05, 0x190e },
	{ 0x2550, 0x8761 },
	{ 0x6655, 0x8771 },
	{ 0x7392, 0xc611 },
	{ 0x2b89, 0x8761 },

	/* Realtek 8821AE Bluetooth devices */
	{ 0x0b05, 0x17dc },
	{ 0x13d3, 0x3414 },
	{ 0x13d3, 0x3458 },
	{ 0x13d3, 0x3461 },
	{ 0x13d3, 0x3462 },

	/* Realtek 8822BE Bluetooth devices */
	{ 0x13d3, 0x3526 },
	{ 0x0b05, 0x185c },

	/* Realtek 8822CE Bluetooth devices */
	{ 0x04ca, 0x4005 },
	{ 0x04c5, 0x161f },
	{ 0x0b05, 0x18ef },
	{ 0x13d3, 0x3548 },
	{ 0x13d3, 0x3549 },
	{ 0x13d3, 0x3553 },
	{ 0x13d3, 0x3555 },
	{ 0x2ff8, 0x3051 },
	{ 0x1358, 0xc123 },
	{ 0x0cb5, 0xc547 },
};

/* rtlbtfw rtlbt_fw.c rtlbt_ic_id_table[]. */
static const struct fw_rtl_ic fw_rtl_ics[] = {
	{ /* 8723A */
	    .lmp_subversion = FW_RTL_LMP_8723A,
	    .hci_revision = 0xb,
	    .hci_version = 0x6,
	    .flags = FW_RTL_IC_SIMPLE,
	    .fw_name = "rtl8723a",
	}, { /* 8723B */
	    .lmp_subversion = FW_RTL_LMP_8723B,
	    .hci_revision = 0xb,
	    .hci_version = 0x6,
	    .fw_name = "rtl8723b",
	}, { /* 8723D */
	    .lmp_subversion = FW_RTL_LMP_8723B,
	    .hci_revision = 0xd,
	    .hci_version = 0x8,
	    .flags = FW_RTL_IC_CONFIG,
	    .fw_name = "rtl8723d",
	}, { /* 8821A */
	    .lmp_subversion = FW_RTL_LMP_8821A,
	    .hci_revision = 0xa,
	    .hci_version = 0x6,
	    .fw_name = "rtl8821a",
	}, { /* 8821C */
	    .lmp_subversion = FW_RTL_LMP_8821A,
	    .hci_revision = 0xc,
	    .hci_version = 0x8,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name = "rtl8821c",
	}, { /* 8761A */
	    .lmp_subversion = FW_RTL_LMP_8761A,
	    .hci_revision = 0xa,
	    .hci_version = 0x6,
	    .fw_name = "rtl8761a",
	}, { /* 8761BU */
	    .lmp_subversion = FW_RTL_LMP_8761A,
	    .hci_revision = 0xb,
	    .hci_version = 0xa,
	    .fw_name = "rtl8761bu",
	}, { /* 8822C with USB interface */
	    .lmp_subversion = FW_RTL_LMP_8822B,
	    .hci_revision = 0xc,
	    .hci_version = 0xa,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name = "rtl8822cu",
	}, { /* 8822B */
	    .lmp_subversion = FW_RTL_LMP_8822B,
	    .hci_revision = 0xb,
	    .hci_version = 0x7,
	    .flags = FW_RTL_IC_CONFIG | FW_RTL_IC_MSFT,
	    .fw_name = "rtl8822b",
	}, { /* 8852A */
	    .lmp_subversion = FW_RTL_LMP_8852A,
	    .hci_revision = 0xa,
	    .hci_version = 0xb,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name = "rtl8852au",
	}, { /* 8852B */
	    .lmp_subversion = FW_RTL_LMP_8852A,
	    .hci_revision = 0xb,
	    .hci_version = 0xb,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name = "rtl8852bu",
	}, { /* 8852C */
	    .lmp_subversion = FW_RTL_LMP_8852A,
	    .hci_revision = 0xc,
	    .hci_version = 0xc,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name  = "rtl8852cu",
	    .fw_suffix = "_fw_v2.bin",
	}, { /* 8851B */
	    .lmp_subversion = FW_RTL_LMP_8851B,
	    .hci_revision = 0xb,
	    .hci_version = 0xc,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name  = "rtl8851bu",
	}, { /* 8922A */
	    .lmp_subversion = FW_RTL_LMP_8922A,
	    .hci_revision = 0xa,
	    .hci_version = 0xc,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name  = "rtl8922au",
	}, { /* 8852BT/8852BE-VT */
	    .lmp_subversion = FW_RTL_LMP_8852A,
	    .hci_revision = 0x87,
	    .hci_version = 0xc,
	    .flags = FW_RTL_IC_MSFT,
	    .fw_name  = "rtl8852btu",
	},
};

enum fw_kind
fw_ids_lookup(uint16_t vendor, uint16_t product, enum fw_intel_gen *gen)
{
	size_t i;

	*gen = FW_INTEL_UNKNOWN;
	if (vendor == FW_VENDOR_INTEL) {
		for (i = 0; i < nitems_(fw_intel_ids); i++) {
			if (fw_intel_ids[i].product == product) {
				*gen = fw_intel_ids[i].gen;
				return (FW_KIND_INTEL);
			}
		}
		return (FW_KIND_NONE);
	}
	if (vendor == FW_VENDOR_REALTEK)
		return (FW_KIND_REALTEK_GENERIC);
	for (i = 0; i < nitems_(fw_rtl_oem_ids); i++) {
		if (fw_rtl_oem_ids[i].vendor == vendor &&
		    fw_rtl_oem_ids[i].product == product)
			return (FW_KIND_REALTEK);
	}
	return (FW_KIND_NONE);
}

int
fw_ids_vendors(uint16_t *out, int max)
{
	size_t i;
	int j, n = 0;

	if (n < max)
		out[n++] = FW_VENDOR_INTEL;
	if (n < max)
		out[n++] = FW_VENDOR_REALTEK;
	for (i = 0; i < nitems_(fw_rtl_oem_ids); i++) {
		for (j = 0; j < n; j++) {
			if (out[j] == fw_rtl_oem_ids[i].vendor)
				break;
		}
		if (j == n && n < max)
			out[n++] = fw_rtl_oem_ids[i].vendor;
	}
	return (n);
}

const struct fw_rtl_ic *
fw_rtl_ic_lookup(uint16_t lmp_subversion, uint16_t hci_revision,
    uint8_t hci_version)
{
	size_t i;

	for (i = 0; i < nitems_(fw_rtl_ics); i++) {
		if (fw_rtl_ics[i].lmp_subversion == lmp_subversion &&
		    fw_rtl_ics[i].hci_revision == hci_revision &&
		    fw_rtl_ics[i].hci_version == hci_version)
			return (&fw_rtl_ics[i]);
	}
	return (NULL);
}
