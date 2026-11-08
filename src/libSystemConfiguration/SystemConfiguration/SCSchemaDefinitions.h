/*
 * Copyright (c) 2000-2023 Apple Inc. All rights reserved.
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
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * SCSchemaDefinitions.h — the canonical SCDynamicStore + SCPreferences
 * schema constant names. freebsd-launchd-mach port of Apple's
 * SystemConfiguration.framework SCSchemaDefinitions.h, populated as the
 * surface our daemons consume grows.
 *
 * Constants are CFSTR'd strings whose exact spelling is part of Apple's
 * stable ABI — every SC-aware daemon (configd, IPMonitor, mDNSResponder,
 * Setup Assistant, ...) reads/writes by these names. Do not edit values;
 * they are sync'd against
 * apple-oss-distributions/configd/SystemConfiguration.fproj/
 * SCSchemaDefinitions.h.
 *
 * Only the subset our in-tree consumers need is exported. Add more as
 * additional clients (e.g. mDNSResponder's reactive watcher, future
 * configd plugins) land.
 */

#ifndef _SCSCHEMADEFINITIONS_H
#define _SCSCHEMADEFINITIONS_H

#include <CoreFoundation/CoreFoundation.h>

/*
 * SCDynamicStore key-name domain prefixes. The store namespace is
 * partitioned into "State:/" (current runtime state, ephemeral) and
 * "Setup:/" (persistent configuration), with sub-paths for /Network/
 * /System/ etc. Apple's SCDynamicStoreKeyCreate* helpers stitch these
 * together with entity names.
 */
#define kSCDynamicStoreDomainState		CFSTR("State:")
#define kSCDynamicStoreDomainSetup		CFSTR("Setup:")
#define kSCDynamicStoreDomainFile		CFSTR("File:")
#define kSCDynamicStoreDomainPlugin		CFSTR("Plugin:")
#define kSCDynamicStoreDomainPrefs		CFSTR("Prefs:")

/*
 * Generic component names used by KeyCreate helpers. kSCCompAnyRegex
 * matches any UUID segment in a SCDynamicStore pattern subscription
 * (POSIX regex inside SCDynamicStoreSetNotificationKeys patterns).
 */
#define kSCCompNetwork				CFSTR("Network")
#define kSCCompService				CFSTR("Service")
#define kSCCompGlobal				CFSTR("Global")
#define kSCCompHostNames			CFSTR("HostNames")
#define kSCCompSystem				CFSTR("System")
#define kSCCompUsers				CFSTR("Users")
#define kSCCompInterface			CFSTR("Interface")
#define kSCCompAnyRegex				CFSTR("[^/]+")

/*
 * Network-entity names that appear as the trailing component of
 * State:/Network/Service/<UUID>/<entity> and analogous keys. Subset
 * we currently consume; populate as needed.
 */
#define kSCEntNetIPv4				CFSTR("IPv4")
#define kSCEntNetIPv6				CFSTR("IPv6")
#define kSCEntNetDHCP				CFSTR("DHCP")
#define kSCEntNetDHCPv6				CFSTR("DHCPv6")
#define kSCEntNetDNS				CFSTR("DNS")
#define kSCEntNetProxies			CFSTR("Proxies")
#define kSCEntNetInterface			CFSTR("Interface")
#define kSCEntNetLink				CFSTR("Link")

/*
 * Event entities the KernelEventMonitor publishes as the trailing
 * component of State:/Network/Interface/<ifname>/<entity> when the
 * kernel reports an IPv4 ARP collision, an IPv6 duplicate address
 * (DAD failure) or an expired IPv6 default router. Private schema in
 * Apple's tree (SCSchemaDefinitionsPrivate.h).
 */
#define kSCEntNetIPv4ARPCollision		CFSTR("IPv4ARPCollision")
#define kSCEntNetIPv6DuplicatedAddress		CFSTR("IPv6DuplicatedAddress")
#define kSCEntNetIPv6RouterExpired		CFSTR("IPv6RouterExpired")

/*
 * Property names that appear as dictionary keys inside the network-
 * entity dicts published into the SCDynamicStore.
 */
#define kSCPropNetIPv4Addresses			CFSTR("Addresses")
#define kSCPropNetIPv4SubnetMasks		CFSTR("SubnetMasks")
#define kSCPropNetIPv4Router			CFSTR("Router")
#define kSCPropNetIPv4BroadcastAddresses	CFSTR("BroadcastAddresses")
#define kSCPropNetIPv6Addresses			CFSTR("Addresses")
#define kSCPropNetIPv6PrefixLength		CFSTR("PrefixLength")
#define kSCPropNetIPv6Flags			CFSTR("Flags")
#define kSCPropNetIPv6ScopeID			CFSTR("ScopeID")
#define kSCPropNetProxiesExceptionsList		CFSTR("ExceptionsList")
#define kSCPropNetProxiesExcludeSimpleHostnames	CFSTR("ExcludeSimpleHostnames")
#define kSCPropNetProxiesFTPEnable		CFSTR("FTPEnable")
#define kSCPropNetProxiesFTPPassive		CFSTR("FTPPassive")
#define kSCPropNetProxiesFTPPort		CFSTR("FTPPort")
#define kSCPropNetProxiesFTPProxy		CFSTR("FTPProxy")
#define kSCPropNetProxiesGopherEnable		CFSTR("GopherEnable")
#define kSCPropNetProxiesGopherPort		CFSTR("GopherPort")
#define kSCPropNetProxiesGopherProxy		CFSTR("GopherProxy")
#define kSCPropNetProxiesHTTPEnable		CFSTR("HTTPEnable")
#define kSCPropNetProxiesHTTPPort		CFSTR("HTTPPort")
#define kSCPropNetProxiesHTTPProxy		CFSTR("HTTPProxy")
#define kSCPropNetProxiesHTTPSEnable		CFSTR("HTTPSEnable")
#define kSCPropNetProxiesHTTPSPort		CFSTR("HTTPSPort")
#define kSCPropNetProxiesHTTPSProxy		CFSTR("HTTPSProxy")
#define kSCPropNetProxiesRTSPEnable		CFSTR("RTSPEnable")
#define kSCPropNetProxiesRTSPPort		CFSTR("RTSPPort")
#define kSCPropNetProxiesRTSPProxy		CFSTR("RTSPProxy")
#define kSCPropNetProxiesSOCKSEnable		CFSTR("SOCKSEnable")
#define kSCPropNetProxiesSOCKSPort		CFSTR("SOCKSPort")
#define kSCPropNetProxiesSOCKSProxy		CFSTR("SOCKSProxy")
#define kSCPropNetProxiesProxyAutoConfigEnable	CFSTR("ProxyAutoConfigEnable")
#define kSCPropNetProxiesProxyAutoConfigJavaScript \
						CFSTR("ProxyAutoConfigJavaScript")
#define kSCPropNetProxiesProxyAutoConfigURLString \
						CFSTR("ProxyAutoConfigURLString")
#define kSCPropNetProxiesProxyAutoDiscoveryEnable \
						CFSTR("ProxyAutoDiscoveryEnable")
/* Private schema in Apple's tree (SCSchemaDefinitionsPrivate.h). */
#define kSCPropNetProxiesFallBackAllowed	CFSTR("FallBackAllowed")
#define kSCPropNetProxiesScoped			CFSTR("__SCOPED__")
#define kSCPropNetProxiesServices		CFSTR("__SERVICES__")
#define kSCPropNetProxiesSupplemental		CFSTR("__SUPPLEMENTAL__")
#define kSCPropNetProxiesSupplementalMatchDomain CFSTR("__MATCH_DOMAIN__")
#define kSCPropNetLinkActive			CFSTR("Active")
#define kSCPropNetLinkDetaching			CFSTR("Detaching")
#define kSCPropNetInterfaceDeviceName		CFSTR("DeviceName")
#define kSCPropNetInterfaceType			CFSTR("Type")
#define kSCPropNetInterfaceHardware		CFSTR("Hardware")
#define kSCPropUserDefinedName			CFSTR("UserDefinedName")
#define kSCResvInactive				CFSTR("__INACTIVE__")
#define kSCPropSystemHostName			CFSTR("HostName")
#define kSCPropSystemComputerName		CFSTR("ComputerName")
#define kSCPropSystemComputerNameEncoding	CFSTR("ComputerNameEncoding")
#define kSCPropNetHostName			CFSTR("HostName")
#define kSCPropNetLocalHostName			CFSTR("LocalHostName")

/*
 * The rest of the network schema that the SCNetworkConfiguration
 * sources and scutil's network commands use. kSCEntNetService,
 * the capability names and the PrimaryRank values are private schema
 * in Apple's tree (SCSchemaDefinitionsPrivate.h).
 */
#define kSCEntNetAirPort			CFSTR("AirPort")
#define kSCEntNetEthernet			CFSTR("Ethernet")
#define kSCEntNetFireWire			CFSTR("FireWire")
#define kSCEntNetSMB				CFSTR("SMB")
#define kSCEntNetService			CFSTR("__SERVICE__")
#define kSCValNetInterfaceTypeLoopback		CFSTR("Loopback")

#define kSCPropNetServicePrimaryRank		CFSTR("PrimaryRank")
#define kSCValNetServicePrimaryRankFirst	CFSTR("First")
#define kSCValNetServicePrimaryRankLast		CFSTR("Last")
#define kSCValNetServicePrimaryRankNever	CFSTR("Never")

#define kSCPropNetDNSDomainName			CFSTR("DomainName")
#define kSCPropNetDNSSearchDomains		CFSTR("SearchDomains")
#define kSCPropNetDNSServerAddresses		CFSTR("ServerAddresses")
#define kSCPropNetDNSSupplementalMatchDomains	CFSTR("SupplementalMatchDomains")

#define kSCPropNetEthernetMediaSubType		CFSTR("MediaSubType")
#define kSCPropNetEthernetMediaOptions		CFSTR("MediaOptions")
#define kSCPropNetEthernetMTU			CFSTR("MTU")
#define kSCPropNetEthernetCapabilityAV		CFSTR("AV")
#define kSCPropNetEthernetCapabilityJUMBO_MTU	CFSTR("JUMBO_MTU")
#define kSCPropNetEthernetCapabilityLRO		CFSTR("LRO")
#define kSCPropNetEthernetCapabilityRXCSUM	CFSTR("RXCSUM")
#define kSCPropNetEthernetCapabilityTSO		CFSTR("TSO")
#define kSCPropNetEthernetCapabilityTSO4	CFSTR("TSO4")
#define kSCPropNetEthernetCapabilityTSO6	CFSTR("TSO6")
#define kSCPropNetEthernetCapabilityTXCSUM	CFSTR("TXCSUM")
#define kSCPropNetEthernetCapabilityVLAN_HWTAGGING CFSTR("VLAN_HWTAGGING")
#define kSCPropNetEthernetCapabilityVLAN_MTU	CFSTR("VLAN_MTU")

#define kSCPropNetIPv4ConfigMethod		CFSTR("ConfigMethod")
#define kSCPropNetIPv4DHCPClientID		CFSTR("DHCPClientID")
#define kSCValNetIPv4ConfigMethodBOOTP		CFSTR("BOOTP")
#define kSCValNetIPv4ConfigMethodDHCP		CFSTR("DHCP")
#define kSCValNetIPv4ConfigMethodINFORM		CFSTR("INFORM")
#define kSCValNetIPv4ConfigMethodLinkLocal	CFSTR("LinkLocal")
#define kSCValNetIPv4ConfigMethodManual		CFSTR("Manual")

#define kSCPropNetIPv6ConfigMethod		CFSTR("ConfigMethod")
#define kSCPropNetIPv6Router			CFSTR("Router")
#define kSCValNetIPv6ConfigMethodAutomatic	CFSTR("Automatic")
#define kSCValNetIPv6ConfigMethodManual		CFSTR("Manual")
#define kSCValNetIPv6ConfigMethodRouterAdvertisement CFSTR("RouterAdvertisement")
#define kSCValNetIPv6ConfigMethod6to4		CFSTR("6to4")

#define kSCPropNetSMBNetBIOSName		CFSTR("NetBIOSName")
#define kSCPropNetSMBNetBIOSNodeType		CFSTR("NetBIOSNodeType")
#define kSCPropNetSMBWINSAddresses		CFSTR("WINSAddresses")
#define kSCPropNetSMBWorkgroup			CFSTR("Workgroup")
#define kSCValNetSMBNetBIOSNodeTypeBroadcast	CFSTR("Broadcast")
#define kSCValNetSMBNetBIOSNodeTypeHybrid	CFSTR("Hybrid")
#define kSCValNetSMBNetBIOSNodeTypeMixed	CFSTR("Mixed")
#define kSCValNetSMBNetBIOSNodeTypePeer		CFSTR("Peer")

/*
 * State:/Network/Global/IPv4 properties. PrimaryService is the
 * service UUID of the primary network service; PrimaryInterface is
 * its BSD interface name. ipconfigd publishes these on BOUND;
 * IPMonitor's set-hostname.c and mDNSResponder read them.
 */
#define kSCDynamicStorePropNetPrimaryService	CFSTR("PrimaryService")
#define kSCDynamicStorePropNetPrimaryInterface	CFSTR("PrimaryInterface")

/*
 * State:/Users/ConsoleUser (Apple's kSCEntUsersConsoleUser and its
 * kSCPropUsersConsoleUser* properties; SessionInfo is Apple's private
 * kSCPropUsersConsoleSessionInfo). Written by the console-session owner
 * (loginwindow on Apple, /usr/libexec/consoleuser here), read by
 * diskarbitrationd (#199).
 */
#define kSCEntUsersConsoleUser			CFSTR("ConsoleUser")
#define kSCPropUsersConsoleUserName		CFSTR("Name")
#define kSCPropUsersConsoleUserUID		CFSTR("UID")
#define kSCPropUsersConsoleUserGID		CFSTR("GID")
#define kSCPropUsersConsoleSessionInfo		CFSTR("SessionInfo")

#endif	/* _SCSCHEMADEFINITIONS_H */
