/*
 * SCNetworkConfigurationInternal.h — private definitions shared by the
 * SCNetworkConfiguration sources (freebsd-launchd-mach port of Apple's
 * SystemConfiguration.fproj SCNetworkConfigurationInternal.h).
 *
 * Not installed.
 */

#ifndef _SCNETWORKCONFIGURATIONINTERNAL_H
#define _SCNETWORKCONFIGURATIONINTERNAL_H

/* first: "config.h" must be seen before <SCDynamicStore.h>, see SCInternal.h */
#include "SCInternal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>

#include <SystemConfiguration/SCNetworkConfiguration.h>
#include <SystemConfiguration/SCNetworkConfigurationPrivate.h>
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

/* Apple's SCValidation.h checks: the argument if of that type, else NULL */
static inline CFTypeRef
__isA_CFType(CFTypeRef obj, CFTypeID type)
{
	return ((obj != NULL) && (CFGetTypeID(obj) == type)) ? obj : NULL;
}
#define isA_CFArray(obj)	((CFArrayRef)__isA_CFType((obj), CFArrayGetTypeID()))
#define isA_CFDictionary(obj)	((CFDictionaryRef)__isA_CFType((obj), CFDictionaryGetTypeID()))
#define isA_CFNumber(obj)	((CFNumberRef)__isA_CFType((obj), CFNumberGetTypeID()))
#define isA_CFString(obj)	((CFStringRef)__isA_CFType((obj), CFStringGetTypeID()))

/* NULL-safe CFEqual: both NULL is equal, one NULL is not */
static inline Boolean
_SC_CFEqual(CFTypeRef val1, CFTypeRef val2)
{
	if (val1 == val2) {
		return TRUE;
	}
	if ((val1 == NULL) || (val2 == NULL)) {
		return FALSE;
	}
	return CFEqual(val1, val2);
}

/*
 * Reserved preferences-plist keys. The network configuration lives in
 * the preferences plist under /NetworkServices/<serviceID>, each
 * service dict carrying an "Interface" entity, a "UserDefinedName", and
 * one sub-dict per configured protocol. A "__INACTIVE__" key marks a
 * service or protocol disabled.
 *
 * Schema-shared constants (kSCCompNetwork, kSCCompService, kSCCompGlobal,
 * kSCEntNetInterface, kSCEntNetIPv4, kSCPropUserDefinedName,
 * kSCPropNetInterfaceType, kSCPropNetInterfaceDeviceName) now live in
 * <SystemConfiguration/SCSchemaDefinitions.h> alongside the rest of the
 * Apple-canonical SCDynamicStore schema; the include above pulls them
 * in. Only prefs-plist-specific keys stay below.
 */
#define kSCPrefNetworkServices		CFSTR("NetworkServices")
#define kSCPrefSets			CFSTR("Sets")
#define kSCPrefCurrentSet		CFSTR("CurrentSet")
#define kSCPropNetServiceOrder		CFSTR("ServiceOrder")

/*
 * Virtual-interface (Bond / Bridge / VLAN) storage. The virtual
 * interfaces live under /VirtualNetworkInterfaces/<type>/<bsdName> in
 * the preferences plist.
 */
#define kSCPrefVirtualNetworkInterfaces	CFSTR("VirtualNetworkInterfaces")
#define kSCPropVLANInterface		CFSTR("Interface")
#define kSCPropVLANTag			CFSTR("Tag")
#define kSCPropVirtualInterfaces	CFSTR("Interfaces")
#define kSCPropVirtualOptions		CFSTR("Options")


#pragma mark -
#pragma mark SCNetworkInterface

/*
 * The SCNetworkInterface object — see SCNetworkInterface.c. A leaf
 * (hardware) interface is the common case; `interface` is non-NULL only
 * for a layered interface. `prefs` / `serviceID` are set when the
 * interface is bound to a stored network service.
 */
typedef struct __SCNetworkInterface {
	CFRuntimeBase		cfBase;

	CFStringRef		interface_type;		/* kSCNetworkInterfaceType* */
	CFStringRef		entity_device;		/* BSD name, e.g. "en0" */

	CFStringRef		name;			/* non-localized */
	CFStringRef		localized_name;		/* localized */

	CFDataRef		address;		/* raw link-layer bytes */
	CFStringRef		addressString;		/* "xx:xx:..." */
	Boolean			builtin;

	SCNetworkInterfaceRef	interface;		/* layered-on, or NULL */

	SCPreferencesRef	prefs;			/* bound prefs, or NULL */
	SCDynamicStoreRef	store;			/* active handle's session, or NULL */
	CFStringRef		serviceID;		/* bound service, or NULL */
	CFStringRef		entity_type;		/* prefs entity "Type" */
	CFStringRef		entity_subtype;		/* prefs entity "SubType" */

	CFArrayRef		supported_interface_types;
	CFArrayRef		supported_protocol_types;

	/* virtual-interface detail (Bond / Bridge / VLAN interface types) */
	CFArrayRef		member_interfaces;	/* Bond / Bridge members */
	SCNetworkInterfaceRef	vlan_physical;		/* VLAN physical interface */
	CFNumberRef		vlan_tag;		/* VLAN tag (1..4094) */
	CFStringRef		bond_mode;		/* Bond aggregation mode */
	CFDictionaryRef		virtual_options;	/* Bond / Bridge / VLAN options */
} SCNetworkInterfacePrivate, *SCNetworkInterfacePrivateRef;


#pragma mark -
#pragma mark SCNetworkService

/*
 * The SCNetworkService object — see SCNetworkService.c. A network
 * service binds an interface to the protocols configured on it; it is
 * persisted at /NetworkServices/<serviceID> in the preferences plist.
 */
typedef struct __SCNetworkService {
	CFRuntimeBase		cfBase;

	CFStringRef		serviceID;
	SCNetworkInterfaceRef	interface;	/* lazily reconstructed */
	SCPreferencesRef	prefs;
	SCDynamicStoreRef	store;		/* active handle's session, or NULL */
	CFStringRef		name;		/* cached UserDefinedName */
} SCNetworkServicePrivate, *SCNetworkServicePrivateRef;


#pragma mark -
#pragma mark SCNetworkProtocol

/*
 * The SCNetworkProtocol object — see SCNetworkProtocol.c. One protocol
 * (IPv4, IPv6, DNS, Proxies, SMB) configured on a network service;
 * persisted at /NetworkServices/<serviceID>/<entityID>.
 */
typedef struct __SCNetworkProtocol {
	CFRuntimeBase		cfBase;

	CFStringRef		entityID;	/* the protocol type */
	SCNetworkServiceRef	service;
} SCNetworkProtocolPrivate, *SCNetworkProtocolPrivateRef;


#pragma mark -
#pragma mark SCNetworkSet

/*
 * The SCNetworkSet object — see SCNetworkSet.c. A set ("location") is an
 * ordered collection of network services; persisted at /Sets/<setID> in
 * the preferences plist, with the active set named by the top-level
 * "CurrentSet" key.
 */
typedef struct __SCNetworkSet {
	CFRuntimeBase		cfBase;

	CFStringRef		setID;
	SCPreferencesRef	prefs;
	CFStringRef		name;		/* cached UserDefinedName */
	Boolean			established;	/* TRUE unless newly created */
} SCNetworkSetPrivate, *SCNetworkSetPrivateRef;


#pragma mark -
#pragma mark Type checks

static inline Boolean
isA_SCNetworkInterface(SCNetworkInterfaceRef interface)
{
	return ((interface != NULL) &&
		(CFGetTypeID(interface) == SCNetworkInterfaceGetTypeID()));
}

static inline Boolean
isA_SCNetworkService(SCNetworkServiceRef service)
{
	return ((service != NULL) &&
		(CFGetTypeID(service) == SCNetworkServiceGetTypeID()));
}

static inline Boolean
isA_SCNetworkProtocol(SCNetworkProtocolRef protocol)
{
	return ((protocol != NULL) &&
		(CFGetTypeID(protocol) == SCNetworkProtocolGetTypeID()));
}

static inline Boolean
isA_SCNetworkSet(SCNetworkSetRef set)
{
	return ((set != NULL) &&
		(CFGetTypeID(set) == SCNetworkSetGetTypeID()));
}


#pragma mark -
#pragma mark Internal helpers

/* SCNetworkInterface.c — allocate a bare interface (all fields NULL). */
SCNetworkInterfacePrivateRef
__SCNetworkInterfaceCreatePrivate	(CFAllocatorRef		allocator);

/*
 * SCNetworkInterface.c — a fresh CFArray of the protocol types that can
 * be configured on a network interface (IPv4/IPv6/DNS/Proxies/SMB).
 */
CFArrayRef
__SCNetworkInterfaceCopyProtocolTypes	(void);

/*
 * SCNetworkInterface.c — synthesize an Ethernet interface for a BSD
 * name (no hardware address). Used to rebuild the physical interface a
 * stored virtual interface references.
 */
SCNetworkInterfaceRef
__SCNetworkInterfaceCreateWithBSDName	(CFAllocatorRef		allocator,
					 CFStringRef		bsdName);

/*
 * SCNetworkInterface.c — the interface <-> preferences-entity bridge.
 * __SCNetworkInterfaceCopyInterfaceEntity serializes an interface into
 * the dictionary stored at a service's "Interface" path; _SCNetworkInter
 * faceCreateWithEntity rebuilds an interface from that dictionary.
 */
CFDictionaryRef
__SCNetworkInterfaceCopyInterfaceEntity	(SCNetworkInterfaceRef	interface);

SCNetworkInterfaceRef
_SCNetworkInterfaceCreateWithEntity	(CFAllocatorRef		allocator,
					 CFDictionaryRef	entity,
					 SCNetworkServiceRef	service);

/* SCNetworkService.c — allocate a service object. */
SCNetworkServicePrivateRef
__SCNetworkServiceCreatePrivate		(CFAllocatorRef		allocator,
					 SCPreferencesRef	prefs,
					 CFStringRef		serviceID,
					 SCNetworkInterfaceRef	interface);

/* SCNetworkProtocol.c — allocate a protocol object. */
SCNetworkProtocolPrivateRef
__SCNetworkProtocolCreatePrivate	(CFAllocatorRef		allocator,
					 CFStringRef		entityID,
					 SCNetworkServiceRef	service);

/* SCNetworkSet.c — allocate a set object. */
SCNetworkSetPrivateRef
__SCNetworkSetCreatePrivate		(CFAllocatorRef		allocator,
					 SCPreferencesRef	prefs,
					 CFStringRef		setID);

/* SCNetworkProtocol.c — TRUE iff `protocolType` is a known protocol. */
Boolean
__SCNetworkProtocolIsValidType		(CFStringRef		protocolType);

/*
 * SCNetworkConfigurationInternal.c — build a preferences path for a
 * network service or one of its entities: entity NULL gives
 * "/NetworkServices/<serviceID>", otherwise
 * "/NetworkServices/<serviceID>/<entity>". Caller releases.
 */
CFStringRef
__SCNetworkServiceEntityPath		(CFStringRef		serviceID,
					 CFStringRef		entity);

/*
 * SCNetworkConfigurationInternal.c — read a configuration entity, with
 * an empty one reported as NULL, and write one (NULL removes it).
 */
CFDictionaryRef
__getPrefsConfiguration			(SCPreferencesRef	prefs,
					 CFStringRef		path);

Boolean
__setPrefsConfiguration			(SCPreferencesRef	prefs,
					 CFStringRef		path,
					 CFDictionaryRef	config,
					 Boolean		keepInactive);

/*
 * SCNetworkConfigurationInternal.c — the default configuration of a
 * protocol on an interface type (Apple's NetworkConfiguration.plist),
 * or NULL. Caller releases.
 */
CFDictionaryRef
__copyProtocolTemplate			(CFStringRef		interfaceType,
					 CFStringRef		protocolType);

/* SCNetworkConfigurationInternal.c — PrimaryRank <-> its stored string. */
Boolean
__rank_to_str				(SCNetworkServicePrimaryRank	rank,
					 CFStringRef			*rankStr);

Boolean
__str_to_rank				(CFStringRef			rankStr,
					 SCNetworkServicePrimaryRank	*rank);

/*
 * SCNetworkInterface.c — a copy of `interface` bound to the service
 * `serviceID` in `prefs`, so its configuration can be read and written.
 */
SCNetworkInterfaceRef
__SCNetworkInterfaceCreateCopy		(CFAllocatorRef		allocator,
					 SCNetworkInterfaceRef	interface,
					 SCPreferencesRef	prefs,
					 CFStringRef		serviceID);

/* SCNetworkInterface.c — the interface's position in the default order. */
int
__SCNetworkInterfaceOrder		(SCNetworkInterfaceRef	interface);

/* SCNetworkService.c — TRUE iff a service in `services` uses `interface`. */
Boolean
__SCNetworkServiceExistsForInterface	(CFArrayRef		services,
					 SCNetworkInterfaceRef	interface);

/* SCNetworkService.c — the service's name with its numeric suffix bumped. */
CFStringRef
__SCNetworkServiceNextName		(SCNetworkServiceRef	service);

/* SCBondInterface.c, SCBridgeInterface.c — add each member to `set`. */
void
__SCBondInterfaceListCollectMembers	(CFArrayRef		interfaces,
					 CFMutableSetRef	set);

void
__SCBridgeInterfaceListCollectMembers	(CFArrayRef		interfaces,
					 CFMutableSetRef	set);

#endif	/* _SCNETWORKCONFIGURATIONINTERNAL_H */
