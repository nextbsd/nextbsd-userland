/*
 * dhcp_duid_iaid.c — the DHCPv6 client identifier and per-interface IAIDs
 * (#424).
 *
 * Apple's bootp-268.1 IPConfiguration.bproj/DHCPDUIDIAID.c, with the same
 * file, keys and rules:
 *
 *   /var/db/dhcpclient/DUID_IA.plist, mode 0644
 *     DUID      data: DUID-LLT, RFC 3315 §9.2
 *     IAIDList  array of interface names; an IAID is a name's index
 *     HostUUID  data: the host this DUID was made for
 *
 * A stored DUID is dropped if HostUUID says another machine made it. Apple
 * prefers en0 and otherwise takes the lowest-named Ethernet interface; there
 * is no en0 here, so it is the lowest-named Ethernet interface, or failing
 * that any interface with a link-layer address. There is no gethostuuid()
 * here either: the UUID is kern.hostuuid, as IOPMEnergyPrefs.c reads it.
 */
#include "dhcp_duid_iaid.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "DHCPDUID.h"

#define	DUID_IA_DIR	"/var/db/dhcpclient"
#define	DUID_IA_FILE	DUID_IA_DIR "/DUID_IA.plist"

#define	kDUIDKey	CFSTR("DUID")		/* data */
#define	kIAIDListKey	CFSTR("IAIDList")	/* array[string] */
#define	kHostUUIDKey	CFSTR("HostUUID")	/* data */

#define	HOST_UUID_LEN	16

/* DHCPv6 service threads and the MIG thread all come through here. */
static pthread_mutex_t		S_lock = PTHREAD_MUTEX_INITIALIZER;
static CFDataRef		S_DUID;
static CFMutableArrayRef	S_IAIDList;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[duid] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* HostUUIDGet(): kern.hostuuid's 16 bytes, or NULL. */
static CFDataRef
host_uuid_get(void)
{
	static CFDataRef uuid;
	char str[64];
	uint8_t bytes[HOST_UUID_LEN];
	size_t len = sizeof(str);
	unsigned n = 0;
	const char *p;

	if (uuid != NULL)
		return (uuid);
	if (sysctlbyname("kern.hostuuid", str, &len, NULL, 0) != 0)
		return (NULL);
	str[sizeof(str) - 1] = '\0';
	for (p = str; *p != '\0' && n < HOST_UUID_LEN * 2; p++) {
		unsigned v;

		if (*p == '-')
			continue;
		if (*p >= '0' && *p <= '9')
			v = (unsigned)(*p - '0');
		else if (*p >= 'a' && *p <= 'f')
			v = (unsigned)(*p - 'a') + 10;
		else if (*p >= 'A' && *p <= 'F')
			v = (unsigned)(*p - 'A') + 10;
		else
			return (NULL);
		if (n % 2 == 0)
			bytes[n / 2] = (uint8_t)(v << 4);
		else
			bytes[n / 2] |= (uint8_t)v;
		n++;
	}
	if (n != HOST_UUID_LEN * 2)
		return (NULL);
	uuid = CFDataCreate(NULL, bytes, HOST_UUID_LEN);
	return (uuid);
}

/* my_CFPropertyListCreateFromFile(). */
static CFPropertyListRef
plist_read(const char *path)
{
	CFPropertyListRef plist = NULL;
	CFDataRef data;
	struct stat st;
	uint8_t *buf;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (NULL);
	if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) {
		(void)close(fd);
		return (NULL);
	}
	buf = malloc((size_t)st.st_size);
	if (buf != NULL &&
	    read(fd, buf, (size_t)st.st_size) == (ssize_t)st.st_size) {
		data = CFDataCreate(NULL, buf, (CFIndex)st.st_size);
		if (data != NULL) {
			plist = CFPropertyListCreateWithData(NULL, data,
			    kCFPropertyListImmutable, NULL, NULL);
			CFRelease(data);
		}
	}
	free(buf);
	(void)close(fd);
	return (plist);
}

/* my_CFPropertyListWriteFile(): written whole, then renamed into place. */
static int
plist_write(CFPropertyListRef plist, const char *path, mode_t mode)
{
	char tmp[] = DUID_IA_DIR "/.DUID_IA.XXXXXX";
	CFDataRef data;
	const uint8_t *p;
	size_t left;
	int fd, saved;

	data = CFPropertyListCreateData(NULL, plist,
	    kCFPropertyListXMLFormat_v1_0, 0, NULL);
	if (data == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	if (mkdir(DUID_IA_DIR, 0755) != 0 && errno != EEXIST) {
		saved = errno;
		CFRelease(data);
		errno = saved;
		return (-1);
	}
	fd = mkstemp(tmp);
	if (fd < 0) {
		saved = errno;
		CFRelease(data);
		errno = saved;
		return (-1);
	}
	p = CFDataGetBytePtr(data);
	left = (size_t)CFDataGetLength(data);
	while (left > 0) {
		ssize_t n = write(fd, p, left);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			goto failed;
		}
		p += n;
		left -= (size_t)n;
	}
	if (fchmod(fd, mode) != 0 || fsync(fd) != 0)
		goto failed;
	(void)close(fd);
	fd = -1;
	if (rename(tmp, path) != 0)
		goto failed;
	CFRelease(data);
	return (0);

failed:
	saved = errno;
	if (fd >= 0)
		(void)close(fd);
	(void)unlink(tmp);
	CFRelease(data);
	errno = saved;
	return (-1);
}

static void
save_DUID_info(void)
{
	CFMutableDictionaryRef duid_ia;
	CFDataRef host_uuid;

	if (S_DUID == NULL)
		return;
	duid_ia = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(duid_ia, kDUIDKey, S_DUID);
	if (S_IAIDList != NULL)
		CFDictionarySetValue(duid_ia, kIAIDListKey, S_IAIDList);
	host_uuid = host_uuid_get();
	if (host_uuid != NULL)
		CFDictionarySetValue(duid_ia, kHostUUIDKey, host_uuid);
	/* ENOENT is expected on a read-only filesystem. */
	if (plist_write(duid_ia, DUID_IA_FILE, 0644) != 0 && errno != ENOENT)
		xlog("failed to write %s: %s", DUID_IA_FILE, strerror(errno));
	CFRelease(duid_ia);
}

static bool
load_DUID_info(void)
{
	CFPropertyListRef plist;
	CFDictionaryRef duid_ia;
	CFDataRef duid, host_uuid;
	CFArrayRef ia_list;

	plist = plist_read(DUID_IA_FILE);
	if (plist == NULL)
		return (false);
	if (CFGetTypeID(plist) != CFDictionaryGetTypeID())
		goto done;
	duid_ia = plist;
	duid = CFDictionaryGetValue(duid_ia, kDUIDKey);
	if (duid == NULL || CFGetTypeID(duid) != CFDataGetTypeID())
		goto done;
	ia_list = CFDictionaryGetValue(duid_ia, kIAIDListKey);
	if (ia_list != NULL && CFGetTypeID(ia_list) != CFArrayGetTypeID())
		ia_list = NULL;
	if (ia_list != NULL) {
		CFIndex count, i;

		count = CFArrayGetCount(ia_list);
		for (i = 0; i < count; i++) {
			if (CFGetTypeID(CFArrayGetValueAtIndex(ia_list, i)) !=
			    CFStringGetTypeID()) {
				/* invalid property */
				ia_list = NULL;
				break;
			}
		}
	}
	host_uuid = CFDictionaryGetValue(duid_ia, kHostUUIDKey);
	if (host_uuid != NULL && CFGetTypeID(host_uuid) == CFDataGetTypeID() &&
	    CFDataGetLength(host_uuid) == HOST_UUID_LEN) {
		CFDataRef our_uuid = host_uuid_get();

		if (our_uuid != NULL && !CFEqual(host_uuid, our_uuid)) {
			xlog("ignoring DUID - host UUID doesn't match");
			goto done;
		}
	}
	S_DUID = CFRetain(duid);
	if (ia_list != NULL)
		S_IAIDList = CFArrayCreateMutableCopy(NULL, 0, ia_list);

done:
	CFRelease(plist);
	return (S_DUID != NULL);
}

/* dl_to_arp_hwtype(). */
static int
dl_to_arp_hwtype(int dltype)
{
	switch (dltype) {
	case IFT_ETHER:
		return (ARPHRD_ETHER);
	case IFT_IEEE1394:
		return (ARPHRD_IEEE1394);
	default:
		return (-1);
	}
}

static CFDataRef
make_DUID_LLT_data(int dltype, const uint8_t *lladdr, int lladdr_len)
{
	CFMutableDataRef data;
	DHCPDUID_LLTRef llt_p;
	struct tm tm;
	time_t epoch;
	CFIndex duid_len;

	duid_len = (CFIndex)offsetof(DHCPDUID_LLT, linklayer_address) +
	    lladdr_len;
	data = CFDataCreateMutable(NULL, duid_len);
	if (data == NULL)
		return (NULL);
	CFDataSetLength(data, duid_len);
	llt_p = (DHCPDUID_LLTRef)(void *)CFDataGetMutableBytePtr(data);
	DHCPDUIDSetType((DHCPDUIDRef)(void *)llt_p, kDHCPDUIDTypeLLT);
	DHCPDUID_LLTSetHardwareType(llt_p, (uint16_t)dl_to_arp_hwtype(dltype));
	(void)memcpy(llt_p->linklayer_address, lladdr, (size_t)lladdr_len);
	/* The DUID-LLT epoch: midnight (UTC), January 1, 2000. */
	(void)memset(&tm, 0, sizeof(tm));
	tm.tm_year = 100;
	tm.tm_mon = 0;
	tm.tm_mday = 1;
	epoch = timegm(&tm);
	DHCPDUID_LLTSetTime(llt_p, (uint32_t)(time(NULL) - epoch));
	return (data);
}

/* Apple's choice of interface for a new DUID, without en0. */
static CFDataRef
make_DUID(void)
{
	struct ifaddrs *ifa, *p;
	const struct sockaddr_dl *best = NULL, *fallback = NULL;
	const char *best_name = NULL;
	CFDataRef duid = NULL;

	if (getifaddrs(&ifa) != 0)
		return (NULL);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_dl *dl;

		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_LINK)
			continue;
		dl = (const struct sockaddr_dl *)(const void *)p->ifa_addr;
		if (dl->sdl_alen == 0)
			continue;
		if (dl->sdl_type == IFT_ETHER || dl->sdl_type == IFT_IEEE1394) {
			/* pick "lowest" named interface */
			if (best == NULL ||
			    strcmp(p->ifa_name, best_name) < 0) {
				best = dl;
				best_name = p->ifa_name;
			}
		} else if (fallback == NULL) {
			fallback = dl;
		}
	}
	if (best == NULL)
		best = fallback;
	if (best == NULL)
		xlog("can't determine interface for DUID");
	else
		duid = make_DUID_LLT_data(best->sdl_type,
		    (const uint8_t *)LLADDR(best), best->sdl_alen);
	freeifaddrs(ifa);
	return (duid);
}

CFDataRef
DHCPDUIDGet(void)
{
	CFDataRef duid;

	(void)pthread_mutex_lock(&S_lock);
	if (S_DUID == NULL && !load_DUID_info()) {
		S_DUID = make_DUID();
		save_DUID_info();
	}
	duid = S_DUID;
	(void)pthread_mutex_unlock(&S_lock);
	return (duid);
}

DHCPIAID
DHCPIAIDGet(const char *ifname)
{
	CFStringRef ifname_cf;
	CFIndex count, where = kCFNotFound;
	DHCPIAID iaid;

	ifname_cf = CFStringCreateWithCString(NULL, ifname,
	    kCFStringEncodingASCII);
	(void)pthread_mutex_lock(&S_lock);
	if (S_IAIDList == NULL) {
		S_IAIDList = CFArrayCreateMutable(NULL, 0,
		    &kCFTypeArrayCallBacks);
		count = 0;
	} else {
		count = CFArrayGetCount(S_IAIDList);
		where = CFArrayGetFirstIndexOfValue(S_IAIDList,
		    CFRangeMake(0, count), ifname_cf);
	}
	if (where != kCFNotFound) {
		iaid = (DHCPIAID)where;
	} else {
		CFArrayAppendValue(S_IAIDList, ifname_cf);
		iaid = (DHCPIAID)count;
		save_DUID_info();
	}
	(void)pthread_mutex_unlock(&S_lock);
	CFRelease(ifname_cf);
	return (iaid);
}
