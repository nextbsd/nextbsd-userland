/*
 * engine.c — the engine table, and the address and profile helpers every
 * layer shares. See engine.h.
 */
#include "engine.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* sockaddr_l2cap.l2cap_bdaddr_type values (ng_bluetooth.h). */
#define	L2CAP_BDADDR_BREDR	0
#define	L2CAP_BDADDR_LE_PUBLIC	1
#define	L2CAP_BDADDR_LE_RANDOM	2

const struct bt_engine_ops * const bt_engines[] = {
	&bt_classic_engine,
	&bt_le_engine,
	NULL
};

const char *
bt_addr_ntoa(const struct bt_addr *a, bt_addr_fmt_t fmt, char *buf,
    size_t len)
{
	const char *hex, *sep, *suffix;

	if (buf == NULL || len == 0)
		return ("");
	if (a == NULL) {
		buf[0] = '\0';
		return (buf);
	}
	if (fmt == BT_ADDR_FMT_KEY) {
		hex = "%02x";
		sep = "-";
		suffix = (a->type == BT_ADDR_RANDOM) ? "-random" : "";
	} else {
		hex = "%02X";
		sep = ":";
		suffix = (a->type == BT_ADDR_RANDOM) ? "/random" : "";
	}

	/* Most significant octet first: b[5] .. b[0]. */
	{
		char tmp[BT_ADDR_STRLEN];
		size_t off = 0;
		int i;

		for (i = BT_ADDR_LEN - 1; i >= 0; i--) {
			off += (size_t)snprintf(tmp + off, sizeof(tmp) - off,
			    hex, a->b[i]);
			if (i > 0)
				off += (size_t)snprintf(tmp + off,
				    sizeof(tmp) - off, "%s", sep);
		}
		(void)snprintf(buf, len, "%s%s", tmp, suffix);
	}
	return (buf);
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	return (-1);
}

bool
bt_addr_aton(const char *s, struct bt_addr *out)
{
	struct bt_addr a;
	char sep = '\0';
	int i;

	if (s == NULL || out == NULL)
		return (false);
	(void)memset(&a, 0, sizeof(a));

	/* Six octets, most significant first, one separator kind. */
	for (i = BT_ADDR_LEN - 1; i >= 0; i--) {
		int hi = hexval((unsigned char)s[0]);
		int lo = (hi < 0) ? -1 : hexval((unsigned char)s[1]);

		if (hi < 0 || lo < 0)
			return (false);
		a.b[i] = (uint8_t)(hi << 4 | lo);
		s += 2;
		if (i == 0)
			break;
		if (*s != ':' && *s != '-')
			return (false);
		if (sep == '\0')
			sep = *s;
		else if (*s != sep)
			return (false);
		s++;
	}

	if (*s == '\0')
		a.type = BT_ADDR_PUBLIC;
	else if (strcmp(s, "/random") == 0 || strcmp(s, "-random") == 0)
		a.type = BT_ADDR_RANDOM;
	else if (strcmp(s, "/public") == 0 || strcmp(s, "-public") == 0)
		a.type = BT_ADDR_PUBLIC;
	else
		return (false);

	*out = a;
	return (true);
}

bool
bt_addr_equal(const struct bt_addr *a, const struct bt_addr *b)
{
	return (a->type == b->type &&
	    memcmp(a->b, b->b, BT_ADDR_LEN) == 0);
}

uint8_t
bt_addr_l2cap_type(const struct bt_addr *a, int transport)
{
	if (transport != BT_TRANSPORT_LE)
		return (L2CAP_BDADDR_BREDR);
	return (a->type == BT_ADDR_RANDOM ? L2CAP_BDADDR_LE_RANDOM
	                                  : L2CAP_BDADDR_LE_PUBLIC);
}

/* Profile bits and their store names. Append; never renumber. */
static const struct {
	uint32_t	bit;
	const char	*name;
} profile_names[] = {
	{ BT_PROFILE_HID,		"HID" },
	{ BT_PROFILE_HOGP,		"HOGP" },
	{ BT_PROFILE_A2DP_SINK,		"A2DPSink" },
	{ BT_PROFILE_A2DP_SOURCE,	"A2DPSource" },
	{ BT_PROFILE_AVRCP,		"AVRCP" },
	{ BT_PROFILE_HFP,		"HFP" },
};

#define	NPROFILES	(sizeof(profile_names) / sizeof(profile_names[0]))

const char *
bt_profile_name(uint32_t bit)
{
	size_t i;

	for (i = 0; i < NPROFILES; i++) {
		if (profile_names[i].bit == bit)
			return (profile_names[i].name);
	}
	return (NULL);
}

uint32_t
bt_profile_bit(const char *name)
{
	size_t i;

	if (name == NULL)
		return (0);
	for (i = 0; i < NPROFILES; i++) {
		if (strcmp(profile_names[i].name, name) == 0)
			return (profile_names[i].bit);
	}
	return (0);
}

void
bt_profiles_format(uint32_t mask, char *buf, size_t len)
{
	size_t i, off = 0;

	if (buf == NULL || len == 0)
		return;
	buf[0] = '\0';
	for (i = 0; i < NPROFILES; i++) {
		if ((mask & profile_names[i].bit) == 0)
			continue;
		off += (size_t)snprintf(buf + off, len > off ? len - off : 0,
		    "%s%s", off > 0 ? "," : "", profile_names[i].name);
		if (off >= len)
			break;
	}
}
