/*
 * vap.c — 802.11 PHY enumeration and VAP lifecycle. See vap.h for why this is
 * wland's job at all.
 */
#include "vap.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net80211/ieee80211.h>
#include <net80211/ieee80211_ioctl.h>

#include <errno.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "wland[vap] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

int
vap_enumerate_phys(struct wlan_phy *out, size_t max)
{
	char *buf = NULL, *tok, *save;
	size_t len = 0;
	int n = 0;

	if (out == NULL || max == 0)
		return (-1);

	/*
	 * net.wlan.devices is a space-separated list of ieee80211com names
	 * ("iwlwifi0 ath0"). A PHY is not an ifnet — it has no ifaddr and never
	 * appears in getifaddrs — so this sysctl is the only way to find one.
	 * Two-call idiom: size, then fetch.
	 */
	if (sysctlbyname("net.wlan.devices", NULL, &len, NULL, 0) != 0) {
		/*
		 * ENOENT here means net80211 is not loaded: it is Wlan.kext,
		 * which a Wi-Fi driver kext pulls in when it matches hardware.
		 * That is "no radio yet", not an error, and the caller asks
		 * again when a radio appears, so say nothing here; wland logs
		 * it once.
		 */
		if (errno == ENOENT)
			return (VAP_NO_NET80211);
		xlog("sysctlbyname(net.wlan.devices) size: %s", strerror(errno));
		return (-1);
	}
	if (len == 0)
		return (0);			/* no radios */

	buf = malloc(len + 1);
	if (buf == NULL)
		return (-1);
	if (sysctlbyname("net.wlan.devices", buf, &len, NULL, 0) != 0) {
		xlog("sysctlbyname(net.wlan.devices): %s", strerror(errno));
		free(buf);
		return (-1);
	}
	buf[len] = '\0';

	for (tok = strtok_r(buf, " \t\n", &save);
	     tok != NULL && (size_t)n < max;
	     tok = strtok_r(NULL, " \t\n", &save)) {
		if (*tok == '\0')
			continue;
		(void)memset(&out[n], 0, sizeof(out[n]));
		(void)strlcpy(out[n].phy, tok, sizeof(out[n].phy));
		out[n].vap[0] = '\0';
		n++;
	}
	free(buf);
	return (n);
}

/*
 * Read net.wlan.<unit>.%parent — the PHY a given VAP was cloned from. The
 * sysctl node is keyed on the interface's *unit* number (ifp->if_dunit), so
 * wlan0 -> net.wlan.0.%parent.
 */
static bool
vap_parent(const char *vap, char *parent_out, size_t parent_out_sz)
{
	char oid[64], buf[IF_NAMESIZE];
	const char *unit;
	size_t len = sizeof(buf);

	if (strncmp(vap, "wlan", 4) != 0)
		return (false);
	unit = vap + 4;
	if (*unit == '\0')
		return (false);

	(void)snprintf(oid, sizeof(oid), "net.wlan.%s.%%parent", unit);
	if (sysctlbyname(oid, buf, &len, NULL, 0) != 0)
		return (false);
	buf[sizeof(buf) - 1] = '\0';
	(void)strlcpy(parent_out, buf, parent_out_sz);
	return (true);
}

bool
vap_find_existing(const char *phy, char *vap_out, size_t vap_out_sz)
{
	struct ifaddrs *ifa, *p;
	bool found = false;

	if (getifaddrs(&ifa) != 0)
		return (false);
	for (p = ifa; p != NULL && !found; p = p->ifa_next) {
		char parent[IF_NAMESIZE];

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK)
			continue;
		if (strncmp(p->ifa_name, "wlan", 4) != 0)
			continue;
		if (!vap_parent(p->ifa_name, parent, sizeof(parent)))
			continue;
		if (strcmp(parent, phy) != 0)
			continue;
		(void)strlcpy(vap_out, p->ifa_name, vap_out_sz);
		found = true;
	}
	freeifaddrs(ifa);
	return (found);
}

int
vap_create(const char *phy, char *vap_out, size_t vap_out_sz)
{
	struct ieee80211_clone_params cp;
	struct ifreq ifr;
	int s;

	if (phy == NULL || vap_out == NULL || vap_out_sz < IF_NAMESIZE)
		return (-1);

	/* Already have one on this radio (e.g. we restarted)? Adopt it. */
	if (vap_find_existing(phy, vap_out, vap_out_sz)) {
		xlog("adopting existing VAP %s on %s", vap_out, phy);
		return (0);
	}

	(void)memset(&cp, 0, sizeof(cp));
	(void)strlcpy(cp.icp_parent, phy, sizeof(cp.icp_parent));
	cp.icp_opmode = IEEE80211_M_STA;	/* station mode; we are a client */
	cp.icp_flags = 0;

	(void)memset(&ifr, 0, sizeof(ifr));
	/*
	 * WILDCARD name. Asking for "wlan" (no unit) makes the clone code pick
	 * the next free unit and write the real name back into ifr_name.
	 * Hard-coding "wlan0" would break the moment a second radio appears, and
	 * would collide with a VAP that already exists.
	 */
	(void)strlcpy(ifr.ifr_name, "wlan", sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t)&cp;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		xlog("socket: %s", strerror(errno));
		return (-1);
	}
	if (ioctl(s, SIOCIFCREATE2, &ifr) != 0) {
		xlog("SIOCIFCREATE2(wlandev %s): %s", phy, strerror(errno));
		(void)close(s);
		return (-1);
	}
	(void)close(s);

	(void)strlcpy(vap_out, ifr.ifr_name, vap_out_sz);
	xlog("created VAP %s on %s (station mode)", vap_out, phy);
	return (0);
}

int
vap_destroy(const char *vap)
{
	struct ifreq ifr;
	int s, rc = 0;

	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, vap, sizeof(ifr.ifr_name));

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return (-1);
	if (ioctl(s, SIOCIFDESTROY, &ifr) != 0) {
		/* Already gone is the state we wanted. */
		if (errno != ENXIO && errno != ENOENT) {
			xlog("SIOCIFDESTROY(%s): %s", vap, strerror(errno));
			rc = -1;
		}
	} else {
		xlog("destroyed VAP %s", vap);
	}
	(void)close(s);
	return (rc);
}

int
vap_bring_up(const char *ifname)
{
	struct ifreq ifr;
	int s, rc = -1;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return (-1);
	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		if ((ifr.ifr_flags & IFF_UP) != 0) {
			rc = 0;			/* already up */
		} else {
			ifr.ifr_flags |= IFF_UP;
			if (ioctl(s, SIOCSIFFLAGS, &ifr) == 0) {
				rc = 0;
				xlog("%s: admin-up", ifname);
			} else {
				xlog("SIOCSIFFLAGS(%s): %s", ifname,
				    strerror(errno));
			}
		}
	}
	(void)close(s);
	return (rc);
}
