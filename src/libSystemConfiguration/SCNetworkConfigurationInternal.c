/*
 * SCNetworkConfigurationInternal.c — definitions shared across the
 * SCNetworkConfiguration sources (freebsd-launchd-mach port).
 *
 * This file defines the kSCNetworkInterfaceType* and kSCNetworkProtocol
 * Type* constant CFStrings. Apple spreads these across SCNetworkInterface.c
 * and SCNetworkProtocol.c; collecting them here keeps every constant in
 * one translation unit as the object families land iteration by
 * iteration. The string values are the ones written into the preferences
 * plist, so they match Apple's exactly.
 */

#include "SCNetworkConfigurationInternal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCNetworkConfiguration.h>

const CFStringRef kSCNetworkInterfaceType6to4		= CFSTR("6to4");
const CFStringRef kSCNetworkInterfaceTypeBluetooth	= CFSTR("Bluetooth");
const CFStringRef kSCNetworkInterfaceTypeBond		= CFSTR("Bond");
const CFStringRef kSCNetworkInterfaceTypeBridge		= CFSTR("Bridge");
const CFStringRef kSCNetworkInterfaceTypeEthernet	= CFSTR("Ethernet");
const CFStringRef kSCNetworkInterfaceTypeFireWire	= CFSTR("FireWire");
const CFStringRef kSCNetworkInterfaceTypeIEEE80211	= CFSTR("IEEE80211");
const CFStringRef kSCNetworkInterfaceTypeIPSec		= CFSTR("IPSec");
const CFStringRef kSCNetworkInterfaceTypeL2TP		= CFSTR("L2TP");
const CFStringRef kSCNetworkInterfaceTypeLoopback	= CFSTR("Loopback");
const CFStringRef kSCNetworkInterfaceTypeModem		= CFSTR("Modem");
const CFStringRef kSCNetworkInterfaceTypePPP		= CFSTR("PPP");
const CFStringRef kSCNetworkInterfaceTypeSerial		= CFSTR("Serial");
const CFStringRef kSCNetworkInterfaceTypeVLAN		= CFSTR("VLAN");
const CFStringRef kSCNetworkInterfaceTypeWWAN		= CFSTR("WWAN");
const CFStringRef kSCNetworkInterfaceTypeIPv4		= CFSTR("IPv4");

const CFStringRef kSCNetworkProtocolTypeDNS		= CFSTR("DNS");
const CFStringRef kSCNetworkProtocolTypeIPv4		= CFSTR("IPv4");
const CFStringRef kSCNetworkProtocolTypeIPv6		= CFSTR("IPv6");
const CFStringRef kSCNetworkProtocolTypeProxies		= CFSTR("Proxies");
const CFStringRef kSCNetworkProtocolTypeSMB		= CFSTR("SMB");


CFStringRef
__SCNetworkServiceEntityPath(CFStringRef serviceID, CFStringRef entity)
{
	if (entity == NULL) {
		return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
						kSCPrefNetworkServices,
						serviceID);
	}
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@/%@"),
					kSCPrefNetworkServices,
					serviceID, entity);
}


#pragma mark -
#pragma mark Configuration entities

CFDictionaryRef
__getPrefsConfiguration(SCPreferencesRef prefs, CFStringRef path)
{
	CFDictionaryRef config;
	CFIndex		n;

	config = SCPreferencesPathGetValue(prefs, path);

	n = isA_CFDictionary(config) ? CFDictionaryGetCount(config) : 0;
	switch (n) {
		case 0 :
			// ignore empty configuration entities
			config = NULL;
			break;
		case 1 :
			if (CFDictionaryContainsKey(config, kSCResvInactive)) {
				// ignore [effectively] empty configuration entities
				config = NULL;
			}
			break;
		default :
			break;
	}

	return config;
}


Boolean
__setPrefsConfiguration(SCPreferencesRef	prefs,
			CFStringRef		path,
			CFDictionaryRef		config,
			Boolean			keepInactive)
{
	CFDictionaryRef		curConfig;
	CFMutableDictionaryRef	newConfig	= NULL;
	Boolean			ok;

	if ((config != NULL) && !isA_CFDictionary(config)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	curConfig = SCPreferencesPathGetValue(prefs, path);

	if (config != NULL) {
		newConfig = CFDictionaryCreateMutableCopy(NULL, 0, config);
	}

	if (keepInactive) {
		if (config == NULL) {
			newConfig = CFDictionaryCreateMutable(NULL,
							      0,
							      &kCFTypeDictionaryKeyCallBacks,
							      &kCFTypeDictionaryValueCallBacks);
		}

		if (isA_CFDictionary(curConfig) && CFDictionaryContainsKey(curConfig, kSCResvInactive)) {
			// if currently disabled
			CFDictionarySetValue(newConfig, kSCResvInactive, kCFBooleanTrue);
		} else {
			// if currently enabled
			CFDictionaryRemoveValue(newConfig, kSCResvInactive);
		}
	}

	// set new configuration
	if (_SC_CFEqual(curConfig, newConfig)) {
		// if no change
		if (newConfig != NULL) CFRelease(newConfig);
		ok = TRUE;
	} else if (newConfig != NULL) {
		// if new configuration (or we are preserving a disabled state)
		ok = SCPreferencesPathSetValue(prefs, path, newConfig);
		CFRelease(newConfig);
	} else {
		ok = SCPreferencesPathRemoveValue(prefs, path);
		if (!ok && (SCError() == kSCStatusNoKey)) {
			ok = TRUE;
		}
	}

	return ok;
}


#pragma mark -
#pragma mark Protocol templates

/*
 * The default protocol configuration, compiled in because the library
 * has no bundle to carry Apple's NetworkConfiguration.plist. These are
 * the plist's "Protocol" values, which are the same for every interface
 * type listed; any other type or protocol has no template (Apple's plist
 * has no SMB template either, so SMB starts out empty).
 */
CFDictionaryRef
__copyProtocolTemplate(CFStringRef interfaceType, CFStringRef protocolType)
{
	static const CFStringRef * const	types[] = {
		&kSCNetworkInterfaceTypeBond,
		&kSCNetworkInterfaceTypeBridge,
		&kSCNetworkInterfaceTypeEthernet,
		&kSCNetworkInterfaceTypeFireWire,
		&kSCNetworkInterfaceTypeIEEE80211,
		&kSCNetworkInterfaceTypeVLAN,
	};
	CFMutableDictionaryRef	template;
	Boolean			found	= FALSE;
	size_t			i;

	for (i = 0; i < (sizeof(types) / sizeof(types[0])); i++) {
		if (CFEqual(interfaceType, *types[i])) {
			found = TRUE;
			break;
		}
	}
	if (!found) {
		return NULL;
	}

	if (!CFEqual(protocolType, kSCNetworkProtocolTypeDNS) &&
	    !CFEqual(protocolType, kSCNetworkProtocolTypeIPv4) &&
	    !CFEqual(protocolType, kSCNetworkProtocolTypeIPv6) &&
	    !CFEqual(protocolType, kSCNetworkProtocolTypeProxies)) {
		return NULL;
	}

	template = CFDictionaryCreateMutable(NULL, 0,
					     &kCFTypeDictionaryKeyCallBacks,
					     &kCFTypeDictionaryValueCallBacks);
	if (CFEqual(protocolType, kSCNetworkProtocolTypeIPv4)) {
		CFDictionarySetValue(template, kSCPropNetIPv4ConfigMethod,
				     kSCValNetIPv4ConfigMethodDHCP);
	} else if (CFEqual(protocolType, kSCNetworkProtocolTypeIPv6)) {
		CFDictionarySetValue(template, kSCPropNetIPv6ConfigMethod,
				     kSCValNetIPv6ConfigMethodAutomatic);
	} else if (CFEqual(protocolType, kSCNetworkProtocolTypeProxies)) {
		const void	*exceptions[2];
		CFArrayRef	list;
		CFNumberRef	num;
		int		one	= 1;

		exceptions[0] = CFSTR("*.local");
		exceptions[1] = CFSTR("169.254/16");
		list = CFArrayCreate(NULL, exceptions, 2, &kCFTypeArrayCallBacks);
		CFDictionarySetValue(template, kSCPropNetProxiesExceptionsList,
				     list);
		CFRelease(list);

		num = CFNumberCreate(NULL, kCFNumberIntType, &one);
		CFDictionarySetValue(template, kSCPropNetProxiesFTPPassive, num);
		CFRelease(num);
	}
	return template;
}


#pragma mark -
#pragma mark PrimaryRank

Boolean
__rank_to_str(SCNetworkServicePrimaryRank rank, CFStringRef *rankStr)
{
	switch (rank) {
		case kSCNetworkServicePrimaryRankDefault :
			*rankStr = NULL;
			break;
		case kSCNetworkServicePrimaryRankFirst :
			*rankStr = kSCValNetServicePrimaryRankFirst;
			break;
		case kSCNetworkServicePrimaryRankLast :
			*rankStr = kSCValNetServicePrimaryRankLast;
			break;
		case kSCNetworkServicePrimaryRankNever :
			*rankStr = kSCValNetServicePrimaryRankNever;
			break;
		default :
			return FALSE;
	}

	return TRUE;
}


Boolean
__str_to_rank(CFStringRef rankStr, SCNetworkServicePrimaryRank *rank)
{
	if (isA_CFString(rankStr)) {
		if (CFEqual(rankStr, kSCValNetServicePrimaryRankFirst)) {
			*rank = kSCNetworkServicePrimaryRankFirst;
		} else if (CFEqual(rankStr, kSCValNetServicePrimaryRankLast)) {
			*rank = kSCNetworkServicePrimaryRankLast;
		} else if (CFEqual(rankStr, kSCValNetServicePrimaryRankNever)) {
			*rank = kSCNetworkServicePrimaryRankNever;
		} else {
			return FALSE;
		}
	} else if (rankStr == NULL) {
		*rank = kSCNetworkServicePrimaryRankDefault;
	} else {
		return FALSE;
	}

	return TRUE;
}
