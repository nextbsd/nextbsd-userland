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
 * fw_rtl.c — Realtek Bluetooth firmware download, in bluetoothd (#202).
 *
 * A port of FreeBSD's usr.sbin/bluetooth/rtlbtfw (releng/15.1): rtlbt_fw.c,
 * rtlbt_hw.c and the download half of main.c, in one file, with the protocol
 * untouched. What changed for NextBSD:
 *
 *   - It runs inside bluetoothd on a worker thread (firmware.c), started by an
 *     IOUSBHostDevice arrival rather than a devd rule; there is no rtlbtfw
 *     binary and no command line.
 *   - The chip table (rtlbt_ic_id_table) and the OEM device list moved to
 *     fw_ids.c, with the Intel ids, so all ids live in one file.
 *   - Firmware comes from NgUbt.kext's Resources (firmware.h).
 *   - Every function is static; fw_rtl_run() at the end is the entry point,
 *     and it frees everything on every path (main() returned early on a
 *     missing firmware file).
 *   - Messages go to bluetoothd's log; the debug lines are off.
 *
 * Bootloader detection is rtlbtfw's: the device needs firmware when ng_ubt
 * has not attached to it (ng_ubt_rtl.c refuses the ROM-only chips), which
 * firmware.c checks before calling in here.
 */
#include "fw_rtl.h"
#include "firmware.h"
#include "fw_ids.h"
#include "hci_compat.h"

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>
#include <sys/queue_mergesort.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

#define	rtlbt_err(fmt, ...)	fw_log("realtek: %s: " fmt, __func__, ##__VA_ARGS__)
#define	rtlbt_info(fmt, ...)	fw_log("realtek: " fmt, ##__VA_ARGS__)
#define	rtlbt_debug(fmt, ...)	do { } while (0)

/* ---- rtlbt_fw.h, rtlbt_hw.h ---------------------------------------------- */


#define RTLBT_PATCH_SNIPPETS		0x01
#define RTLBT_PATCH_DUMMY_HEADER	0x02
#define RTLBT_PATCH_SECURITY_HEADER	0x03

enum rtlbt_fw_type {
	RTLBT_FW_TYPE_UNKNOWN,
	RTLBT_FW_TYPE_V1,
	RTLBT_FW_TYPE_V2,
};

struct rtlbt_firmware {
	char *fwname;
	size_t len;
	unsigned char *buf;
};

SLIST_HEAD(rtlbt_patch_list, rtlbt_patch_entry);

struct rtlbt_patch_entry {
	SLIST_ENTRY(rtlbt_patch_entry) next;
	uint32_t opcode;
	uint32_t len;
	uint8_t prio;
	uint8_t *data;
};

struct rtlbt_iov {
	uint8_t *data;
	uint32_t len;
};

struct rtlbt_fw_header_v1 {
	uint8_t signature[8];
	uint32_t fw_version;
	uint16_t num_patches;
} __attribute__ ((packed));

struct rtlbt_fw_header_v2 {
	uint8_t signature[8];
	uint8_t fw_version[8];
	uint32_t num_sections;
} __attribute__ ((packed));

struct rtlbt_section {
	uint32_t opcode;
	uint32_t len;
	uint8_t data[];
} __attribute__ ((packed));

struct rtlbt_sec_hdr {
	uint16_t num;
	uint16_t reserved;
} __attribute__ ((packed));

struct rtlbt_subsec_hdr {
	uint8_t eco;
	uint8_t prio;
	uint8_t cb[2];
	uint32_t len;
} __attribute__ ((packed));

struct rtlbt_subsec_security_hdr {
	uint8_t eco;
	uint8_t prio;
	uint8_t key_id;
	uint8_t reserved;
	uint32_t len;
} __attribute__ ((packed));

/* USB control request (HCI command) structure */
struct rtlbt_hci_cmd {
	uint16_t	opcode;
	uint8_t		length;
	uint8_t		data[];
} __attribute__ ((packed));

#define RTLBT_HCI_CMD_SIZE(cmd) \
	((cmd)->length + offsetof(struct rtlbt_hci_cmd, data))

/* USB interrupt transfer HCI event header structure */
struct rtlbt_hci_evhdr {
	uint8_t		event;
	uint8_t		length;
} __attribute__ ((packed));

/* USB interrupt transfer (generic HCI event) structure */
struct rtlbt_hci_event {
	struct rtlbt_hci_evhdr	header;
	uint8_t			data[];
} __attribute__ ((packed));

/* USB interrupt transfer (HCI command completion event) structure */
struct rtlbt_hci_event_cmd_compl {
	struct rtlbt_hci_evhdr	header;
	uint8_t			numpkt;
	uint16_t		opcode;
	uint8_t			data[];
} __attribute__ ((packed));

#define RTLBT_HCI_EVT_COMPL_SIZE(payload) \
	(offsetof(struct rtlbt_hci_event_cmd_compl, data) + sizeof(payload))

#define	RTLBT_CONTROL_ENDPOINT_ADDR	0x00
#define	RTLBT_INTERRUPT_ENDPOINT_ADDR	0x81

#define	RTLBT_HCI_MAX_CMD_SIZE		256
#define	RTLBT_HCI_MAX_EVENT_SIZE	16

#define	RTLBT_MSEC2TS(msec)				\
	(struct timespec) {				\
	    .tv_sec = (msec) / 1000,			\
	    .tv_nsec = ((msec) % 1000) * 1000000	\
	};
#define	RTLBT_TS2MSEC(ts)	((ts).tv_sec * 1000 + (ts).tv_nsec / 1000000)
#define	RTLBT_HCI_CMD_TIMEOUT		2000	/* ms */
#define	RTLBT_LOADCMPL_TIMEOUT		5000	/* ms */

#define RTLBT_MAX_CMD_DATA_LEN		252

struct rtlbt_rom_ver_rp {
	uint8_t status;
	uint8_t version;
} __attribute__ ((packed));

struct rtlbt_hci_dl_cmd {
        uint8_t index;
        uint8_t data[RTLBT_MAX_CMD_DATA_LEN];
} __attribute__ ((packed));

struct rtlbt_hci_dl_rp {
        uint8_t status;
        uint8_t index;
} __attribute__ ((packed));

/* Vendor USB request payload */
struct rtlbt_vendor_cmd {
	uint8_t data[5];
}  __attribute__ ((packed));
#define	RTLBT_SEC_PROJ	(&(struct rtlbt_vendor_cmd) {{0x10, 0xA4, 0x0D, 0x00, 0xb0}})

struct rtlbt_vendor_rp {
	uint8_t status;
	uint8_t data[2];
};

/* ---- rtlbt_fw.c, rtlbt_hw.c ---------------------------------------------- */





static const uint16_t project_ids[] = {
	[  0 ] = FW_RTL_LMP_8723A,
	[  1 ] = FW_RTL_LMP_8723B,
	[  2 ] = FW_RTL_LMP_8821A,
	[  3 ] = FW_RTL_LMP_8761A,
	[  7 ] = FW_RTL_LMP_8703B,
	[  8 ] = FW_RTL_LMP_8822B,
	[  9 ] = FW_RTL_LMP_8723B,	/* 8723DU */
	[ 10 ] = FW_RTL_LMP_8821A,	/* 8821CU */
	[ 13 ] = FW_RTL_LMP_8822B,	/* 8822CU */
	[ 14 ] = FW_RTL_LMP_8761A,	/* 8761BU */
	[ 18 ] = FW_RTL_LMP_8852A,	/* 8852AU */
	[ 19 ] = FW_RTL_LMP_8723B,	/* 8723FU */
	[ 20 ] = FW_RTL_LMP_8852A,	/* 8852BU */
	[ 25 ] = FW_RTL_LMP_8852A,	/* 8852CU */
	[ 33 ] = FW_RTL_LMP_8822B,	/* 8822EU */
	[ 36 ] = FW_RTL_LMP_8851B,	/* 8851BU */
	[ 44 ] = FW_RTL_LMP_8922A,	/* 8922A */
	[ 47 ] = FW_RTL_LMP_8852A,	/* 8852BT */
};

/* Signatures */
static const uint8_t fw_header_sig_v1[8] =
    {0x52, 0x65, 0x61, 0x6C, 0x74, 0x65, 0x63, 0x68};	/* Realtech */
static const uint8_t fw_header_sig_v2[8] =
    {0x52, 0x54, 0x42, 0x54, 0x43, 0x6F, 0x72, 0x65};	/* RTBTCore */
static const uint8_t fw_ext_sig[4] = {0x51, 0x04, 0xFD, 0x77};

static int
rtlbt_fw_read(struct rtlbt_firmware *fw, const char *fwname)
{
	int fd;
	struct stat sb;
	unsigned char *buf;
	ssize_t r;

	fd = open(fwname, O_RDONLY);
	if (fd < 0) {
		rtlbt_err("open %s: %s", fwname, strerror(errno));
		return (0);
	}

	if (fstat(fd, &sb) != 0) {
		rtlbt_err("stat %s: %s", fwname, strerror(errno));
		close(fd);
		return (0);
	}

	buf = calloc(1, sb.st_size);
	if (buf == NULL) {
		rtlbt_err("out of memory");
		close(fd);
		return (0);
	}

	/* XXX handle partial reads */
	r = read(fd, buf, sb.st_size);
	if (r < 0) {
		rtlbt_err("read %s: %s", fwname, strerror(errno));
		free(buf);
		close(fd);
		return (0);
	}

	if (r != sb.st_size) {
		rtlbt_err("read len %d != file size %d",
		    (int) r,
		    (int) sb.st_size);
		free(buf);
		close(fd);
		return (0);
	}

	/* We have everything, so! */

	memset(fw, 0, sizeof(*fw));

	fw->fwname = strdup(fwname);
	fw->len = sb.st_size;
	fw->buf = buf;

	close(fd);
	return (1);
}

static void
rtlbt_fw_free(struct rtlbt_firmware *fw)
{
	if (fw->fwname)
		free(fw->fwname);
	if (fw->buf)
		free(fw->buf);
	memset(fw, 0, sizeof(*fw));
}

static char *
rtlbt_get_fwname(const char *fw_name, const char *prefix, const char *suffix)
{
	char *fwname;

	asprintf(&fwname, "%s/%s%s", prefix, fw_name, suffix);

	return (fwname);
}


static enum rtlbt_fw_type
rtlbt_get_fw_type(struct rtlbt_firmware *fw, uint16_t *fw_lmp_subversion)
{
	enum rtlbt_fw_type fw_type;
	size_t fw_header_len;
	uint8_t *ptr;
	uint8_t opcode, oplen, project_id;

	if (fw->len < 8) {
		rtlbt_err("firmware file too small");
		return (RTLBT_FW_TYPE_UNKNOWN);
	}

	if (memcmp(fw->buf, fw_header_sig_v1, sizeof(fw_header_sig_v1)) == 0) {
		fw_type = RTLBT_FW_TYPE_V1;
		fw_header_len = sizeof(struct rtlbt_fw_header_v1);
	} else
	if (memcmp(fw->buf, fw_header_sig_v2, sizeof(fw_header_sig_v2)) == 0) {
		fw_type = RTLBT_FW_TYPE_V2;
		fw_header_len = sizeof(struct rtlbt_fw_header_v2);
	} else
		return (RTLBT_FW_TYPE_UNKNOWN);

	if (fw->len < fw_header_len + sizeof(fw_ext_sig) + 4) {
		rtlbt_err("firmware file too small");
		return (RTLBT_FW_TYPE_UNKNOWN);
	}

	ptr = fw->buf + fw->len - sizeof(fw_ext_sig);
	if (memcmp(ptr, fw_ext_sig, sizeof(fw_ext_sig)) != 0) {
		rtlbt_err("invalid extension section signature");
		return (RTLBT_FW_TYPE_UNKNOWN);
	}

	do {
		opcode = *--ptr;
		oplen = *--ptr;
		ptr -= oplen;

		rtlbt_debug("code=%x len=%x", opcode, oplen);

		if (opcode == 0x00) {
			if (oplen != 1) {
				rtlbt_err("invalid instruction length");
				return (RTLBT_FW_TYPE_UNKNOWN);
			}
			project_id = *ptr;
			rtlbt_debug("project_id=%x", project_id);
			if (project_id >= nitems(project_ids) ||
			    project_ids[project_id] == 0) {
				rtlbt_err("unknown project id %x", project_id);
				return (RTLBT_FW_TYPE_UNKNOWN);
			}
			*fw_lmp_subversion = project_ids[project_id];
			return (fw_type);
		}
	} while (opcode != 0xff && ptr > fw->buf + fw_header_len);

	rtlbt_err("can not find project id instruction");
	return (RTLBT_FW_TYPE_UNKNOWN);
};

static int
rtlbt_parse_fwfile_v1(struct rtlbt_firmware *fw, uint8_t rom_version)
{
	struct rtlbt_fw_header_v1 *fw_header;
	uint8_t *patch_buf;
	unsigned int i;
	const uint8_t *chip_id_base;
	uint32_t patch_offset;
	uint16_t patch_length, num_patches;

	fw_header = (struct rtlbt_fw_header_v1 *)fw->buf;
	num_patches = le16toh(fw_header->num_patches);
	rtlbt_debug("fw_version=%x, num_patches=%d",
	       le32toh(fw_header->fw_version), num_patches);

	/* Find a right patch for the chip. */
	if (fw->len < sizeof(struct rtlbt_fw_header_v1) +
		      sizeof(fw_ext_sig) + 4 + 8 * num_patches) {
		errno = EINVAL;
		return (-1);
	}

	chip_id_base = fw->buf + sizeof(struct rtlbt_fw_header_v1);
	for (i = 0; i < num_patches; i++) {
		if (le16dec(chip_id_base + i * 2) != rom_version + 1)
			continue;
		patch_length = le16dec(chip_id_base + 2 * (num_patches + i));
		patch_offset = le32dec(chip_id_base + 4 * (num_patches + i));
		break;
	}

	if (i >= num_patches) {
		rtlbt_err("can not find patch for chip id %d", rom_version);
		errno = EINVAL;
		return (-1);
	}

	rtlbt_debug(
	    "index=%d length=%x offset=%x", i, patch_length, patch_offset);
	if (fw->len < patch_offset + patch_length) {
		errno = EINVAL;
		return (-1);
	}

	patch_buf = malloc(patch_length);
	if (patch_buf == NULL) {
		errno = ENOMEM;
		return (-1);
	}

	memcpy(patch_buf, fw->buf + patch_offset, patch_length - 4);
	memcpy(patch_buf + patch_length - 4, &fw_header->fw_version, 4);

	free(fw->buf);
	fw->buf = patch_buf;
	fw->len = patch_length;

	return (0);
}

static void *
rtlbt_iov_fetch(struct rtlbt_iov *iov, uint32_t len)
{
	void *data = NULL;

	if (iov->len >= len) {
		data = iov->data;
		iov->data += len;
		iov->len  -= len;
	}

	return (data);
}

static int
rtlbt_patch_entry_cmp(struct rtlbt_patch_entry *a, struct rtlbt_patch_entry *b,
    void *thunk __unused)
{
	return ((a->prio > b->prio) - (a->prio < b->prio));
}

static int
rtlbt_parse_section(struct rtlbt_patch_list *patch_list, uint32_t opcode,
    uint8_t *data, uint32_t len, uint8_t rom_version, uint8_t key_id)
{
	struct rtlbt_sec_hdr *hdr;
	struct rtlbt_patch_entry *entry;
	struct rtlbt_subsec_hdr *subsec_hdr;
	struct rtlbt_subsec_security_hdr *subsec_security_hdr;
	uint16_t num_subsecs;
	uint8_t *subsec_data;
	uint32_t subsec_len;
	int i, sec_len = 0;
	struct rtlbt_iov iov = {
		.data = data,
		.len  = len,
	};

	hdr = rtlbt_iov_fetch(&iov, sizeof(*hdr));
	if (hdr == NULL) {
		errno = EINVAL;
		return (-1);
	}
	num_subsecs = le16toh(hdr->num);

	for (i = 0; i < num_subsecs; i++) {
		subsec_hdr = rtlbt_iov_fetch(&iov, sizeof(*subsec_hdr));
		if (subsec_hdr == NULL)
			break;
		subsec_len = le32toh(subsec_hdr->len);

		rtlbt_debug("subsection, eco 0x%02x, prio 0x%02x, len 0x%x",
		    subsec_hdr->eco, subsec_hdr->prio, subsec_len);

		subsec_data = rtlbt_iov_fetch(&iov, subsec_len);
		if (subsec_data == NULL)
			break;

		if (subsec_hdr->eco == rom_version + 1) {
			if (opcode == RTLBT_PATCH_SECURITY_HEADER) {
				subsec_security_hdr = (void *)subsec_hdr;
				if (subsec_security_hdr->key_id == key_id)
					break;
				continue;
			}

			entry = calloc(1, sizeof(*entry));
			if (entry == NULL) {
				errno = ENOMEM;
				return (-1);
			}
			*entry = (struct rtlbt_patch_entry) {
				.opcode = opcode,
				.len = subsec_len,
				.prio = subsec_hdr->prio,
				.data = subsec_data,
			};
			SLIST_INSERT_HEAD(patch_list, entry, next);
			sec_len += subsec_len;
		}
	}

	return (sec_len);
}

static int
rtlbt_parse_fwfile_v2(struct rtlbt_firmware *fw, uint8_t rom_version,
    uint8_t key_id)
{
	struct rtlbt_fw_header_v2 *hdr;
	struct rtlbt_section *section;
	struct rtlbt_patch_entry *entry;
	uint32_t num_sections;
	uint32_t section_len;
	uint32_t opcode;
	int seclen, len = 0, patch_len = 0;
	uint32_t i;
	uint8_t *section_data, *patch_buf;
	struct rtlbt_patch_list patch_list =
	    SLIST_HEAD_INITIALIZER(patch_list);
	struct rtlbt_iov iov = {
		.data = fw->buf,
		.len = fw->len - 7,
	};

	hdr = rtlbt_iov_fetch(&iov, sizeof(*hdr));
	if (hdr == NULL) {
		errno = EINVAL;
		return (-1);
	}
	num_sections = le32toh(hdr->num_sections);

	rtlbt_debug("FW version %02x%02x%02x%02x-%02x%02x%02x%02x",
	    hdr->fw_version[0], hdr->fw_version[1],
	    hdr->fw_version[2], hdr->fw_version[3],
	    hdr->fw_version[4], hdr->fw_version[5],
	    hdr->fw_version[6], hdr->fw_version[7]);

	for (i = 0; i < num_sections; i++) {
		section = rtlbt_iov_fetch(&iov, sizeof(*section));
		if (section == NULL)
			break;
		section_len = le32toh(section->len);
		opcode = le32toh(section->opcode);

		rtlbt_debug("section, opcode 0x%08x", section->opcode);

		section_data = rtlbt_iov_fetch(&iov, section_len);
		if (section_data == NULL)
			break;

		seclen = 0;
		switch (opcode) {
		case RTLBT_PATCH_SECURITY_HEADER:
			if (key_id == 0)
				break;
			/* FALLTHROUGH */
		case RTLBT_PATCH_SNIPPETS:
		case RTLBT_PATCH_DUMMY_HEADER:
			seclen = rtlbt_parse_section(&patch_list, opcode,
			    section_data, section_len, rom_version, key_id);
			break;
		default:
			break;
		}
		if (seclen < 0) {
			rtlbt_err("Section parse (0x%08x) failed. err %d",
			    opcode, errno);
			return (-1);
		}
		len += seclen;
	}

	if (len == 0) {
		errno = ENOMSG;
		return (-1);
	}

	patch_buf = calloc(1, len);
	if (patch_buf == NULL) {
		errno = ENOMEM;
		return (-1);
	}

	SLIST_MERGESORT(&patch_list, NULL,
	    rtlbt_patch_entry_cmp, rtlbt_patch_entry, next);
	while (!SLIST_EMPTY(&patch_list)) {
		entry = SLIST_FIRST(&patch_list);
		rtlbt_debug("opcode 0x%08x, addr 0x%p, len 0x%x",
			    entry->opcode, entry->data, entry->len);
		memcpy(patch_buf + patch_len, entry->data, entry->len);
		patch_len += entry->len;
		SLIST_REMOVE_HEAD(&patch_list, next);
		free(entry);
	}

	if (patch_len == 0) {
		errno = EPERM;
		return (-1);
	}

	free(fw->buf);
	fw->buf = patch_buf;
	fw->len = patch_len;

	return (0);
}

static int
rtlbt_append_fwfile(struct rtlbt_firmware *fw, struct rtlbt_firmware *opt)
{
	uint8_t *buf;
	int len;

	len = fw->len + opt->len;
	buf = realloc(fw->buf, len);
	if (buf == NULL)
		return (-1);
	memcpy(buf + fw->len, opt->buf, opt->len);
	fw->buf = buf;
	fw->len = len;

	return (0);
}







static int
rtlbt_hci_command(struct libusb_device_handle *hdl, struct rtlbt_hci_cmd *cmd,
    void *event, int size, int *transferred, int timeout)
{
	struct timespec to, now, remains;
	int ret;

	ret = libusb_control_transfer(hdl,
	    LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_DEVICE,
	    0,
	    0,
	    0,
	    (uint8_t *)cmd,
	    RTLBT_HCI_CMD_SIZE(cmd),
	    timeout);

	if (ret < 0) {
		rtlbt_err("libusb_control_transfer() failed: err=%s",
		    libusb_strerror(ret));
		return (ret);
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	to = RTLBT_MSEC2TS(timeout);
	timespecadd(&to, &now, &to);

	do {
		timespecsub(&to, &now, &remains);
		ret = libusb_interrupt_transfer(hdl,
		    RTLBT_INTERRUPT_ENDPOINT_ADDR,
		    event,
		    size,
		    transferred,
		    RTLBT_TS2MSEC(remains) + 1);

		if (ret < 0) {
			rtlbt_err("libusb_interrupt_transfer() failed: err=%s",
			    libusb_strerror(ret));
			return (ret);
		}

		switch (((struct rtlbt_hci_event *)event)->header.event) {
		case NG_HCI_EVENT_COMMAND_COMPL:
			if (*transferred <
			    (int)offsetof(struct rtlbt_hci_event_cmd_compl, data))
				break;
			if (cmd->opcode !=
			    ((struct rtlbt_hci_event_cmd_compl *)event)->opcode)
				break;
			return (0);
		default:
			break;
		}
		rtlbt_debug("Stray HCI event: %x",
		    ((struct rtlbt_hci_event *)event)->header.event);
	} while (timespeccmp(&to, &now, >));

	rtlbt_err("libusb_interrupt_transfer() failed: err=%s",
	    libusb_strerror(LIBUSB_ERROR_TIMEOUT));

	return (LIBUSB_ERROR_TIMEOUT);
}

static int
rtlbt_read_local_ver(struct libusb_device_handle *hdl,
    ng_hci_read_local_ver_rp *ver)
{
	int ret, transferred;
	struct rtlbt_hci_event_cmd_compl *event;
	struct rtlbt_hci_cmd cmd = {
		.opcode = htole16(NG_HCI_OPCODE(NG_HCI_OGF_INFO,
		    NG_HCI_OCF_READ_LOCAL_VER)),
		.length = 0,
	};
	uint8_t buf[RTLBT_HCI_EVT_COMPL_SIZE(ng_hci_read_local_ver_rp)];

	memset(buf, 0, sizeof(buf));

	ret = rtlbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    RTLBT_HCI_CMD_TIMEOUT);

	if (ret < 0 || transferred != sizeof(buf)) {
		 rtlbt_debug("Can't read local version: code=%d, size=%d",
		     ret,
		     transferred);
		 return (-1);
	}

	event = (struct rtlbt_hci_event_cmd_compl *)buf;
	memcpy(ver, event->data, sizeof(ng_hci_read_local_ver_rp));

	return (0);
}

static int
rtlbt_read_rom_ver(struct libusb_device_handle *hdl, uint8_t *ver)
{
	int ret, transferred;
	struct rtlbt_hci_event_cmd_compl *event;
	struct rtlbt_hci_cmd cmd = {
		.opcode = htole16(0xfc6d),
		.length = 0,
	};
	uint8_t buf[RTLBT_HCI_EVT_COMPL_SIZE(struct rtlbt_rom_ver_rp)];

	memset(buf, 0, sizeof(buf));

	ret = rtlbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    RTLBT_HCI_CMD_TIMEOUT);

	if (ret < 0 || transferred != sizeof(buf)) {
		 rtlbt_debug("Can't read ROM version: code=%d, size=%d",
		     ret,
		     transferred);
		 return (-1);
	}

	event = (struct rtlbt_hci_event_cmd_compl *)buf;
	*ver = ((struct rtlbt_rom_ver_rp *)event->data)->version;

	return (0);
}

static int
rtlbt_read_reg16(struct libusb_device_handle *hdl,
    struct rtlbt_vendor_cmd *vcmd, uint8_t *resp)
{
	int ret, transferred;
	struct rtlbt_hci_event_cmd_compl *event;
	uint8_t cmd_buf[offsetof(struct rtlbt_hci_cmd, data) + sizeof(*vcmd)];
	struct rtlbt_hci_cmd *cmd = (struct rtlbt_hci_cmd *)cmd_buf;
	cmd->opcode = htole16(0xfc61);
	cmd->length = sizeof(struct rtlbt_vendor_cmd);
	memcpy(cmd->data, vcmd, sizeof(struct rtlbt_vendor_cmd));
	uint8_t buf[RTLBT_HCI_EVT_COMPL_SIZE(struct rtlbt_vendor_rp)];

	memset(buf, 0, sizeof(buf));

	ret = rtlbt_hci_command(hdl,
	    cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    RTLBT_HCI_CMD_TIMEOUT);

	if (ret < 0 || transferred != sizeof(buf)) {
		 rtlbt_debug("Can't read reg16: code=%d, size=%d",
		     ret,
		     transferred);
		 return (-1);
	}

	event = (struct rtlbt_hci_event_cmd_compl *)buf;
	memcpy(resp, &(((struct rtlbt_vendor_rp *)event->data)->data), 2);

	return (0);
}

static int
rtlbt_load_fwfile(struct libusb_device_handle *hdl,
    const struct rtlbt_firmware *fw)
{
	uint8_t cmd_buf[RTLBT_HCI_MAX_CMD_SIZE];
	struct rtlbt_hci_cmd *cmd = (struct rtlbt_hci_cmd *)cmd_buf;
	struct rtlbt_hci_dl_cmd *dl_cmd = (struct rtlbt_hci_dl_cmd *)cmd->data;
	uint8_t evt_buf[RTLBT_HCI_EVT_COMPL_SIZE(struct rtlbt_hci_dl_rp)];
	uint8_t *data = fw->buf;
	int frag_num = fw->len / RTLBT_MAX_CMD_DATA_LEN + 1;
	int frag_len = RTLBT_MAX_CMD_DATA_LEN;
	int i, j;
	int ret, transferred;

	for (i = 0, j = 0; i < frag_num; i++, j++) {

		rtlbt_debug("download fw (%d/%d)", i + 1, frag_num);

		memset(cmd_buf, 0, sizeof(cmd_buf));
		cmd->opcode = htole16(0xfc20);
		if (j > 0x7f)
			j = 1;
		dl_cmd->index = j;

		if (i == (frag_num - 1)) {
			dl_cmd->index |= 0x80; /* data end */
			frag_len = fw->len % RTLBT_MAX_CMD_DATA_LEN;
		}
		cmd->length = frag_len + 1;
		memcpy(dl_cmd->data, data, frag_len);

		/* Send download command */
		ret = rtlbt_hci_command(hdl,
		    cmd,
		    evt_buf,
		    sizeof(evt_buf),
		    &transferred,
		    RTLBT_HCI_CMD_TIMEOUT);
		if (ret < 0) {
			rtlbt_err("download fw command failed (%d)", errno);
			goto out;
		}
		if (transferred != sizeof(evt_buf)) {
			rtlbt_err("download fw event length mismatch");
			errno = EIO;
			ret = -1;
			goto out;
		}

		data += RTLBT_MAX_CMD_DATA_LEN;
	}

out:
	return (ret);
}

/* ---- main.c -------------------------------------------------------------- */

static int
rtlbt_is_bluetooth(struct libusb_device *dev)
{
	struct libusb_config_descriptor *cfg;
	const struct libusb_interface *ifc;
	const struct libusb_interface_descriptor *d;
	int r;

	r = libusb_get_active_config_descriptor(dev, &cfg);
	if (r < 0) {
		rtlbt_err("Cannot retrieve config descriptor: %s",
		    libusb_error_name(r));
		return (0);
	}

	if (cfg->bNumInterfaces != 0) {
		/* Only 0-th HCI/ACL interface is supported by downloader */
		ifc = &cfg->interface[0];
		if (ifc->num_altsetting != 0) {
			/* BT HCI/ACL interface has no altsettings */
			d = &ifc->altsetting[0];
			/* Check if interface is a bluetooth */
			if (d->bInterfaceClass == LIBUSB_CLASS_WIRELESS &&
			    d->bInterfaceSubClass == 0x01 &&
			    d->bInterfaceProtocol == 0x01) {
				rtlbt_info("found USB Realtek");
				libusb_free_config_descriptor(cfg);
				return (1);
			}
		}
	}
	libusb_free_config_descriptor(cfg);

	/* Not found */
	return (0);
}

static void
rtlbt_dump_version(ng_hci_read_local_ver_rp *ver)
{
	rtlbt_info("hci_version    0x%02x", ver->hci_version);
	rtlbt_info("hci_revision   0x%04x", le16toh(ver->hci_revision));
	rtlbt_info("lmp_version    0x%02x", ver->lmp_version);
	rtlbt_info("lmp_subversion 0x%04x", le16toh(ver->lmp_subversion));
}

/* ---- NextBSD: the entry point (main()'s device half) --------------------- */

bool
fw_rtl_is_bluetooth(libusb_device *dev)
{
	return (rtlbt_is_bluetooth(dev) != 0);
}

int
fw_rtl_run(libusb_device_handle *hdl, const char *firmware_dir)
{
	ng_hci_read_local_ver_rp ver;
	const struct fw_rtl_ic *ic;
	struct rtlbt_firmware fw, cfg;
	enum rtlbt_fw_type fw_type;
	char *firmware_path = NULL, *config_path = NULL;
	const char *fw_suffix;
	uint16_t fw_lmp_subversion;
	uint8_t rom_version;
	int r, ret = FW_RESULT_FAILED;

	(void)memset(&fw, 0, sizeof(fw));

	/* Get local version */
	r = rtlbt_read_local_ver(hdl, &ver);
	if (r < 0) {
		rtlbt_err("rtlbt_read_local_ver() failed code %d", r);
		goto out;
	}
	rtlbt_dump_version(&ver);

	ic = fw_rtl_ic_lookup(le16toh(ver.lmp_subversion),
	    le16toh(ver.hci_revision), ver.hci_version);
	if (ic == NULL) {
		rtlbt_err("Unknown IC");
		goto out;
	}

	fw_suffix = ic->fw_suffix == NULL ? "_fw.bin" : ic->fw_suffix;
	firmware_path = rtlbt_get_fwname(ic->fw_name, firmware_dir, fw_suffix);
	if (firmware_path == NULL)
		goto out;
	rtlbt_info("loading firmware %s", firmware_path);

	/* Read in the firmware */
	if (rtlbt_fw_read(&fw, firmware_path) <= 0)
		goto out;

	fw_type = rtlbt_get_fw_type(&fw, &fw_lmp_subversion);
	if (fw_type == RTLBT_FW_TYPE_UNKNOWN &&
	    (ic->flags & FW_RTL_IC_SIMPLE) == 0) {
		rtlbt_err("Unknown firmware type");
		goto out;
	}

	if (fw_type != RTLBT_FW_TYPE_UNKNOWN) {

		/* Match hardware and firmware lmp_subversion */
		if (fw_lmp_subversion != le16toh(ver.lmp_subversion)) {
			rtlbt_err("firmware is for %x but this is a %x",
			    fw_lmp_subversion, le16toh(ver.lmp_subversion));
			goto out;
		}

		/* Query a ROM version */
		r = rtlbt_read_rom_ver(hdl, &rom_version);
		if (r < 0) {
			rtlbt_err("rtlbt_read_rom_ver() failed code %d", r);
			goto out;
		}

		/* Load in the firmware */
		if (fw_type == RTLBT_FW_TYPE_V2) {
			uint8_t key_id, reg_val[2];

			r = rtlbt_read_reg16(hdl, RTLBT_SEC_PROJ, reg_val);
			if (r < 0) {
				rtlbt_err("rtlbt_read_reg16() failed code %d",
				    r);
				goto out;
			}
			key_id = reg_val[0];
			r = rtlbt_parse_fwfile_v2(&fw, rom_version, key_id);
		} else
			r = rtlbt_parse_fwfile_v1(&fw, rom_version);
		if (r < 0) {
			rtlbt_err("Parsing firmware file failed");
			goto out;
		}

		config_path = rtlbt_get_fwname(ic->fw_name, firmware_dir,
		    "_config.bin");
		if (config_path == NULL)
			goto out;

		rtlbt_info("loading config %s", config_path);

		/* Read in the config file */
		if (rtlbt_fw_read(&cfg, config_path) <= 0) {
			rtlbt_err("rtlbt_fw_read() failed");
			if ((ic->flags & FW_RTL_IC_CONFIG) != 0)
				goto out;
		} else {
			r = rtlbt_append_fwfile(&fw, &cfg);
			rtlbt_fw_free(&cfg);
			if (r < 0) {
				rtlbt_err("Appending config file failed");
				goto out;
			}
		}
	}

	r = rtlbt_load_fwfile(hdl, &fw);
	if (r < 0) {
		rtlbt_err("Loading firmware file failed");
		goto out;
	}

	rtlbt_info("Firmware download complete");

	/* Execute Read Local Version one more time */
	r = rtlbt_read_local_ver(hdl, &ver);
	if (r < 0) {
		rtlbt_err("rtlbt_read_local_ver() failed code %d", r);
		goto out;
	}
	rtlbt_dump_version(&ver);
	ret = FW_RESULT_DOWNLOADED;

out:
	rtlbt_fw_free(&fw);
	free(firmware_path);
	free(config_path);
	return (ret);
}
