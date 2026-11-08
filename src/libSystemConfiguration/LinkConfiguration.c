/*
 * Copyright (c) 2002-2007, 2010, 2011, 2013 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * LinkConfiguration.c — NextBSD port of configd-596.15
 * SystemConfiguration.fproj/LinkConfiguration.c: an interface's offload
 * capabilities, media options and MTU, read live from the kernel with
 * the same ioctls Apple uses (SIOCGIFCAP, SIOCGIFMEDIA, SIOCGIFMTU).
 *
 * Differences from Apple's file:
 *  - There is no SIOCGIFDEVMTU (and no IORegistry to ask instead), so
 *    SCNetworkInterfaceCopyMTU has no device range and reports the
 *    current MTU as the minimum and maximum too.
 *  - IFCAP_AV does not exist here, so the AV capability is compiled out.
 *  - _SCNetworkInterfaceIsPhysicalEthernet has no tethered or
 *    Bluetooth-PAN interfaces to rule out.
 *  - __SCNetworkInterfaceCreateCapabilities, __SCNetworkInterfaceCreate
 *    MediaOptions, SCNetworkInterfaceSetMediaOptions and
 *    SCNetworkInterfaceSetMTU have no caller yet and are not ported.
 */

#include "SCNetworkConfigurationInternal.h"

#include <SystemConfiguration/SCPrivate.h>

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>


#pragma mark -
#pragma mark Capabilities


// the following table needs to keep the capabilitiy names and values
// between the <SystemConfiguration/SCSchemaDefinitions.h> and
// <net/if.h> headers in sync.
static const struct {
	CFStringRef		name;
	Boolean			readwrite;
	int			val;
} capabilityMappings[] = {
	{ kSCPropNetEthernetCapabilityRXCSUM,		TRUE,	IFCAP_RXCSUM		},	// can offload checksum on RX
	{ kSCPropNetEthernetCapabilityTXCSUM,		TRUE,	IFCAP_TXCSUM		},	// can offload checksum on TX
	{ kSCPropNetEthernetCapabilityVLAN_MTU,		FALSE,	IFCAP_VLAN_MTU		},	// VLAN-compatible MTU
	{ kSCPropNetEthernetCapabilityVLAN_HWTAGGING,	FALSE,	IFCAP_VLAN_HWTAGGING	},	// hardware VLAN tag support
	{ kSCPropNetEthernetCapabilityJUMBO_MTU,	FALSE,	IFCAP_JUMBO_MTU		},	// 9000 byte MTU supported
	{ kSCPropNetEthernetCapabilityTSO,		TRUE,	IFCAP_TSO		},	// can do TCP/TCP6 Segmentation Offload
	{ kSCPropNetEthernetCapabilityTSO4,		FALSE,	IFCAP_TSO4		},	// can do TCP Segmentation Offload
	{ kSCPropNetEthernetCapabilityTSO6,		FALSE,	IFCAP_TSO6		},	// can do TCP6 Segmentation Offload
	{ kSCPropNetEthernetCapabilityLRO,		TRUE,	IFCAP_LRO		},	// can do Large Receive Offload
#ifdef	IFCAP_AV
	{ kSCPropNetEthernetCapabilityAV,		TRUE,	IFCAP_AV		},	// can do 802.1 AV Bridging
#endif	// IFCAP_AV
};


static CFIndex
findCapability(CFStringRef capability)
{
	CFIndex		i;

	for (i = 0; i < sizeof(capabilityMappings) / sizeof(capabilityMappings[0]); i++) {
		if (CFEqual(capability, capabilityMappings[i].name)) {
			return i;
		}
	}

	return kCFNotFound;
}


static Boolean
__getCapabilities(CFStringRef	interfaceName,
		  int		*current,
		  int		*available)
{
	struct ifreq	ifr;
	Boolean		ok		= FALSE;
	int		sock		= -1;

	bzero((void *)&ifr, sizeof(ifr));
	if (_SC_cfstring_to_cstring(interfaceName, ifr.ifr_name, sizeof(ifr.ifr_name), kCFStringEncodingASCII) == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		_SCErrorSet(errno);
		return FALSE;
	}

	if (ioctl(sock, SIOCGIFCAP, (caddr_t)&ifr) == -1) {
		_SCErrorSet(errno);
		goto done;
	}

	if (current   != NULL)	*current   = ifr.ifr_curcap;
	if (available != NULL)	*available = ifr.ifr_reqcap;

	ok = TRUE;

    done :

	(void)close(sock);
	return ok;
}


CFTypeRef
SCNetworkInterfaceCopyCapability(SCNetworkInterfaceRef	interface,
				 CFStringRef		capability)
{
	int		cap_current	= 0;
	int		cap_available	= 0;
	int		cap_val;
	CFIndex		i;
	CFStringRef	interfaceName;
	CFTypeRef	val		= NULL;

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	interfaceName = SCNetworkInterfaceGetBSDName(interface);
	if (interfaceName == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (!__getCapabilities(interfaceName, &cap_current, &cap_available)) {
		return NULL;
	}

	if (capability == NULL) {
		CFMutableDictionaryRef	all	= NULL;

		// if ALL capabilities requested
		for (i = 0; i < sizeof(capabilityMappings) / sizeof(capabilityMappings[0]); i++) {
			if ((cap_available & capabilityMappings[i].val) == capabilityMappings[i].val) {
				if (all == NULL) {
					all = CFDictionaryCreateMutable(NULL,
									0,
									&kCFTypeDictionaryKeyCallBacks,
									&kCFTypeDictionaryValueCallBacks);
				}
				cap_val = ((cap_current & capabilityMappings[i].val) == capabilityMappings[i].val) ? 1 : 0;
				val = CFNumberCreate(NULL, kCFNumberIntType, &cap_val);
				CFDictionarySetValue(all, capabilityMappings[i].name, val);
				CFRelease(val);
				cap_available &= ~capabilityMappings[i].val;
			}
		}

		val = all;
	} else {
		i = findCapability(capability);
		if (i == kCFNotFound) {
			// if unknown capability
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}

		if ((cap_available & capabilityMappings[i].val) != capabilityMappings[i].val) {
			// if capability not available
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}

		cap_val = ((cap_current & capabilityMappings[i].val) == capabilityMappings[i].val) ? 1 : 0;
		val = CFNumberCreate(NULL, kCFNumberIntType, &cap_val);
	}

	return val;
}


Boolean
SCNetworkInterfaceSetCapability(SCNetworkInterfaceRef	interface,
				CFStringRef		capability,
				CFTypeRef		newValue)
{
	int			cap_available		= 0;
	CFDictionaryRef		configuration;
	CFIndex			i;
	CFStringRef		interfaceName;
	CFMutableDictionaryRef	newConfiguration	= NULL;
	Boolean			ok			= FALSE;

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	interfaceName = SCNetworkInterfaceGetBSDName(interface);
	if (interfaceName == NULL) {
		// if no interface name
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	i = findCapability(capability);
	if (i == kCFNotFound) {
		// if unknown capability
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (!capabilityMappings[i].readwrite) {
		// if not read-write
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if ((newValue != NULL) && !isA_CFNumber(newValue)) {
		// all values must (for now) be CFNumber[0 or 1]'s
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (!__getCapabilities(interfaceName, NULL, &cap_available)) {
		return FALSE;
	}

	if ((cap_available & capabilityMappings[i].val) == 0) {
		// if capability not available
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	configuration = SCNetworkInterfaceGetConfiguration(interface);
	if (configuration == NULL) {
		newConfiguration = CFDictionaryCreateMutable(NULL,
							     0,
							     &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
	} else {
		newConfiguration = CFDictionaryCreateMutableCopy(NULL, 0, configuration);
		CFDictionaryRemoveValue(newConfiguration, kSCResvInactive);
	}

	if ((newValue != NULL)) {
		CFDictionarySetValue(newConfiguration, capability, newValue);
	} else {
		CFDictionaryRemoveValue(newConfiguration, capability);
		if (CFDictionaryGetCount(newConfiguration) == 0) {
			CFRelease(newConfiguration);
			newConfiguration = NULL;
		}
	}

	ok = SCNetworkInterfaceSetConfiguration(interface, newConfiguration);
	if (newConfiguration != NULL) CFRelease(newConfiguration);

	return ok;
}


#pragma mark -
#pragma mark Media Options


static const struct ifmedia_description ifm_subtype_shared_descriptions[] =
    IFM_SUBTYPE_SHARED_DESCRIPTIONS;

static const struct ifmedia_description ifm_subtype_ethernet_descriptions[] =
    IFM_SUBTYPE_ETHERNET_DESCRIPTIONS;

static const struct ifmedia_description ifm_subtype_ieee80211_descriptions[] =
    IFM_SUBTYPE_IEEE80211_DESCRIPTIONS;

static const struct ifmedia_description ifm_shared_option_descriptions[] =
    IFM_SHARED_OPTION_DESCRIPTIONS;

static const struct ifmedia_description ifm_subtype_ethernet_option_descriptions[] =
    IFM_SUBTYPE_ETHERNET_OPTION_DESCRIPTIONS;

static const struct ifmedia_description ifm_subtype_ieee80211_option_descriptions[] =
    IFM_SUBTYPE_IEEE80211_OPTION_DESCRIPTIONS;


static void
__freeMediaList(struct ifmediareq *ifm)
{
	if (ifm->ifm_ulist != NULL) CFAllocatorDeallocate(NULL, ifm->ifm_ulist);
	CFAllocatorDeallocate(NULL, ifm);
	return;
}


static struct ifmediareq *
__copyMediaList(CFStringRef interfaceName)
{
	struct ifmediareq	*ifm;
	Boolean			ok	= FALSE;
	int			sock	= -1;

	ifm = (struct ifmediareq *)CFAllocatorAllocate(NULL, sizeof(struct ifmediareq), 0);
	bzero((void *)ifm, sizeof(*ifm));

	if (_SC_cfstring_to_cstring(interfaceName, ifm->ifm_name, sizeof(ifm->ifm_name), kCFStringEncodingASCII) == NULL) {
		goto done;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		goto done;
	}

	if (ioctl(sock, SIOCGIFMEDIA, (caddr_t)ifm) == -1) {
		goto done;
	}

	if (ifm->ifm_count > 0) {
		ifm->ifm_ulist = (int *)CFAllocatorAllocate(NULL, ifm->ifm_count * sizeof(int), 0);
		if (ioctl(sock, SIOCGIFMEDIA, (caddr_t)ifm) == -1) {
			goto done;
		}
	}

	ok = TRUE;

    done :

	if (sock != -1)	(void)close(sock);
	if (!ok) {
		__freeMediaList(ifm);
		ifm = NULL;
		_SCErrorSet(kSCStatusFailed);
	}
	return ifm;
}


static CFDictionaryRef
__createMediaDictionary(int media_options, Boolean filter)
{
	CFMutableDictionaryRef			dict			= NULL;
	int					i;
	const struct ifmedia_description	*option_descriptions	= NULL;
	CFMutableArrayRef			options			= NULL;
	const struct ifmedia_description	*subtype_descriptions	= NULL;
	CFStringRef				val;

	if (filter &&
	    ((IFM_SUBTYPE(media_options) == IFM_NONE) ||
	     ((IFM_OPTIONS(media_options) & IFM_LOOP) != 0))) {
		return NULL;	/* filter */
	}

	switch (IFM_TYPE(media_options)) {
		case IFM_ETHER :
			option_descriptions  = ifm_subtype_ethernet_option_descriptions;
			subtype_descriptions = ifm_subtype_ethernet_descriptions;
			break;
		case IFM_IEEE80211 :
			option_descriptions  = ifm_subtype_ieee80211_option_descriptions;
			subtype_descriptions = ifm_subtype_ieee80211_descriptions;
			break;
		default :
			return NULL;
	}

	dict = CFDictionaryCreateMutable(NULL,
					 0,
					 &kCFTypeDictionaryKeyCallBacks,
					 &kCFTypeDictionaryValueCallBacks);

	/* subtype */

	val = NULL;
	for (i = 0; !val && ifm_subtype_shared_descriptions[i].ifmt_string; i++) {
		if (IFM_SUBTYPE(media_options) == ifm_subtype_shared_descriptions[i].ifmt_word) {
			val = CFStringCreateWithCString(NULL,
							ifm_subtype_shared_descriptions[i].ifmt_string,
							kCFStringEncodingASCII);
			break;
		}
	}

	if (subtype_descriptions != NULL) {
		for (i = 0; !val && subtype_descriptions[i].ifmt_string; i++) {
			if (IFM_SUBTYPE(media_options) == subtype_descriptions[i].ifmt_word) {
				val = CFStringCreateWithCString(NULL,
								subtype_descriptions[i].ifmt_string,
								kCFStringEncodingASCII);
				break;
			}
		}
	}

	if (val) {
		CFDictionaryAddValue(dict, kSCPropNetEthernetMediaSubType, val);
		CFRelease(val);
	}

	/* options */

	options = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	for (i = 0; (IFM_OPTIONS(media_options) != 0) && (ifm_shared_option_descriptions[i].ifmt_string != NULL); i++) {
		if ((media_options & ifm_shared_option_descriptions[i].ifmt_word) != 0) {
			val = CFStringCreateWithCString(NULL,
							ifm_shared_option_descriptions[i].ifmt_string,
							kCFStringEncodingASCII);
			CFArrayAppendValue(options, val);
			CFRelease(val);

			media_options &= ~ifm_shared_option_descriptions[i].ifmt_word;
		}
	}

	if (option_descriptions != NULL) {
		for (i = 0; (IFM_OPTIONS(media_options) != 0) && (option_descriptions[i].ifmt_string != NULL); i++) {
			if ((media_options & option_descriptions[i].ifmt_word) != 0) {
				val = CFStringCreateWithCString(NULL,
								option_descriptions[i].ifmt_string,
								kCFStringEncodingASCII);
				CFArrayAppendValue(options, val);
				CFRelease(val);

				media_options &= ~option_descriptions[i].ifmt_word;
			}
		}
	}

	CFDictionaryAddValue(dict, kSCPropNetEthernetMediaOptions, options);
	CFRelease(options);

	return dict;
}


Boolean
SCNetworkInterfaceCopyMediaOptions(SCNetworkInterfaceRef	interface,
				   CFDictionaryRef		*current,
				   CFDictionaryRef		*active,
				   CFArrayRef			*available,
				   Boolean			filter)
{
	int			i;
	struct ifmediareq	*ifm;
	CFStringRef		interfaceName;

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	interfaceName = SCNetworkInterfaceGetBSDName(interface);
	if (interfaceName == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	ifm = __copyMediaList(interfaceName);
	if (ifm == NULL) {
		return FALSE;
	}

	if (active != NULL)	*active    = NULL;
	if (current != NULL)	*current   = NULL;
	if (available != NULL) {
		CFMutableArrayRef	media_options;

		media_options = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		for (i = 0; i < ifm->ifm_count; i++) {
			CFDictionaryRef	options;

			options = __createMediaDictionary(ifm->ifm_ulist[i], filter);
			if (options == NULL) {
				continue;
			}

			if ((active != NULL)  && (*active == NULL)  && (ifm->ifm_active == ifm->ifm_ulist[i])) {
				*active  = CFRetain(options);
			}

			if ((current != NULL) && (*current == NULL) && (ifm->ifm_current == ifm->ifm_ulist[i])) {
				*current = CFRetain(options);
			}

			if (!CFArrayContainsValue(media_options, CFRangeMake(0, CFArrayGetCount(media_options)), options)) {
				CFArrayAppendValue(media_options, options);
			}

			CFRelease(options);
		}
		*available = (CFArrayRef)media_options;
	}

	if ((active != NULL)  && (*active == NULL)) {
		*active = __createMediaDictionary(ifm->ifm_active, FALSE);
	}

	if ((current != NULL) && (*current == NULL)) {
		if ((active != NULL) && (ifm->ifm_active == ifm->ifm_current)) {
			if (*active != NULL)	*current = CFRetain(*active);
		} else {
			*current = __createMediaDictionary(ifm->ifm_current, FALSE);
		}
	}

	__freeMediaList(ifm);
	return TRUE;
}


CFArrayRef
SCNetworkInterfaceCopyMediaSubTypes(CFArrayRef	available)
{
	CFIndex			i;
	CFIndex			n;
	CFMutableArrayRef	subTypes;

	if (!isA_CFArray(available)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	subTypes = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	n = CFArrayGetCount(available);
	for (i = 0; i < n; i++) {
		CFDictionaryRef	options;
		CFStringRef	subType;

		options = CFArrayGetValueAtIndex(available, i);
		if (!isA_CFDictionary(options)) {
			continue;
		}

		subType = CFDictionaryGetValue(options, kSCPropNetEthernetMediaSubType);
		if (!isA_CFString(subType)) {
			continue;
		}

		if (!CFArrayContainsValue(subTypes, CFRangeMake(0, CFArrayGetCount(subTypes)), subType)) {
			CFArrayAppendValue(subTypes, subType);
		}
	}

	if (CFArrayGetCount(subTypes) == 0) {
		CFRelease(subTypes);
		subTypes = NULL;
		_SCErrorSet(kSCStatusOK);
	}

	return subTypes;
}


CFArrayRef
SCNetworkInterfaceCopyMediaSubTypeOptions(CFArrayRef	available,
					  CFStringRef	subType)
{
	CFIndex			i;
	CFIndex			n;
	CFMutableArrayRef	subTypeOptions;

	if (!isA_CFArray(available)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	subTypeOptions = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	n = CFArrayGetCount(available);
	for (i = 0; i < n; i++) {
		CFDictionaryRef	options;
		CFArrayRef	mediaOptions;
		CFStringRef	mediaSubType;

		options = CFArrayGetValueAtIndex(available, i);
		if (!isA_CFDictionary(options)) {
			continue;
		}

		mediaSubType = CFDictionaryGetValue(options, kSCPropNetEthernetMediaSubType);
		if (!isA_CFString(mediaSubType) || !CFEqual(subType, mediaSubType)) {
			continue;
		}

		mediaOptions = CFDictionaryGetValue(options, kSCPropNetEthernetMediaOptions);
		if (!isA_CFArray(mediaOptions)) {
			continue;
		}

		if (!CFArrayContainsValue(subTypeOptions, CFRangeMake(0, CFArrayGetCount(subTypeOptions)), mediaOptions)) {
			CFArrayAppendValue(subTypeOptions, mediaOptions);
		}
	}

	if (CFArrayGetCount(subTypeOptions) == 0) {
		CFRelease(subTypeOptions);
		subTypeOptions = NULL;
		_SCErrorSet(kSCStatusOK);
	}

	return subTypeOptions;
}


Boolean
_SCNetworkInterfaceIsPhysicalEthernet(SCNetworkInterfaceRef interface)
{
	int			i;
	struct ifmediareq	*ifm;
	CFStringRef		interfaceName;
	Boolean			realEthernet = FALSE;

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	interfaceName = SCNetworkInterfaceGetBSDName(interface);
	if (interfaceName == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	ifm = __copyMediaList(interfaceName);
	if (ifm == NULL) {
		CFStringRef interfaceType;

		interfaceType = SCNetworkInterfaceGetInterfaceType(interface);
		if (CFEqual(interfaceType, kSCNetworkInterfaceTypeEthernet)) {
		    // if likely physical ethernet interface
		    return TRUE;
		}
		return FALSE;
	}
	_SCErrorSet(kSCStatusOK);
	if (IFM_TYPE(ifm->ifm_current) != IFM_ETHER) {
		goto done;
	}
	if (ifm->ifm_count == 1
	    && IFM_SUBTYPE(ifm->ifm_ulist[0]) == IFM_AUTO) {
		/* only support autoselect, not really ethernet */
		goto done;
	}
	for (i = 0; i < ifm->ifm_count; i++) {
		if ((ifm->ifm_ulist[i] & IFM_FDX) != 0) {
			realEthernet = TRUE;
			break;
		}
	}
 done:
	__freeMediaList(ifm);
	return (realEthernet);
}


Boolean
SCNetworkInterfaceCopyMTU(SCNetworkInterfaceRef	interface,
			  int			*mtu_cur,
			  int			*mtu_min,
			  int			*mtu_max)
{
	struct ifreq	ifr;
	CFStringRef	interfaceName;
	Boolean		ok		= FALSE;
	int		sock		= -1;

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	interfaceName = SCNetworkInterfaceGetBSDName(interface);
	if (interfaceName == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	bzero((void *)&ifr, sizeof(ifr));
	if (_SC_cfstring_to_cstring(interfaceName, ifr.ifr_name, sizeof(ifr.ifr_name), kCFStringEncodingASCII) == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		_SCErrorSet(errno);
		return FALSE;
	}

	if (ioctl(sock, SIOCGIFMTU, (caddr_t)&ifr) == -1) {
		_SCErrorSet(errno);
		goto done;
	}

	/* no SIOCGIFDEVMTU: without a device range, min and max are current */
	if (mtu_cur)	*mtu_cur = ifr.ifr_mtu;
	if (mtu_min)	*mtu_min = ifr.ifr_mtu;
	if (mtu_max)	*mtu_max = ifr.ifr_mtu;

	ok = TRUE;

    done :

	(void)close(sock);
	return ok;
}
