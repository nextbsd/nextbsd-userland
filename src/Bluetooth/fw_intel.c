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
 * fw_intel.c — Intel Bluetooth firmware download, in bluetoothd (#202).
 *
 * A port of FreeBSD's usr.sbin/bluetooth/iwmbtfw (releng/15.1): iwmbt_fw.c,
 * iwmbt_hw.c and the identify/handle_* half of main.c, in one file, with the
 * protocol untouched. What changed for NextBSD:
 *
 *   - It runs inside bluetoothd on a worker thread (firmware.c), started by an
 *     IOUSBHostDevice arrival rather than a devd rule; there is no iwmbtfw
 *     binary and no command line.
 *   - Firmware comes from NgUbt.kext's Resources (firmware.h).
 *   - Every function is static; fw_intel_run() at the end is the entry point.
 *   - Messages go to bluetoothd's log; the per-fragment debug lines are off.
 *   - The TLV parser's assert()s return an error instead: a malformed reply
 *     must not abort the daemon.
 *   - "Already downloaded" is FW_RESULT_OPERATIONAL, not 0, so firmware.c
 *     can tell a download from a controller that needed none.
 *   - A controller that does not answer identify at all (a USB timeout) is
 *     FW_RESULT_NO_REPLY, so firmware.c can reset it and try once more
 *     (#539); any other failure is FW_RESULT_FAILED.
 *
 * Generations (fw_ids.c has the PIDs):
 *   7260/7265  manufacturer mode, a .bseq patch (ibt-hw-*.bseq)
 *   8260/8265  legacy version (0xfc05), secure boot, ibt-<variant>-<revid>.sfi
 *              then .ddc. The ThinkPad T460s's 8087:0a2b is hw_variant 0x0b,
 *              dev_revid 5: ibt-11-5.sfi and ibt-11-5.ddc.
 *   9260+      TLV version, secure boot (RSA or ECDSA), ibt-<cnvi>-<cnvr>.sfi
 * The generation is read from the controller (iwmbt_identify), the PID only
 * seeds it.
 */
#include "fw_intel.h"
#include "firmware.h"
#include "hci_compat.h"

#include <sys/param.h>
#include <sys/endian.h>
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

#define	iwmbt_err(fmt, ...)	fw_log("intel: %s: " fmt, __func__, ##__VA_ARGS__)
#define	iwmbt_info(fmt, ...)	fw_log("intel: " fmt, ##__VA_ARGS__)
#define	iwmbt_debug(fmt, ...)	do { } while (0)

/* iwmbt_parse_tlv's assert(len == n), as an error return. */
#define	TLV_LEN(n)	do { if (len != (n)) return (-1); } while (0)

enum iwmbt_device {
	IWMBT_DEVICE_UNKNOWN,
	IWMBT_DEVICE_7260,
	IWMBT_DEVICE_8260,
	IWMBT_DEVICE_9260,
};

/* ---- iwmbt_fw.h ---------------------------------------------------------- */

#define	RSA_HEADER_LEN		644
#define	ECDSA_HEADER_LEN	320
#define	ECDSA_OFFSET		RSA_HEADER_LEN
#define	CSS_HEADER_OFFSET	8

struct iwmbt_version {
	uint8_t status;
	uint8_t hw_platform;
	uint8_t hw_variant;
	uint8_t hw_revision;
	uint8_t fw_variant;
	uint8_t fw_revision;
	uint8_t fw_build_num;
	uint8_t fw_build_ww;
	uint8_t fw_build_yy;
	uint8_t fw_patch_num;
} __attribute__ ((packed));

/* Known values for fw_variant */
#define	FW_VARIANT_BOOTLOADER	0x06 /* Bootloader mode */
#define	FW_VARIANT_OPERATIONAL	0x23 /* Operational mode */

struct iwmbt_boot_params {
	uint8_t status;
	uint8_t otp_format;
	uint8_t otp_content;
	uint8_t otp_patch;
	uint16_t dev_revid;
	uint8_t secure_boot;
	uint8_t key_from_hdr;
	uint8_t key_type;
	uint8_t otp_lock;
	uint8_t api_lock;
	uint8_t debug_lock;
	uint8_t otp_bdaddr[6];
	uint8_t min_fw_build_nn;
	uint8_t min_fw_build_cw;
	uint8_t min_fw_build_yy;
	uint8_t limited_cce;
	uint8_t unlocked_state;
} __attribute__ ((packed));

enum {
	IWMBT_TLV_CNVI_TOP = 0x10,
	IWMBT_TLV_CNVR_TOP,
	IWMBT_TLV_CNVI_BT,
	IWMBT_TLV_CNVR_BT,
	IWMBT_TLV_CNVI_OTP,
	IWMBT_TLV_CNVR_OTP,
	IWMBT_TLV_DEV_REV_ID,
	IWMBT_TLV_USB_VENDOR_ID,
	IWMBT_TLV_USB_PRODUCT_ID,
	IWMBT_TLV_PCIE_VENDOR_ID,
	IWMBT_TLV_PCIE_DEVICE_ID,
	IWMBT_TLV_PCIE_SUBSYSTEM_ID,
	IWMBT_TLV_IMAGE_TYPE,
	IWMBT_TLV_TIME_STAMP,
	IWMBT_TLV_BUILD_TYPE,
	IWMBT_TLV_BUILD_NUM,
	IWMBT_TLV_FW_BUILD_PRODUCT,
	IWMBT_TLV_FW_BUILD_HW,
	IWMBT_TLV_FW_STEP,
	IWMBT_TLV_BT_SPEC,
	IWMBT_TLV_MFG_NAME,
	IWMBT_TLV_HCI_REV,
	IWMBT_TLV_LMP_SUBVER,
	IWMBT_TLV_OTP_PATCH_VER,
	IWMBT_TLV_SECURE_BOOT,
	IWMBT_TLV_KEY_FROM_HDR,
	IWMBT_TLV_OTP_LOCK,
	IWMBT_TLV_API_LOCK,
	IWMBT_TLV_DEBUG_LOCK,
	IWMBT_TLV_MIN_FW,
	IWMBT_TLV_LIMITED_CCE,
	IWMBT_TLV_SBE_TYPE,
	IWMBT_TLV_OTP_BDADDR,
	IWMBT_TLV_UNLOCKED_STATE
};

struct iwmbt_version_tlv {
	uint32_t cnvi_top;
	uint32_t cnvr_top;
	uint32_t cnvi_bt;
	uint32_t cnvr_bt;
	uint16_t dev_rev_id;
	uint8_t img_type;
	uint16_t timestamp;
	uint8_t build_type;
	uint32_t build_num;
	uint8_t secure_boot;
	uint8_t otp_lock;
	uint8_t api_lock;
	uint8_t debug_lock;
	uint8_t min_fw_build_nn;
	uint8_t min_fw_build_cw;
	uint8_t min_fw_build_yy;
	uint8_t limited_cce;
	uint8_t sbe_type;
	bdaddr_t otp_bd_addr;
};

/* Known TLV img_type values */
#define	TLV_IMG_TYPE_BOOTLOADER		0x01 /* Bootloader mode */
#define	TLV_IMG_TYPE_OPERATIONAL	0x03 /* Operational mode */

struct iwmbt_firmware {
	char *fwname;
	int len;
	unsigned char *buf;
};

/* ---- iwmbt_hw.h ---------------------------------------------------------- */

/* USB control request (HCI command) structure */
struct iwmbt_hci_cmd {
	uint16_t	opcode;
	uint8_t		length;
	uint8_t		data[];
} __attribute__ ((packed));

#define IWMBT_HCI_CMD_SIZE(cmd) \
	((cmd)->length + offsetof(struct iwmbt_hci_cmd, data))

/* USB interrupt transfer HCI event header structure */
struct iwmbt_hci_evhdr {
	uint8_t		event;
	uint8_t		length;
} __attribute__ ((packed));

/* USB interrupt transfer (generic HCI event) structure */
struct iwmbt_hci_event {
	struct iwmbt_hci_evhdr	header;
	uint8_t			data[];
} __attribute__ ((packed));

/* USB interrupt transfer (HCI command completion event) structure */
struct iwmbt_hci_event_cmd_compl {
	struct iwmbt_hci_evhdr	header;
	uint8_t			numpkt;
	uint16_t		opcode;
	uint8_t			data[];
} __attribute__ ((packed));

/*
 * Manufacturer mode exit type: selects reset type,
 * 0x00: simply exit manufacturer mode without a reset.
 * 0x01: exit manufacturer mode with a reset and patches disabled
 * 0x02: exit manufacturer mode with a reset and patches enabled
 */
enum iwmbt_mm_exit {
	IWMBT_MM_EXIT_ONLY = 0x00,
	IWMBT_MM_EXIT_COLD_RESET = 0x01,
	IWMBT_MM_EXIT_WARM_RESET = 0x02,
};

#define IWMBT_HCI_EVT_COMPL_SIZE(payload) \
	(offsetof(struct iwmbt_hci_event_cmd_compl, data) + sizeof(payload))
#define	IWMBT_HCI_EVENT_COMPL_HEAD_SIZE \
	(offsetof(struct iwmbt_hci_event_cmd_compl, data) - \
	 offsetof(struct iwmbt_hci_event_cmd_compl, numpkt))

#define	IWMBT_CONTROL_ENDPOINT_ADDR	0x00
#define	IWMBT_INTERRUPT_ENDPOINT_ADDR	0x81
#define	IWMBT_BULK_IN_ENDPOINT_ADDR	0x82
#define	IWMBT_BULK_OUT_ENDPOINT_ADDR	0x02

#define	IWMBT_HCI_MAX_CMD_SIZE		256
#define	IWMBT_HCI_MAX_EVENT_SIZE	16

#define	IWMBT_MSEC2TS(msec)				\
	(struct timespec) {				\
	    .tv_sec = (msec) / 1000,			\
	    .tv_nsec = ((msec) % 1000) * 1000000	\
	};
#define	IWMBT_TS2MSEC(ts)	((ts).tv_sec * 1000 + (ts).tv_nsec / 1000000)
#define	IWMBT_HCI_CMD_TIMEOUT		2000	/* ms */
#define	IWMBT_LOADCMPL_TIMEOUT		5000	/* ms */

/* ---- iwmbt_fw.c, iwmbt_hw.c, main.c -------------------------------------- */





static int
iwmbt_fw_read(struct iwmbt_firmware *fw, const char *fwname)
{
	int fd;
	struct stat sb;
	unsigned char *buf;
	ssize_t r;

	fd = open(fwname, O_RDONLY);
	if (fd < 0) {
		iwmbt_err("open %s: %s", fwname, strerror(errno));
		return (0);
	}

	if (fstat(fd, &sb) != 0) {
		iwmbt_err("stat %s: %s", fwname, strerror(errno));
		close(fd);
		return (0);
	}

	buf = calloc(1, sb.st_size);
	if (buf == NULL) {
		iwmbt_err("out of memory");
		close(fd);
		return (0);
	}

	/* XXX handle partial reads */
	r = read(fd, buf, sb.st_size);
	if (r < 0) {
		iwmbt_err("read %s: %s", fwname, strerror(errno));
		free(buf);
		close(fd);
		return (0);
	}

	if (r != sb.st_size) {
		iwmbt_err("read len %d != file size %d",
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
iwmbt_fw_free(struct iwmbt_firmware *fw)
{
	if (fw->fwname)
		free(fw->fwname);
	if (fw->buf)
		free(fw->buf);
	memset(fw, 0, sizeof(*fw));
}

static char *
iwmbt_get_fwname(struct iwmbt_version *ver, struct iwmbt_boot_params *params,
    const char *prefix, const char *suffix)
{
	struct stat sb;
	char *fwname;

	switch (ver->hw_variant) {
	case 0x07:	/* 7260 */
	case 0x08:	/* 7265 */
		// NB: don't use params, they are NULL for 7xxx
		asprintf(&fwname, "%s/ibt-hw-%x.%x.%x-fw-%x.%x.%x.%x.%x.%s",
		    prefix,
		    le16toh(ver->hw_platform),
		    le16toh(ver->hw_variant),
		    le16toh(ver->hw_revision),
		    le16toh(ver->fw_variant),
		    le16toh(ver->fw_revision),
		    le16toh(ver->fw_build_num),
		    le16toh(ver->fw_build_ww),
		    le16toh(ver->fw_build_yy),
		    suffix);
		/*
		 * Fallback to the default firmware patch if
		 * the correct firmware patch file is not found.
		 */
		if (stat(fwname, &sb) != 0 && errno == ENOENT) {
			free(fwname);
			asprintf(&fwname, "%s/ibt-hw-%x.%x.%s",
			    prefix,
			    le16toh(ver->hw_platform),
			    le16toh(ver->hw_variant),
			    suffix);
		}
		break;

	case 0x0b:	/* 8260 */
	case 0x0c:	/* 8265 */
		asprintf(&fwname, "%s/ibt-%u-%u.%s",
		    prefix,
		    le16toh(ver->hw_variant),
		    le16toh(params->dev_revid),
		    suffix);
		break;

	case 0x11:	/* 9560 */
	case 0x12:	/* 9260 */
	case 0x13:
	case 0x14:	/* 22161 */
		asprintf(&fwname, "%s/ibt-%u-%u-%u.%s",
		    prefix,
		    le16toh(ver->hw_variant),
		    le16toh(ver->hw_revision),
		    le16toh(ver->fw_revision),
		    suffix);
		break;

	default:
		fwname = NULL;
	}

	return (fwname);
}

static char *
iwmbt_get_fwname_tlv(struct iwmbt_version_tlv *ver, const char *prefix,
    const char *suffix)
{
	char *fwname;

#define	IWMBT_PACK_CNVX_TOP(cnvx_top)	((uint16_t)(	\
	((cnvx_top) & 0x0f000000) >> 16 |		\
	((cnvx_top) & 0x0000000f) << 12 |		\
	((cnvx_top) & 0x00000ff0) >> 4))

	asprintf(&fwname, "%s/ibt-%04x-%04x.%s",
	    prefix,
	    IWMBT_PACK_CNVX_TOP(ver->cnvi_top),
	    IWMBT_PACK_CNVX_TOP(ver->cnvr_top),
	    suffix);

	return (fwname);
}

static int
iwmbt_parse_tlv(uint8_t *data, uint8_t datalen,
    struct iwmbt_version_tlv *version)
{
	uint8_t status, type, len;

	status = *data++;
	if (status != 0)
		return (-1);
	datalen--;

	while (datalen >= 2) {
		type = *data++;
		len = *data++;
		datalen -= 2;

		if (datalen < len)
			return (-1);

		switch (type) {
		case IWMBT_TLV_CNVI_TOP:
			TLV_LEN(4);
			version->cnvi_top = le32dec(data);
			break;
		case IWMBT_TLV_CNVR_TOP:
			TLV_LEN(4);
			version->cnvr_top = le32dec(data);
			break;
		case IWMBT_TLV_CNVI_BT:
			TLV_LEN(4);
			version->cnvi_bt = le32dec(data);
			break;
		case IWMBT_TLV_CNVR_BT:
			TLV_LEN(4);
			version->cnvr_bt = le32dec(data);
			break;
		case IWMBT_TLV_DEV_REV_ID:
			TLV_LEN(2);
			version->dev_rev_id = le16dec(data);
			break;
		case IWMBT_TLV_IMAGE_TYPE:
			TLV_LEN(1);
			version->img_type = *data;
			break;
		case IWMBT_TLV_TIME_STAMP:
			TLV_LEN(2);
			version->min_fw_build_cw = data[0];
			version->min_fw_build_yy = data[1];
			version->timestamp = le16dec(data);
			break;
		case IWMBT_TLV_BUILD_TYPE:
			TLV_LEN(1);
			version->build_type = *data;
			break;
		case IWMBT_TLV_BUILD_NUM:
			TLV_LEN(4);
			version->min_fw_build_nn = *data;
			version->build_num = le32dec(data);
			break;
		case IWMBT_TLV_SECURE_BOOT:
			TLV_LEN(1);
			version->secure_boot = *data;
			break;
		case IWMBT_TLV_OTP_LOCK:
			TLV_LEN(1);
			version->otp_lock = *data;
			break;
		case IWMBT_TLV_API_LOCK:
			TLV_LEN(1);
			version->api_lock = *data;
			break;
		case IWMBT_TLV_DEBUG_LOCK:
			TLV_LEN(1);
			version->debug_lock = *data;
			break;
		case IWMBT_TLV_MIN_FW:
			TLV_LEN(3);
			version->min_fw_build_nn = data[0];
			version->min_fw_build_cw = data[1];
			version->min_fw_build_yy = data[2];
			break;
		case IWMBT_TLV_LIMITED_CCE:
			TLV_LEN(1);
			version->limited_cce = *data;
			break;
		case IWMBT_TLV_SBE_TYPE:
			TLV_LEN(1);
			version->sbe_type = *data;
			break;
		case IWMBT_TLV_OTP_BDADDR:
			memcpy(&version->otp_bd_addr, data, sizeof(bdaddr_t));
			break;
		default:
			/* Ignore other types */
			break;
		}

		datalen -= len;
		data += len;
	}

	return (0);
}







#define	XMIN(x, y)	((x) < (y) ? (x) : (y))

static int
iwmbt_send_fragment(struct libusb_device_handle *hdl,
    uint8_t fragment_type, const void *data, uint8_t len, int timeout)
{
	int ret, transferred;
	uint8_t buf[IWMBT_HCI_MAX_CMD_SIZE];
	struct iwmbt_hci_cmd *cmd = (struct iwmbt_hci_cmd *) buf;

	memset(buf, 0, sizeof(buf));
	cmd->opcode = htole16(0xfc09),
	cmd->length = len + 1,
	cmd->data[0] = fragment_type;
	memcpy(cmd->data + 1, data, len);

	ret = libusb_bulk_transfer(hdl,
	    IWMBT_BULK_OUT_ENDPOINT_ADDR,
	    (uint8_t *)cmd,
	    IWMBT_HCI_CMD_SIZE(cmd),
	    &transferred,
	    timeout);

	if (ret < 0 || transferred != (int)IWMBT_HCI_CMD_SIZE(cmd)) {
		iwmbt_err("libusb_bulk_transfer() failed: err=%s, size=%zu",
		    libusb_strerror(ret),
		    IWMBT_HCI_CMD_SIZE(cmd));
		return (-1);
	}

	ret = libusb_bulk_transfer(hdl,
	    IWMBT_BULK_IN_ENDPOINT_ADDR,
	    buf,
	    sizeof(buf),
	    &transferred,
	    timeout);

	if (ret < 0) {
		iwmbt_err("libusb_bulk_transfer() failed: err=%s",
		    libusb_strerror(ret));
		return (-1);
	}

	return (0);
}

static int
iwmbt_hci_command(struct libusb_device_handle *hdl, struct iwmbt_hci_cmd *cmd,
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
	    IWMBT_HCI_CMD_SIZE(cmd),
	    timeout);

	if (ret < 0) {
		iwmbt_err("libusb_control_transfer() failed: err=%s",
		    libusb_strerror(ret));
		return (ret);
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	to = IWMBT_MSEC2TS(timeout);
	timespecadd(&to, &now, &to);

	do {
		timespecsub(&to, &now, &remains);
		ret = libusb_interrupt_transfer(hdl,
		    IWMBT_INTERRUPT_ENDPOINT_ADDR,
		    event,
		    size,
		    transferred,
		    IWMBT_TS2MSEC(remains) + 1);

		if (ret < 0) {
			iwmbt_err("libusb_interrupt_transfer() failed: err=%s",
			    libusb_strerror(ret));
			return (ret);
		}

		switch (((struct iwmbt_hci_event *)event)->header.event) {
		case NG_HCI_EVENT_COMMAND_COMPL:
			if (*transferred <
			    (int)offsetof(struct iwmbt_hci_event_cmd_compl, data))
				break;
			if (cmd->opcode !=
			    ((struct iwmbt_hci_event_cmd_compl *)event)->opcode)
				break;
			/* FALLTHROUGH */
		case 0xFF:
			return (0);
		default:
			break;
		}
		iwmbt_debug("Stray HCI event: %x",
		    ((struct iwmbt_hci_event *)event)->header.event);
	} while (timespeccmp(&to, &now, >));

	iwmbt_err("libusb_interrupt_transfer() failed: err=%s",
	    libusb_strerror(LIBUSB_ERROR_TIMEOUT));

	return (LIBUSB_ERROR_TIMEOUT);
}

static int
iwmbt_patch_fwfile(struct libusb_device_handle *hdl,
    const struct iwmbt_firmware *fw)
{
	int ret, transferred;
	struct iwmbt_firmware fw_job = *fw;
	uint16_t cmd_opcode;
	uint8_t cmd_length;
	struct iwmbt_hci_cmd *cmd_buf;
	uint8_t evt_code;
	uint8_t evt_length;
	uint8_t evt_buf[IWMBT_HCI_MAX_EVENT_SIZE];
	int activate_patch = 0;

	while (fw_job.len > 0) {
		if (fw_job.len < 4) {
			iwmbt_err("Invalid firmware, unexpected EOF in HCI "
			    "command header. Remains=%d", fw_job.len);
			return (-1);
		}

		if (fw_job.buf[0] != 0x01) {
			iwmbt_err("Invalid firmware, expected HCI command (%d)",
					fw_job.buf[0]);
			return (-1);
		}

		/* Advance by one. */
		fw_job.buf++;
		fw_job.len--;

		/* Load in the HCI command to perform. */
		cmd_opcode = le16dec(fw_job.buf);
		cmd_length = fw_job.buf[2];
		cmd_buf = (struct iwmbt_hci_cmd *)fw_job.buf;

		iwmbt_debug("opcode=%04x, len=%02x", cmd_opcode, cmd_length);

		/*
		 * If there is a command that loads a patch in the
		 * firmware file, then activate the patch upon success,
		 * otherwise just disable the manufacturer mode.
		 */
		if (cmd_opcode == 0xfc8e)
			activate_patch = 1;

		/* Advance by three. */
		fw_job.buf += 3;
		fw_job.len -= 3;

		if (fw_job.len < cmd_length) {
			iwmbt_err("Invalid firmware, unexpected EOF in HCI "
			    "command data. len=%d, remains=%d",
			    cmd_length, fw_job.len);
			return (-1);
		}

		/* Advance by data length. */
		fw_job.buf += cmd_length;
		fw_job.len -= cmd_length;

		ret = libusb_control_transfer(hdl,
		    LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_DEVICE,
		    0,
		    0,
		    0,
		    (uint8_t *)cmd_buf,
		    IWMBT_HCI_CMD_SIZE(cmd_buf),
		    IWMBT_HCI_CMD_TIMEOUT);

		if (ret < 0) {
			iwmbt_err("libusb_control_transfer() failed: err=%s",
			    libusb_strerror(ret));
			return (-1);
		}

		/*
		 * Every command has its associated event: data must match
		 * what is recorded in the firmware file. Perform that check
		 * now.
		 */

		while (fw_job.len > 0 && fw_job.buf[0] == 0x02) {
			/* Is this the end of the file? */
			if (fw_job.len < 3) {
				iwmbt_err("Invalid firmware, unexpected EOF in"
				    "event header. remains=%d", fw_job.len);
				return (-1);
			}

			/* Advance by one. */
			fw_job.buf++;
			fw_job.len--;

			/* Load in the HCI event. */
			evt_code = fw_job.buf[0];
			evt_length = fw_job.buf[1];

			/* Advance by two. */
			fw_job.buf += 2;
			fw_job.len -= 2;

			/* Prepare HCI event buffer. */
			memset(evt_buf, 0, IWMBT_HCI_MAX_EVENT_SIZE);

			iwmbt_debug("event=%04x, len=%02x",
					evt_code, evt_length);

			if (fw_job.len < evt_length) {
				iwmbt_err("Invalid firmware, unexpected EOF in"
				    " event data. len=%d, remains=%d",
				    evt_length, fw_job.len);
				return (-1);
			}

			ret = libusb_interrupt_transfer(hdl,
			    IWMBT_INTERRUPT_ENDPOINT_ADDR,
			    evt_buf,
			    IWMBT_HCI_MAX_EVENT_SIZE,
			    &transferred,
			    IWMBT_HCI_CMD_TIMEOUT);

			if (ret < 0) {
				iwmbt_err("libusb_interrupt_transfer() failed:"
				    " err=%s", libusb_strerror(ret));
				return (-1);
			}

			if ((int)evt_length + 2 != transferred ||
			    memcmp(evt_buf + 2, fw_job.buf, evt_length) != 0) {
				iwmbt_err("event does not match firmware");
				return (-1);
			}

			/* Advance by data length. */
			fw_job.buf += evt_length;
			fw_job.len -= evt_length;
		}
	}

	return (activate_patch);
}

#define	IWMBT_SEND_FRAGMENT(fragment_type, size, msg)	do {		\
	iwmbt_debug("transferring %d bytes, offset %d", size, sent);	\
									\
	ret = iwmbt_send_fragment(hdl,					\
	    fragment_type,						\
	    fw->buf + sent,						\
	    XMIN(size, fw->len - sent),					\
	    IWMBT_HCI_CMD_TIMEOUT);					\
									\
	if (ret < 0) {							\
		iwmbt_debug("Failed to send "msg": code=%d", ret);	\
		return (-1);						\
	}								\
	sent += size;							\
} while (0)

static int
iwmbt_load_rsa_header(struct libusb_device_handle *hdl,
    const struct iwmbt_firmware *fw)
{
	int ret, sent = 0;

	IWMBT_SEND_FRAGMENT(0x00, 0x80, "CCS segment");
	IWMBT_SEND_FRAGMENT(0x03, 0x80, "public key / part 1");
	IWMBT_SEND_FRAGMENT(0x03, 0x80, "public key / part 2");

	/* skip 4 bytes */
	sent += 4;

	IWMBT_SEND_FRAGMENT(0x02, 0x80, "signature / part 1");
	IWMBT_SEND_FRAGMENT(0x02, 0x80, "signature / part 2");

	return (0);
}

static int
iwmbt_load_ecdsa_header(struct libusb_device_handle *hdl,
    const struct iwmbt_firmware *fw)
{
	int ret, sent = ECDSA_OFFSET;

	IWMBT_SEND_FRAGMENT(0x00, 0x80, "CCS segment");
	IWMBT_SEND_FRAGMENT(0x03, 0x60, "public key");
	IWMBT_SEND_FRAGMENT(0x02, 0x60, "signature");

	return (0);
}

static int
iwmbt_load_fwfile(struct libusb_device_handle *hdl,
    const struct iwmbt_firmware *fw, uint32_t *boot_param, int offset)
{
	int ready = 0, sent = offset;
	int ret, transferred;
	struct iwmbt_hci_cmd *cmd;
	struct iwmbt_hci_event *event;
	uint8_t buf[IWMBT_HCI_MAX_EVENT_SIZE];

	/*
	 * Send firmware chunks. Chunk len must be 4 byte aligned.
	 * multiple commands can be combined
	 */
	while (fw->len - sent - ready >= (int) sizeof(struct iwmbt_hci_cmd)) {
		cmd = (struct iwmbt_hci_cmd *)(fw->buf + sent + ready);
		/* Parse firmware for Intel Reset HCI command parameter */
		if (cmd->opcode == htole16(0xfc0e)) {
			*boot_param = le32dec(cmd->data);
			iwmbt_debug("boot_param=0x%08x", *boot_param);
		}
		ready += IWMBT_HCI_CMD_SIZE(cmd);
		while (ready >= 0xFC) {
			IWMBT_SEND_FRAGMENT(0x01, 0xFC, "firmware chunk");
			ready -= 0xFC;
		}
		if (ready > 0 && ready % 4 == 0) {
			IWMBT_SEND_FRAGMENT(0x01, ready, "firmware chunk");
			ready = 0;
		}
	}

	/* Wait for firmware download completion event */
	ret = libusb_interrupt_transfer(hdl,
	    IWMBT_INTERRUPT_ENDPOINT_ADDR,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_LOADCMPL_TIMEOUT);

	if (ret < 0 || transferred < (int)sizeof(struct iwmbt_hci_event) + 1) {
		iwmbt_err("libusb_interrupt_transfer() failed: "
		    "err=%s, size=%d",
		    libusb_strerror(ret),
		    transferred);
		return (-1);
	}

	/* Expect Vendor Specific Event 0x06 */
	event = (struct iwmbt_hci_event *)buf;
	if (event->header.event != 0xFF || event->data[0] != 0x06) {
		iwmbt_err("firmware download completion event missed");
		return (-1);
	}

	return (0);
}

static int
iwmbt_bt_reset(struct libusb_device_handle *hdl)
{
	int ret, transferred;
	static struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0x0c03),
		.length = 0,
	};
	uint8_t buf[IWMBT_HCI_MAX_EVENT_SIZE];

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0)
		 iwmbt_debug("HCI reset command failed: code=%d", ret);

	return (ret);
}

static int
iwmbt_enter_manufacturer(struct libusb_device_handle *hdl)
{
	int ret, transferred;
	static struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0xfc11),
		.length = 2,
		.data = { 0x01, 0x00 },
	};
	uint8_t buf[IWMBT_HCI_MAX_EVENT_SIZE];

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0) {
		 iwmbt_debug("Can't enter manufacturer mode: code=%d, size=%d",
		     ret,
		     transferred);
		 return (-1);
	}

	return (0);
}

static int
iwmbt_exit_manufacturer(struct libusb_device_handle *hdl,
    enum iwmbt_mm_exit mode)
{
	int ret, transferred;
	static struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0xfc11),
		.length = 2,
		.data = { 0x00, 0x00 },
	};
	uint8_t buf[IWMBT_HCI_MAX_EVENT_SIZE];

	cmd.data[1] = (uint8_t)mode;

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0) {
		 iwmbt_debug("Can't exit manufacturer mode: code=%d, size=%d",
		     ret,
		     transferred);
		 return (-1);
	}

	return (0);
}

static int
iwmbt_get_version(struct libusb_device_handle *hdl,
    struct iwmbt_version *version)
{
	int ret, transferred;
	struct iwmbt_hci_event_cmd_compl*event;
	struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0xfc05),
		.length = 0,
	};
	uint8_t buf[IWMBT_HCI_EVT_COMPL_SIZE(struct iwmbt_version)];

	memset(buf, 0, sizeof(buf));

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0 || transferred != sizeof(buf)) {
		 iwmbt_debug("Can't get version: : code=%d, size=%d",
		     ret,
		     transferred);
		 return (-1);
	}

	event = (struct iwmbt_hci_event_cmd_compl *)buf;
	memcpy(version, event->data, sizeof(struct iwmbt_version));

	return (0);
}

static int
iwmbt_read_version_tlv(struct libusb_device_handle *hdl,
    uint8_t *data, uint8_t *datalen)
{
	int ret, transferred;
	struct iwmbt_hci_event_cmd_compl *event;
	static struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0xfc05),
		.length = 1,
		.data = { 0xff },
	};
	uint8_t buf[255];

	memset(buf, 0, sizeof(buf));

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0 || transferred < (int)IWMBT_HCI_EVT_COMPL_SIZE(uint16_t)) {
		 iwmbt_debug("Can't get version: code=%d, size=%d",
		     ret,
		     transferred);
		 /* NextBSD: the libusb error, so a timeout can be told apart. */
		 return (ret < 0 ? ret : -1);
	}

	event = (struct iwmbt_hci_event_cmd_compl *)buf;
	*datalen = event->header.length - IWMBT_HCI_EVENT_COMPL_HEAD_SIZE;
	memcpy(data, event->data, *datalen);

	return (0);
}

static int
iwmbt_get_version_tlv(struct libusb_device_handle *hdl,
    struct iwmbt_version_tlv *version)
{

	uint8_t data[255];
	uint8_t datalen;
	int ret;

	memset(data, 0, sizeof(data));

	ret = iwmbt_read_version_tlv(hdl, data, &datalen);
	if (ret < 0) {
		 iwmbt_debug("Can't get version tlv");
		 return (-1);
	}

	return (iwmbt_parse_tlv(data, datalen, version));
}

static int
iwmbt_get_boot_params(struct libusb_device_handle *hdl,
    struct iwmbt_boot_params *params)
{
	int ret, transferred = 0;
	struct iwmbt_hci_event_cmd_compl *event;
	struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0xfc0d),
		.length = 0,
	};
	uint8_t buf[IWMBT_HCI_EVT_COMPL_SIZE(struct iwmbt_boot_params)];

	memset(buf, 0, sizeof(buf));

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0 || transferred != sizeof(buf)) {
		 iwmbt_debug("Can't get boot params: code=%d, size=%d",
		     ret,
		     transferred);
		 return (-1);
	}

	event = (struct iwmbt_hci_event_cmd_compl *)buf;
	memcpy(params, event->data, sizeof(struct iwmbt_boot_params));

	return (0);
}

static int
iwmbt_intel_reset(struct libusb_device_handle *hdl, uint32_t boot_param)
{
	int ret, transferred = 0;
	struct iwmbt_hci_event *event;
	static struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0xfc01),
		.length = 8,
		.data = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 },
	};
	uint8_t buf[IWMBT_HCI_MAX_EVENT_SIZE];

	le32enc(cmd.data + 4, boot_param);
	memset(buf, 0, sizeof(buf));

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0 || transferred < (int)sizeof(struct iwmbt_hci_event) + 1) {
		 iwmbt_debug("Intel Reset command failed: code=%d, size=%d",
		    ret,
		    transferred);
		 return (ret);
	}

	/* expect Vendor Specific Event 0x02 */
	event = (struct iwmbt_hci_event *)buf;
	if (event->header.event != 0xFF || event->data[0] != 0x02) {
		iwmbt_err("Intel Reset completion event missed");
		return (-1);
	}

	return (0);
}

static int
iwmbt_load_ddc(struct libusb_device_handle *hdl,
    const struct iwmbt_firmware *ddc)
{
	int size, sent = 0;
	int ret, transferred;
	uint8_t buf[IWMBT_HCI_MAX_CMD_SIZE];
	uint8_t evt[IWMBT_HCI_MAX_CMD_SIZE];
	struct iwmbt_hci_cmd *cmd = (struct iwmbt_hci_cmd *)buf;

	size = ddc->len;

	iwmbt_debug("file=%s, size=%d", ddc->fwname, size);

	while (size > 0) {

		memset(buf, 0, sizeof(buf));
		cmd->opcode = htole16(0xfc8b);
		cmd->length = ddc->buf[sent] + 1;
		memcpy(cmd->data, ddc->buf + sent, XMIN(ddc->buf[sent], size));

		iwmbt_debug("transferring %d bytes, offset %d",
		    cmd->length,
		    sent);

		size -= cmd->length;
		sent += cmd->length;

		ret = iwmbt_hci_command(hdl,
		    cmd,
		    evt,
		    sizeof(evt),
		    &transferred,
		    IWMBT_HCI_CMD_TIMEOUT);

		if (ret < 0) {
			 iwmbt_debug("Intel Write DDC failed: code=%d", ret);
			 return (-1);
		}
	}

	return (0);
}

static int
iwmbt_set_event_mask(struct libusb_device_handle *hdl)
{
	int ret, transferred = 0;
	static struct iwmbt_hci_cmd cmd = {
		.opcode = htole16(0xfc52),
		.length = 8,
		.data = { 0x87, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	};
	uint8_t buf[IWMBT_HCI_MAX_EVENT_SIZE];

	ret = iwmbt_hci_command(hdl,
	    &cmd,
	    buf,
	    sizeof(buf),
	    &transferred,
	    IWMBT_HCI_CMD_TIMEOUT);

	if (ret < 0)
		 iwmbt_debug("Intel Set Event Mask failed: code=%d", ret);

	return (ret);
}
static void
iwmbt_dump_version(struct iwmbt_version *ver)
{
	iwmbt_info("status       0x%02x", ver->status);
	iwmbt_info("hw_platform  0x%02x", ver->hw_platform);
	iwmbt_info("hw_variant   0x%02x", ver->hw_variant);
	iwmbt_info("hw_revision  0x%02x", ver->hw_revision);
	iwmbt_info("fw_variant   0x%02x", ver->fw_variant);
	iwmbt_info("fw_revision  0x%02x", ver->fw_revision);
	iwmbt_info("fw_build_num 0x%02x", ver->fw_build_num);
	iwmbt_info("fw_build_ww  0x%02x", ver->fw_build_ww);
	iwmbt_info("fw_build_yy  0x%02x", ver->fw_build_yy);
	iwmbt_info("fw_patch_num 0x%02x", ver->fw_patch_num);
}

static void
iwmbt_dump_boot_params(struct iwmbt_boot_params *params)
{
	iwmbt_info("Device revision: %u", le16toh(params->dev_revid));
	iwmbt_info("Secure Boot:  %s", params->secure_boot ? "on" : "off");
	iwmbt_info("OTP lock:     %s", params->otp_lock    ? "on" : "off");
	iwmbt_info("API lock:     %s", params->api_lock    ? "on" : "off");
	iwmbt_info("Debug lock:   %s", params->debug_lock  ? "on" : "off");
	iwmbt_info("Minimum firmware build %u week %u year %u",
	    params->min_fw_build_nn,
	    params->min_fw_build_cw,
	    2000 + params->min_fw_build_yy);
	iwmbt_info("OTC BD_ADDR:  %02x:%02x:%02x:%02x:%02x:%02x",
	    params->otp_bdaddr[5],
	    params->otp_bdaddr[4],
	    params->otp_bdaddr[3],
	    params->otp_bdaddr[2],
	    params->otp_bdaddr[1],
	    params->otp_bdaddr[0]);
}

static void
iwmbt_dump_version_tlv(struct iwmbt_version_tlv *ver)
{
	iwmbt_info("cnvi_top     0x%08x", ver->cnvi_top);
	iwmbt_info("cnvr_top     0x%08x", ver->cnvr_top);
	iwmbt_info("cnvi_bt      0x%08x", ver->cnvi_bt);
	iwmbt_info("cnvr_bt      0x%08x", ver->cnvr_bt);
	iwmbt_info("dev_rev_id   0x%04x", ver->dev_rev_id);
	iwmbt_info("img_type     0x%02x", ver->img_type);
	iwmbt_info("timestamp    0x%04x", ver->timestamp);
	iwmbt_info("build_type   0x%02x", ver->build_type);
	iwmbt_info("build_num    0x%08x", ver->build_num);
	iwmbt_info("Secure Boot:  %s", ver->secure_boot ? "on" : "off");
	iwmbt_info("OTP lock:     %s", ver->otp_lock    ? "on" : "off");
	iwmbt_info("API lock:     %s", ver->api_lock    ? "on" : "off");
	iwmbt_info("Debug lock:   %s", ver->debug_lock  ? "on" : "off");
	iwmbt_info("Minimum firmware build %u week %u year %u",
	    ver->min_fw_build_nn,
	    ver->min_fw_build_cw,
	    2000 + ver->min_fw_build_yy);
	iwmbt_info("limited_cce  0x%02x", ver->limited_cce);
	iwmbt_info("sbe_type     0x%02x", ver->sbe_type);
	iwmbt_info("OTC BD_ADDR:  %02x:%02x:%02x:%02x:%02x:%02x",
	    ver->otp_bd_addr.b[5],
	    ver->otp_bd_addr.b[4],
	    ver->otp_bd_addr.b[3],
	    ver->otp_bd_addr.b[2],
	    ver->otp_bd_addr.b[1],
	    ver->otp_bd_addr.b[0]);
	if (ver->img_type == TLV_IMG_TYPE_BOOTLOADER ||
	    ver->img_type == TLV_IMG_TYPE_OPERATIONAL)
		iwmbt_info("%s timestamp %u.%u buildtype %u build %u",
		    (ver->img_type == TLV_IMG_TYPE_BOOTLOADER ?
		    "Bootloader" : "Firmware"),
		    2000 + (ver->timestamp >> 8),
		    ver->timestamp & 0xff,
		    ver->build_type,
		    ver->build_num);
}

/*
 * NextBSD: `noreply` is set when the controller did not answer at all (a USB
 * timeout), rather than answering with something unsupported.
 */
static enum iwmbt_device
iwmbt_identify(libusb_device_handle *hdl, enum iwmbt_device device,
    bool *noreply)
{
	uint8_t data[255];
	uint8_t datalen, hw_platform, hw_variant;
	struct iwmbt_version *ver = (struct iwmbt_version *)data;
	struct iwmbt_version_tlv ver_tlv;
	int r;

	if (device == IWMBT_DEVICE_7260) {
		r = iwmbt_bt_reset(hdl);
		if (r < 0) {
			iwmbt_debug("iwmbt_bt_reset() failed!");
			*noreply = (r == LIBUSB_ERROR_TIMEOUT);
			return (IWMBT_DEVICE_UNKNOWN);
		}
	}

	memset(data, 0, sizeof(data));
	r = iwmbt_read_version_tlv(hdl, data, &datalen);
	if (r < 0) {
		iwmbt_debug("iwmbt_read_version_tlv() failed");
		*noreply = (r == LIBUSB_ERROR_TIMEOUT);
		return (IWMBT_DEVICE_UNKNOWN);
	}

	if (datalen == sizeof(*ver) && ver->hw_platform == 0x37) {
		switch (ver->hw_variant) {
		case 0x07:
		case 0x08:
			return (IWMBT_DEVICE_7260);
		case 0x0b:
		case 0x0c:
		case 0x11:
		case 0x12:
		case 0x13:
		case 0x14:
			return (IWMBT_DEVICE_8260);
		default:
			iwmbt_debug("Unsupported hw_variant (0x%2x)",
			    ver->hw_variant);
			return (IWMBT_DEVICE_UNKNOWN);
		}
	}

	r = iwmbt_parse_tlv(data, datalen, &ver_tlv);
	if (r < 0) {
		iwmbt_debug("iwmbt_parse_tlv() failed");
		return (IWMBT_DEVICE_UNKNOWN);
	}

	hw_platform = (ver_tlv.cnvi_bt >> 8) & 0xff;
	hw_variant = (ver_tlv.cnvi_bt >> 16) & 0x3f;

	if (hw_platform != 0x37) {
		iwmbt_debug("Unsupported hw_platform (0x%2x)", hw_platform);
		return (IWMBT_DEVICE_UNKNOWN);
	}

	return (hw_variant < 0x17 ? IWMBT_DEVICE_8260 : IWMBT_DEVICE_9260);
}

static int
iwmbt_init_firmware(libusb_device_handle *hdl, const char *firmware_path,
    uint32_t *boot_param, uint8_t hw_variant, uint8_t sbe_type)
{
	struct iwmbt_firmware fw;
	int header_len, ret = -1;

	iwmbt_debug("loading %s", firmware_path);

	/* Read in the firmware */
	if (iwmbt_fw_read(&fw, firmware_path) <= 0) {
		iwmbt_debug("iwmbt_fw_read() failed");
		return (-1);
	}

	iwmbt_debug("Firmware file size=%d", fw.len);

	if (hw_variant <= 0x14) {
		/*
		 * Hardware variants 0x0b, 0x0c, 0x11 - 0x14 .sfi file have
		 * a RSA header of 644 bytes followed by Command Buffer.
		 */
		header_len = RSA_HEADER_LEN;
		if (fw.len < header_len) {
			iwmbt_err("Invalid size of firmware file (%d)", fw.len);
			ret = -1;
			goto exit;
		}

		/* Check if the CSS Header version is RSA(0x00010000) */
		if (le32dec(fw.buf + CSS_HEADER_OFFSET) != 0x00010000) {
			iwmbt_err("Invalid CSS Header version");
			ret = -1;
			goto exit;
		}

		/* Only RSA secure boot engine supported */
		if (sbe_type != 0x00) {
			iwmbt_err("Invalid SBE type for hardware variant (%d)",
			    hw_variant);
			ret = -1;
			goto exit;
		}

	} else if (hw_variant >= 0x17) {
		/*
		 * Hardware variants 0x17, 0x18 onwards support both RSA and
		 * ECDSA secure boot engine. As a result, the corresponding sfi
		 * file will have RSA header of 644, ECDSA header of 320 bytes
		 * followed by Command Buffer.
		 */
		header_len = ECDSA_OFFSET + ECDSA_HEADER_LEN;
		if (fw.len < header_len) {
			iwmbt_err("Invalid size of firmware file (%d)", fw.len);
			ret = -1;
			goto exit;
		}

		/* Check if CSS header for ECDSA follows the RSA header */
		if (fw.buf[ECDSA_OFFSET] != 0x06) {
			ret = -1;
			goto exit;
		}

		/* Check if the CSS Header version is ECDSA(0x00020000) */
		if (le32dec(fw.buf + ECDSA_OFFSET + CSS_HEADER_OFFSET) != 0x00020000) {
			iwmbt_err("Invalid CSS Header version");
			ret = -1;
			goto exit;
		}
	}

	/* Load in the CSS header */
	if (sbe_type == 0x00)
		ret = iwmbt_load_rsa_header(hdl, &fw);
	else if (sbe_type == 0x01)
		ret = iwmbt_load_ecdsa_header(hdl, &fw);
	if (ret < 0)
		goto exit;

	/* Load in the Command Buffer */
	ret = iwmbt_load_fwfile(hdl, &fw, boot_param, header_len);

exit:
	/* free firmware */
	iwmbt_fw_free(&fw);

	return (ret);
}

static int
iwmbt_init_ddc(libusb_device_handle *hdl, const char *ddc_path)
{
	struct iwmbt_firmware ddc;
	int ret;

	iwmbt_debug("loading %s", ddc_path);

	/* Read in the DDC file */
	if (iwmbt_fw_read(&ddc, ddc_path) <= 0) {
		iwmbt_debug("iwmbt_fw_read() failed");
		return (-1);
	}

	/* Load in the DDC file */
	ret = iwmbt_load_ddc(hdl, &ddc);
	if (ret < 0)
		iwmbt_debug("Loading DDC file failed");

	/* free it */
	iwmbt_fw_free(&ddc);

	return (ret);
}

/*
 * Returns 0 on success.
 */
static int
handle_7260(libusb_device_handle *hdl, char *firmware_dir)
{
	int r;
	char *firmware_path;
	struct iwmbt_version ver;
	struct iwmbt_firmware fw;

	r = iwmbt_get_version(hdl, &ver);
	if (r < 0) {
		iwmbt_debug("iwmbt_get_version() failed code %d", r);
		return 1;
	}
	iwmbt_dump_version(&ver);
	iwmbt_debug("fw_patch_num=0x%02x", (int) ver.fw_patch_num);

	/* fw_patch_num = >0 operational mode */
	if (ver.fw_patch_num > 0x00) {
		iwmbt_info("Firmware has already been downloaded");
		return (FW_RESULT_OPERATIONAL);
	}

	firmware_path = iwmbt_get_fwname(&ver, NULL, firmware_dir, "bseq");
	if (firmware_path == NULL)
		return 1;
	iwmbt_debug("firmware_path = %s", firmware_path);

	r = iwmbt_fw_read(&fw, firmware_path);
	free(firmware_path);
	if (r <= 0) {
		iwmbt_debug("iwmbt_fw_read() failed");
		return 1;
	}

	r = iwmbt_enter_manufacturer(hdl);
	if (r < 0) {
		iwmbt_debug("iwmbt_enter_manufacturer() failed code %d", r);
		iwmbt_fw_free(&fw);
		return 1;
	}

	/* Download firmware */
	r = iwmbt_patch_fwfile(hdl, &fw);
	iwmbt_fw_free(&fw);
	if (r < 0) {
		iwmbt_debug("Loading firmware file failed");
		(void)iwmbt_exit_manufacturer(hdl, IWMBT_MM_EXIT_COLD_RESET);
		return 1;
	}

	iwmbt_info("Firmware download complete");

	r = iwmbt_exit_manufacturer(hdl,
	    (r == 0 ? IWMBT_MM_EXIT_ONLY : IWMBT_MM_EXIT_WARM_RESET));
	if (r < 0) {
		iwmbt_debug("iwmbt_exit_manufacturer() failed code %d", r);
		return 1;
	}

	/* Once device is running in operational mode we can ignore failures */

	/* Dump actual controller version */
	r = iwmbt_get_version(hdl, &ver);
	if (r == 0)
		iwmbt_dump_version(&ver);

	if (iwmbt_enter_manufacturer(hdl) < 0)
		return 0;
	r = iwmbt_set_event_mask(hdl);
	if (r == 0)
		iwmbt_info("Intel Event Mask is set");
	(void)iwmbt_exit_manufacturer(hdl, IWMBT_MM_EXIT_ONLY);

	return 0;
}


/*
 * Returns 0 on success.
 */
static int
handle_8260(libusb_device_handle *hdl, char *firmware_dir)
{
	int r;
	uint32_t boot_param;
	struct iwmbt_version ver;
	struct iwmbt_boot_params params;
	char *firmware_path = NULL;

	r = iwmbt_get_version(hdl, &ver);
	if (r < 0) {
		iwmbt_debug("iwmbt_get_version() failed code %d", r);
		return 1;
	}
	iwmbt_dump_version(&ver);
	iwmbt_debug("fw_variant=0x%02x", (int) ver.fw_variant);

	if (ver.fw_variant == FW_VARIANT_OPERATIONAL) {
		iwmbt_info("Firmware has already been downloaded");
		return (FW_RESULT_OPERATIONAL);
	}

	if (ver.fw_variant != FW_VARIANT_BOOTLOADER){
		iwmbt_err("unknown fw_variant 0x%02x", (int) ver.fw_variant);
		return 1;
	}

	/* Read Intel Secure Boot Params */
	r = iwmbt_get_boot_params(hdl, &params);
	if (r < 0) {
		iwmbt_debug("iwmbt_get_boot_params() failed!");
		return 1;
	}
	iwmbt_dump_boot_params(&params);

	/* Check if firmware fragments are ACKed with a cmd complete event */
	if (params.limited_cce != 0x00) {
		iwmbt_err("Unsupported Intel firmware loading method (%u)",
		   params.limited_cce);
		return 1;
	}

	firmware_path = iwmbt_get_fwname(&ver, &params, firmware_dir, "sfi");
	if (firmware_path == NULL)
		return 1;
	iwmbt_debug("firmware_path = %s", firmware_path);

	/* Download firmware and parse it for magic Intel Reset parameter */
	r = iwmbt_init_firmware(hdl, firmware_path, &boot_param, 0, 0);
	free(firmware_path);
	if (r < 0)
		return 1;

	iwmbt_info("Firmware download complete");

	r = iwmbt_intel_reset(hdl, boot_param);
	if (r < 0) {
		iwmbt_debug("iwmbt_intel_reset() failed!");
		return 1;
	}

	iwmbt_info("Firmware operational");

	/* Once device is running in operational mode we can ignore failures */

	/* Dump actual controller version */
	r = iwmbt_get_version(hdl, &ver);
	if (r == 0)
		iwmbt_dump_version(&ver);

	/* Apply the device configuration (DDC) parameters */
	firmware_path = iwmbt_get_fwname(&ver, &params, firmware_dir, "ddc");
	iwmbt_debug("ddc_path = %s", firmware_path);
	if (firmware_path != NULL) {
		r = iwmbt_init_ddc(hdl, firmware_path);
		if (r == 0)
			iwmbt_info("DDC download complete");
		free(firmware_path);
	}

	r = iwmbt_set_event_mask(hdl);
	if (r == 0)
		iwmbt_info("Intel Event Mask is set");

	return 0;
}


static int
handle_9260(libusb_device_handle *hdl, char *firmware_dir)
{
	int r;
	uint32_t boot_param;
	struct iwmbt_version_tlv vt;
	char *firmware_path = NULL;

	r = iwmbt_get_version_tlv(hdl, &vt);
	if (r < 0) {
		iwmbt_debug("iwmbt_get_version_tlv() failed code %d", r);
		return 1;
	}
	iwmbt_dump_version_tlv(&vt);
	iwmbt_debug("img_type=0x%02x", (int) vt.img_type);

	if (vt.img_type == TLV_IMG_TYPE_OPERATIONAL) {
		iwmbt_info("Firmware has already been downloaded");
		return (FW_RESULT_OPERATIONAL);
	}

	if (vt.img_type != TLV_IMG_TYPE_BOOTLOADER) {
		iwmbt_err("unknown img_type 0x%02x", (int) vt.img_type);
		return 1;
	}

	/* Check if firmware fragments are ACKed with a cmd complete event */
	if (vt.limited_cce != 0x00) {
		iwmbt_err("Unsupported Intel firmware loading method (%u)",
		   vt.limited_cce);
		return 1;
	}

	/* Check if secure boot engine is supported: 1 (ECDSA) or 0 (RSA) */
	if (vt.sbe_type > 0x01) {
		iwmbt_err("Unsupported secure boot engine (%u)",
		   vt.sbe_type);
		return 1;
	}

	firmware_path = iwmbt_get_fwname_tlv(&vt, firmware_dir, "sfi");
	if (firmware_path == NULL)
		return 1;
	iwmbt_debug("firmware_path = %s", firmware_path);

	/* Download firmware and parse it for magic Intel Reset parameter */
	r = iwmbt_init_firmware(hdl, firmware_path, &boot_param,
	    vt.cnvi_bt >> 16 & 0x3f, vt.sbe_type);
	free(firmware_path);
	if (r < 0)
		return 1;

	iwmbt_info("Firmware download complete");

	r = iwmbt_intel_reset(hdl, boot_param);
	if (r < 0) {
		iwmbt_debug("iwmbt_intel_reset() failed!");
		return 1;
	}

	iwmbt_info("Firmware operational");

	/* Once device is running in operational mode we can ignore failures */

	r = iwmbt_get_version_tlv(hdl, &vt);
	if (r == 0)
		iwmbt_dump_version_tlv(&vt);

	/* Apply the device configuration (DDC) parameters */
	firmware_path = iwmbt_get_fwname_tlv(&vt, firmware_dir, "ddc");
	iwmbt_debug("ddc_path = %s", firmware_path);
	if (firmware_path != NULL) {
		r = iwmbt_init_ddc(hdl, firmware_path);
		if (r == 0)
			iwmbt_info("DDC download complete");
		free(firmware_path);
	}

	r = iwmbt_set_event_mask(hdl);
	if (r == 0)
		iwmbt_info("Intel Event Mask is set");

	return 0;
}



/* ---- NextBSD: the entry point (main()'s device half) --------------------- */

int
fw_intel_run(libusb_device_handle *hdl, enum fw_intel_gen gen,
    const char *firmware_dir)
{
	enum iwmbt_device dev;
	bool noreply = false;
	char *dir;
	int r;

	switch (gen) {
	case FW_INTEL_7260:	dev = IWMBT_DEVICE_7260; break;
	case FW_INTEL_8260:	dev = IWMBT_DEVICE_8260; break;
	case FW_INTEL_9260:	dev = IWMBT_DEVICE_9260; break;
	default:		dev = IWMBT_DEVICE_UNKNOWN; break;
	}

	/* What the controller says it is beats what its PID suggests. */
	dev = iwmbt_identify(hdl, dev, &noreply);
	if (dev == IWMBT_DEVICE_UNKNOWN) {
		iwmbt_err("Failed to identify device%s",
		    noreply ? ": no reply" : "");
		return (noreply ? FW_RESULT_NO_REPLY : FW_RESULT_FAILED);
	}

	dir = strdup(firmware_dir);
	if (dir == NULL)
		return (FW_RESULT_FAILED);
	switch (dev) {
	case IWMBT_DEVICE_7260:
		r = handle_7260(hdl, dir);
		break;
	case IWMBT_DEVICE_8260:
		r = handle_8260(hdl, dir);
		break;
	case IWMBT_DEVICE_9260:
		r = handle_9260(hdl, dir);
		break;
	default:
		r = FW_RESULT_FAILED;
		break;
	}
	free(dir);
	if (r == 0)
		return (FW_RESULT_DOWNLOADED);
	return (r == FW_RESULT_OPERATIONAL ? r : FW_RESULT_FAILED);
}
