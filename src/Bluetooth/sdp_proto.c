/*
 * sdp_proto.c — SDP data elements: parse, build, and name. See sdp_proto.h.
 */
#include "sdp_proto.h"

#include <stdlib.h>
#include <string.h>

/* 00000000-0000-1000-8000-00805F9B34FB */
static const uint8_t sdp_base_uuid[16] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
};

static bool
de_is_var(uint8_t type)
{
	return (type == SDP_DE_TEXT || type == SDP_DE_SEQ ||
	    type == SDP_DE_ALT || type == SDP_DE_URL);
}

size_t
sdp_de_get(const uint8_t *p, const uint8_t *end, struct sdp_de *out)
{
	uint8_t type, sidx;
	size_t hdr = 1, avail;
	uint32_t len;

	if (p == NULL || p >= end)
		return (0);
	type = p[0] >> 3;
	sidx = p[0] & 0x7;
	avail = (size_t)(end - p);

	if (type == SDP_DE_NIL) {
		if (sidx != 0)
			return (0);
		len = 0;
	} else if (sidx <= 4) {
		if (de_is_var(type) || type > SDP_DE_URL)
			return (0);
		len = 1u << sidx;
		if (type == SDP_DE_BOOL && sidx != 0)
			return (0);
		if (type == SDP_DE_UUID && sidx != 1 && sidx != 2 && sidx != 4)
			return (0);
	} else {
		size_t n = (size_t)1 << (sidx - 5);	/* 1, 2 or 4 */
		size_t i;

		if (!de_is_var(type) || avail < 1 + n)
			return (0);
		len = 0;
		for (i = 0; i < n; i++)
			len = (len << 8) | p[1 + i];
		hdr += n;
	}
	if (avail - hdr < len)
		return (0);
	out->type = type;
	out->val = p + hdr;
	out->len = len;
	return (hdr + len);
}

bool
sdp_de_uint(const struct sdp_de *de, uint64_t *out)
{
	uint64_t v = 0;
	uint32_t i;

	if (de->type != SDP_DE_UINT && de->type != SDP_DE_BOOL)
		return (false);
	if (de->len != 1 && de->len != 2 && de->len != 4 && de->len != 8)
		return (false);
	for (i = 0; i < de->len; i++)
		v = (v << 8) | de->val[i];
	*out = v;
	return (true);
}

void
sdp_uuid128_from32(uint32_t u, uint8_t out[16])
{
	(void)memcpy(out, sdp_base_uuid, 16);
	out[0] = (uint8_t)(u >> 24);
	out[1] = (uint8_t)(u >> 16);
	out[2] = (uint8_t)(u >> 8);
	out[3] = (uint8_t)u;
}

bool
sdp_de_uuid128(const struct sdp_de *de, uint8_t out[16])
{
	if (de->type != SDP_DE_UUID)
		return (false);
	switch (de->len) {
	case 2:
		sdp_uuid128_from32(((uint32_t)de->val[0] << 8) | de->val[1],
		    out);
		return (true);
	case 4:
		sdp_uuid128_from32(((uint32_t)de->val[0] << 24) |
		    ((uint32_t)de->val[1] << 16) | ((uint32_t)de->val[2] << 8) |
		    de->val[3], out);
		return (true);
	case 16:
		(void)memcpy(out, de->val, 16);
		return (true);
	default:
		return (false);
	}
}

bool
sdp_de_uuid32(const struct sdp_de *de, uint32_t *out)
{
	uint8_t u[16];

	if (!sdp_de_uuid128(de, u))
		return (false);
	if (memcmp(u + 4, sdp_base_uuid + 4, 12) != 0)
		return (false);
	*out = ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) |
	    ((uint32_t)u[2] << 8) | u[3];
	return (true);
}

bool
sdp_iter_init(struct sdp_iter *it, const struct sdp_de *de)
{
	it->bad = false;
	if (de->type != SDP_DE_SEQ && de->type != SDP_DE_ALT) {
		it->p = it->end = NULL;
		return (false);
	}
	it->p = de->val;
	it->end = de->val + de->len;
	return (true);
}

bool
sdp_iter_next(struct sdp_iter *it, struct sdp_de *out)
{
	size_t n;

	if (it->p == NULL || it->p >= it->end)
		return (false);
	n = sdp_de_get(it->p, it->end, out);
	if (n == 0) {
		it->bad = true;
		it->p = it->end;
		return (false);
	}
	it->p += n;
	return (true);
}

bool
sdp_iter_attr(struct sdp_iter *it, uint16_t *id, struct sdp_de *val)
{
	struct sdp_de de;
	uint64_t v;

	if (!sdp_iter_next(it, &de))
		return (false);
	if (de.type != SDP_DE_UINT || de.len != 2 || !sdp_de_uint(&de, &v) ||
	    !sdp_iter_next(it, val)) {
		it->bad = true;
		it->p = it->end;
		return (false);
	}
	*id = (uint16_t)v;
	return (true);
}

/* ---- writing ------------------------------------------------------------ */

void
sdp_wbuf_free(struct sdp_wbuf *w)
{
	free(w->b);
	(void)memset(w, 0, sizeof(*w));
}

void
sdp_put_raw(struct sdp_wbuf *w, const void *p, size_t n)
{
	if (w->err)
		return;
	if (w->len + n > w->cap) {
		size_t ncap = w->cap != 0 ? w->cap : 256;
		uint8_t *nb;

		while (ncap < w->len + n)
			ncap *= 2;
		nb = realloc(w->b, ncap);
		if (nb == NULL) {
			w->err = true;
			return;
		}
		w->b = nb;
		w->cap = ncap;
	}
	if (n > 0)
		(void)memcpy(w->b + w->len, p, n);
	w->len += n;
}

void
sdp_put_be16(struct sdp_wbuf *w, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

	sdp_put_raw(w, b, 2);
}

void
sdp_put_be32(struct sdp_wbuf *w, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
	    (uint8_t)(v >> 8), (uint8_t)v };

	sdp_put_raw(w, b, 4);
}

static void
put_hdr(struct sdp_wbuf *w, uint8_t type, uint32_t len)
{
	uint8_t d;

	if (type == SDP_DE_NIL) {
		d = 0;
		sdp_put_raw(w, &d, 1);
		return;
	}
	if (!de_is_var(type)) {
		uint8_t sidx = len == 1 ? 0 : len == 2 ? 1 : len == 4 ? 2 :
		    len == 8 ? 3 : 4;

		d = (uint8_t)(type << 3 | sidx);
		sdp_put_raw(w, &d, 1);
		return;
	}
	if (len <= 0xff) {
		d = (uint8_t)(type << 3 | 5);
		sdp_put_raw(w, &d, 1);
		d = (uint8_t)len;
		sdp_put_raw(w, &d, 1);
	} else if (len <= 0xffff) {
		d = (uint8_t)(type << 3 | 6);
		sdp_put_raw(w, &d, 1);
		sdp_put_be16(w, (uint16_t)len);
	} else {
		d = (uint8_t)(type << 3 | 7);
		sdp_put_raw(w, &d, 1);
		sdp_put_be32(w, len);
	}
}

void
sdp_put_uint8(struct sdp_wbuf *w, uint8_t v)
{
	put_hdr(w, SDP_DE_UINT, 1);
	sdp_put_raw(w, &v, 1);
}

void
sdp_put_uint16(struct sdp_wbuf *w, uint16_t v)
{
	put_hdr(w, SDP_DE_UINT, 2);
	sdp_put_be16(w, v);
}

void
sdp_put_uint32(struct sdp_wbuf *w, uint32_t v)
{
	put_hdr(w, SDP_DE_UINT, 4);
	sdp_put_be32(w, v);
}

void
sdp_put_uuid16(struct sdp_wbuf *w, uint16_t v)
{
	put_hdr(w, SDP_DE_UUID, 2);
	sdp_put_be16(w, v);
}

void
sdp_put_bool(struct sdp_wbuf *w, bool v)
{
	uint8_t b = v ? 1 : 0;

	put_hdr(w, SDP_DE_BOOL, 1);
	sdp_put_raw(w, &b, 1);
}

void
sdp_put_text(struct sdp_wbuf *w, const void *s, size_t n)
{
	if (n > 0xffff) {
		w->err = true;
		return;
	}
	put_hdr(w, SDP_DE_TEXT, (uint32_t)n);
	sdp_put_raw(w, s, n);
}

size_t
sdp_seq_begin(struct sdp_wbuf *w)
{
	static const uint8_t hdr[3] = { SDP_DE_SEQ << 3 | 6, 0, 0 };
	size_t mark = w->len;

	sdp_put_raw(w, hdr, sizeof(hdr));
	return (mark);
}

void
sdp_seq_end(struct sdp_wbuf *w, size_t mark)
{
	size_t body;

	if (w->err)
		return;
	body = w->len - mark - 3;
	if (body > 0xffff) {
		w->err = true;
		return;
	}
	w->b[mark + 1] = (uint8_t)(body >> 8);
	w->b[mark + 2] = (uint8_t)body;
}

void
sdp_put_de(struct sdp_wbuf *w, const struct sdp_de *de)
{
	put_hdr(w, de->type, de->len);
	sdp_put_raw(w, de->val, de->len);
}

/* ---- names -------------------------------------------------------------- */

static const struct {
	uint32_t	uuid;
	const char	*name;
} uuid_names[] = {
	{ SDP_UUID_SDP,			"SDP" },
	{ SDP_UUID_RFCOMM,		"RFCOMM" },
	{ SDP_UUID_ATT,			"ATT" },
	{ SDP_UUID_OBEX,		"OBEX" },
	{ SDP_UUID_BNEP,		"BNEP" },
	{ SDP_UUID_HIDP,		"HIDP" },
	{ SDP_UUID_AVCTP,		"AVCTP" },
	{ SDP_UUID_AVDTP,		"AVDTP" },
	{ SDP_UUID_L2CAP,		"L2CAP" },
	{ SDP_UUID_SDP_SERVER,		"ServiceDiscoveryServer" },
	{ SDP_UUID_PUBLIC_BROWSE_ROOT,	"PublicBrowseRoot" },
	{ SDP_UUID_SERIAL_PORT,		"SerialPort" },
	{ 0x1105,			"OBEXObjectPush" },
	{ 0x1106,			"OBEXFileTransfer" },
	{ SDP_UUID_HEADSET,		"Headset" },
	{ SDP_UUID_AUDIO_SOURCE,	"AudioSource" },
	{ SDP_UUID_AUDIO_SINK,		"AudioSink" },
	{ SDP_UUID_AV_REMOTE_TARGET,	"A/V_RemoteControlTarget" },
	{ SDP_UUID_ADVANCED_AUDIO,	"AdvancedAudioDistribution" },
	{ SDP_UUID_AV_REMOTE,		"A/V_RemoteControl" },
	{ SDP_UUID_AV_REMOTE_CONTROLLER, "A/V_RemoteControlController" },
	{ SDP_UUID_HEADSET_AG,		"Headset-AudioGateway" },
	{ SDP_UUID_PANU,		"PANU" },
	{ SDP_UUID_NAP,			"NAP" },
	{ SDP_UUID_HANDSFREE,		"Handsfree" },
	{ SDP_UUID_HANDSFREE_AG,	"HandsfreeAudioGateway" },
	{ SDP_UUID_HID,			"HumanInterfaceDeviceService" },
	{ 0x112f,			"Phonebook Access - PSE" },
	{ 0x1131,			"Headset - HS" },
	{ 0x1132,			"Message Access Server" },
	{ 0x1134,			"Message Access Profile" },
	{ SDP_UUID_PNP_INFORMATION,	"PnPInformation" },
	{ SDP_UUID_GENERIC_AUDIO,	"GenericAudio" },
	{ 0x1800,			"GenericAccess" },
	{ 0x1801,			"GenericAttribute" },
};

const char *
sdp_uuid_name(uint32_t uuid)
{
	size_t i;

	for (i = 0; i < sizeof(uuid_names) / sizeof(uuid_names[0]); i++) {
		if (uuid_names[i].uuid == uuid)
			return (uuid_names[i].name);
	}
	return (NULL);
}

struct attr_name {
	uint16_t	id;
	const char	*name;
};

static const struct attr_name universal_attrs[] = {
	{ SDP_ATTR_RECORD_HANDLE,		"ServiceRecordHandle" },
	{ SDP_ATTR_SERVICE_CLASS_ID_LIST,	"ServiceClassIDList" },
	{ SDP_ATTR_RECORD_STATE,		"ServiceRecordState" },
	{ SDP_ATTR_SERVICE_ID,			"ServiceID" },
	{ SDP_ATTR_PROTOCOL_DESC_LIST,		"ProtocolDescriptorList" },
	{ SDP_ATTR_BROWSE_GROUP_LIST,		"BrowseGroupList" },
	{ SDP_ATTR_LANG_BASE_LIST,		"LanguageBaseAttributeIDList" },
	{ SDP_ATTR_TIME_TO_LIVE,		"ServiceInfoTimeToLive" },
	{ SDP_ATTR_AVAILABILITY,		"ServiceAvailability" },
	{ SDP_ATTR_PROFILE_DESC_LIST,		"BluetoothProfileDescriptorList" },
	{ SDP_ATTR_DOC_URL,			"DocumentationURL" },
	{ SDP_ATTR_CLIENT_EXEC_URL,		"ClientExecutableURL" },
	{ SDP_ATTR_ICON_URL,			"IconURL" },
	{ SDP_ATTR_ADD_PROTOCOL_DESC_LIST,	"AdditionalProtocolDescriptorLists" },
	/* The primary language base is 0x0100 on every device seen. */
	{ SDP_ATTR_PRIMARY_LANG_BASE + SDP_ATTR_SERVICE_NAME, "ServiceName" },
	{ SDP_ATTR_PRIMARY_LANG_BASE + SDP_ATTR_SERVICE_DESCRIPTION,
						"ServiceDescription" },
	{ SDP_ATTR_PRIMARY_LANG_BASE + SDP_ATTR_PROVIDER_NAME, "ProviderName" },
	{ SDP_ATTR_SUPPORTED_FEATURES,		"SupportedFeatures" },
	{ 0, NULL }
};

static const struct attr_name sdp_server_attrs[] = {
	{ SDP_ATTR_VERSION_NUMBER_LIST,		"VersionNumberList" },
	{ SDP_ATTR_DATABASE_STATE,		"ServiceDatabaseState" },
	{ 0, NULL }
};

static const struct attr_name pnp_attrs[] = {
	{ SDP_ATTR_PNP_SPECIFICATION_ID,	"SpecificationID" },
	{ SDP_ATTR_PNP_VENDOR_ID,		"VendorID" },
	{ SDP_ATTR_PNP_PRODUCT_ID,		"ProductID" },
	{ SDP_ATTR_PNP_VERSION,			"Version" },
	{ SDP_ATTR_PNP_PRIMARY_RECORD,		"PrimaryRecord" },
	{ SDP_ATTR_PNP_VENDOR_ID_SOURCE,	"VendorIDSource" },
	{ 0, NULL }
};

static const struct attr_name hid_attrs[] = {
	{ SDP_ATTR_HID_DEVICE_RELEASE,		"HIDDeviceReleaseNumber" },
	{ SDP_ATTR_HID_PARSER_VERSION,		"HIDParserVersion" },
	{ SDP_ATTR_HID_DEVICE_SUBCLASS,		"HIDDeviceSubclass" },
	{ SDP_ATTR_HID_COUNTRY_CODE,		"HIDCountryCode" },
	{ SDP_ATTR_HID_VIRTUAL_CABLE,		"HIDVirtualCable" },
	{ SDP_ATTR_HID_RECONNECT_INITIATE,	"HIDReconnectInitiate" },
	{ SDP_ATTR_HID_DESCRIPTOR_LIST,		"HIDDescriptorList" },
	{ SDP_ATTR_HID_LANGID_BASE_LIST,	"HIDLANGIDBaseList" },
	{ SDP_ATTR_HID_SDP_DISABLE,		"HIDSDPDisable" },
	{ SDP_ATTR_HID_BATTERY_POWER,		"HIDBatteryPower" },
	{ SDP_ATTR_HID_REMOTE_WAKE,		"HIDRemoteWake" },
	{ SDP_ATTR_HID_PROFILE_VERSION,		"HIDProfileVersion" },
	{ SDP_ATTR_HID_SUPERVISION_TIMEOUT,	"HIDSupervisionTimeout" },
	{ SDP_ATTR_HID_NORMALLY_CONNECTABLE,	"HIDNormallyConnectable" },
	{ SDP_ATTR_HID_BOOT_DEVICE,		"HIDBootDevice" },
	{ SDP_ATTR_HID_SSR_HOST_MAX_LATENCY,	"HIDSSRHostMaxLatency" },
	{ SDP_ATTR_HID_SSR_HOST_MIN_TIMEOUT,	"HIDSSRHostMinTimeout" },
	{ 0, NULL }
};

static const char *
attr_lookup(const struct attr_name *t, uint16_t id)
{
	for (; t->name != NULL; t++) {
		if (t->id == id)
			return (t->name);
	}
	return (NULL);
}

const char *
sdp_attr_name(uint16_t id, uint32_t svc_class)
{
	const char *n;

	if ((n = attr_lookup(universal_attrs, id)) != NULL)
		return (n);
	switch (svc_class) {
	case SDP_UUID_SDP_SERVER:
		return (attr_lookup(sdp_server_attrs, id));
	case SDP_UUID_PNP_INFORMATION:
		return (attr_lookup(pnp_attrs, id));
	case SDP_UUID_HID:
		return (attr_lookup(hid_attrs, id));
	default:
		return (NULL);
	}
}
