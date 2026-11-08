/*
 * SCNetworkService.c — the SCNetworkService object (freebsd-launchd-mach
 * port of Apple's SystemConfiguration.fproj SCNetworkService.c).
 *
 * A network service binds a network interface to the protocols (IPv4,
 * IPv6, DNS, Proxies, ...) configured on it. It is persisted in the
 * preferences plist at /NetworkServices/<serviceID>: a dictionary whose
 * "Interface" entity describes the interface, "UserDefinedName" holds
 * the service name, and each remaining sub-dictionary is one protocol
 * entity.
 *
 * Apple's SCNetworkService.c additionally seeds interface-type-specific
 * configuration templates (PPP/Modem/Bluetooth), routes privileged
 * writes through SCHelper, and tracks set membership. The port keeps
 * the core CRUD: create / copy / list a service, name it, read its
 * interface, attach and remove protocols; plus, from configd-596.15,
 * the default configuration and PrimaryRank SPI at the end of the file.
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <pthread.h>
#include <stdlib.h>


#pragma mark -
#pragma mark CoreFoundation runtime type

static CFTypeID	__kSCNetworkServiceTypeID	= _kCFRuntimeNotATypeID;

static void
__SCNetworkServiceDeallocate(CFTypeRef cf)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)cf;

	if (sp->serviceID != NULL)	CFRelease(sp->serviceID);
	if (sp->interface != NULL)	CFRelease(sp->interface);
	if (sp->prefs != NULL)		CFRelease(sp->prefs);
	if (sp->store != NULL)		CFRelease(sp->store);
	if (sp->name != NULL)		CFRelease(sp->name);
}

static Boolean
__SCNetworkServiceEqual(CFTypeRef cf1, CFTypeRef cf2)
{
	SCNetworkServicePrivateRef	s1	= (SCNetworkServicePrivateRef)cf1;
	SCNetworkServicePrivateRef	s2	= (SCNetworkServicePrivateRef)cf2;

	if (s1 == s2) {
		return TRUE;
	}
	return CFEqual(s1->serviceID, s2->serviceID);
}

static CFHashCode
__SCNetworkServiceHash(CFTypeRef cf)
{
	return CFHash(((SCNetworkServicePrivateRef)cf)->serviceID);
}

static CFStringRef
__SCNetworkServiceCopyDescription(CFTypeRef cf)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)cf;

	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("<SCNetworkService %@>"), sp->serviceID);
}

static const CFRuntimeClass __SCNetworkServiceClass = {
	.version	= 0,
	.className	= "SCNetworkService",
	.finalize	= __SCNetworkServiceDeallocate,
	.equal		= __SCNetworkServiceEqual,
	.hash		= __SCNetworkServiceHash,
	.copyDebugDesc	= __SCNetworkServiceCopyDescription,
};

static void
__SCNetworkServiceClassInitialize(void)
{
	__kSCNetworkServiceTypeID =
		_CFRuntimeRegisterClass(&__SCNetworkServiceClass);
}

CFTypeID
SCNetworkServiceGetTypeID(void)
{
	static pthread_once_t	once	= PTHREAD_ONCE_INIT;

	(void) pthread_once(&once, __SCNetworkServiceClassInitialize);
	return __kSCNetworkServiceTypeID;
}

SCNetworkServicePrivateRef
__SCNetworkServiceCreatePrivate(CFAllocatorRef		allocator,
				SCPreferencesRef	prefs,
				CFStringRef		serviceID,
				SCNetworkInterfaceRef	interface)
{
	SCNetworkServicePrivateRef	sp;
	CFIndex				extra;

	extra = sizeof(SCNetworkServicePrivate) - sizeof(CFRuntimeBase);
	sp = (SCNetworkServicePrivateRef)
	     _CFRuntimeCreateInstance(allocator,
				      SCNetworkServiceGetTypeID(),
				      extra, NULL);
	if (sp == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	sp->serviceID = CFStringCreateCopy(NULL, serviceID);
	sp->interface = (interface != NULL)
			? (SCNetworkInterfaceRef)CFRetain(interface) : NULL;
	sp->prefs     = (prefs != NULL)
			? (SCPreferencesRef)CFRetain(prefs) : NULL;
	sp->store     = NULL;
	sp->name      = NULL;
	return sp;
}


#pragma mark -
#pragma mark Helpers

/*
 * Copy the keys and values of `dict` into caller-freed arrays; both
 * returned arrays must be free()d. Returns the count, or 0 (with the
 * arrays left NULL) for an empty or non-dictionary argument.
 */
static CFIndex
__copyDictEntries(CFDictionaryRef dict, const void ***keys,
		  const void ***vals)
{
	CFIndex	n;

	*keys = NULL;
	*vals = NULL;
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		return 0;
	}
	n = CFDictionaryGetCount(dict);
	if (n <= 0) {
		return 0;
	}
	*keys = (const void **)malloc((size_t)n * sizeof(void *));
	*vals = (const void **)malloc((size_t)n * sizeof(void *));
	CFDictionaryGetKeysAndValues(dict, *keys, *vals);
	return n;
}

/* the service's "Interface" entity dictionary, or NULL */
static CFDictionaryRef
__serviceInterfaceEntity(SCNetworkServicePrivateRef sp)
{
	CFDictionaryRef	entity;
	CFStringRef	path;

	path   = __SCNetworkServiceEntityPath(sp->serviceID, kSCEntNetInterface);
	entity = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((entity != NULL) &&
	    (CFGetTypeID(entity) == CFDictionaryGetTypeID())) {
		return entity;
	}
	return NULL;
}


#pragma mark -
#pragma mark SCNetworkService APIs

SCNetworkServiceRef
SCNetworkServiceCreate(SCPreferencesRef prefs, SCNetworkInterfaceRef interface)
{
	CFArrayRef			components;
	CFDictionaryRef			entity;
	CFStringRef			name;
	CFStringRef			path;
	CFStringRef			prefix;
	CFStringRef			serviceID;
	SCNetworkInterfaceRef		bound;
	SCNetworkServicePrivateRef	sp;

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	/* only interfaces that support a protocol may back a service */
	if (SCNetworkInterfaceGetSupportedProtocolTypes(interface) == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	/* mint a unique /NetworkServices/<serviceID> entry — the prefix
	 * passed to SCPreferencesPathCreateUniqueChild must be a rooted
	 * path, not the bare "NetworkServices" preferences key */
	prefix = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@"),
					  kSCPrefNetworkServices);
	path = SCPreferencesPathCreateUniqueChild(prefs, prefix);
	CFRelease(prefix);
	if (path == NULL) {
		return NULL;
	}
	components = CFStringCreateArrayBySeparatingStrings(NULL, path,
							    CFSTR("/"));
	CFRelease(path);
	/* path "/NetworkServices/<id>" splits to ["", "NetworkServices", id] */
	if ((components == NULL) || (CFArrayGetCount(components) < 3)) {
		if (components != NULL) CFRelease(components);
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	serviceID = CFArrayGetValueAtIndex(components, 2);

	/* the service holds a copy of the interface bound to it, as Apple's
	 * does, so SCNetworkInterfaceGet/SetConfiguration work on it */
	bound = __SCNetworkInterfaceCreateCopy(NULL, interface, prefs, serviceID);
	if (bound == NULL) {
		CFRelease(components);
		return NULL;
	}
	sp = __SCNetworkServiceCreatePrivate(NULL, prefs, serviceID, bound);
	CFRelease(bound);
	CFRelease(components);
	if (sp == NULL) {
		return NULL;
	}

	/* store the interface entity at /NetworkServices/<id>/Interface */
	path   = __SCNetworkServiceEntityPath(sp->serviceID, kSCEntNetInterface);
	entity = __SCNetworkInterfaceCopyInterfaceEntity(interface);
	if (!SCPreferencesPathSetValue(sp->prefs, path, entity)) {
		CFRelease(entity);
		CFRelease(path);
		CFRelease(sp);
		return NULL;
	}
	CFRelease(entity);
	CFRelease(path);

	/* seed the service name from the interface's display name */
	name = SCNetworkServiceGetName((SCNetworkServiceRef)sp);
	if (name != NULL) {
		(void) SCNetworkServiceSetName((SCNetworkServiceRef)sp, name);
	}

	return (SCNetworkServiceRef)sp;
}

SCNetworkServiceRef
SCNetworkServiceCopy(SCPreferencesRef prefs, CFStringRef serviceID)
{
	SCNetworkServicePrivateRef	sp;

	if ((serviceID == NULL) ||
	    (CFGetTypeID(serviceID) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	sp = __SCNetworkServiceCreatePrivate(NULL, prefs, serviceID, NULL);
	if (sp == NULL) {
		return NULL;
	}
	/* a service must have an interface entity to exist */
	if (__serviceInterfaceEntity(sp) == NULL) {
		CFRelease(sp);
		_SCErrorSet(kSCStatusNoKey);
		return NULL;
	}
	return (SCNetworkServiceRef)sp;
}

CFArrayRef
SCNetworkServiceCopyAll(SCPreferencesRef prefs)
{
	CFMutableArrayRef	array;
	const void		**keys;
	const void		**vals;
	CFIndex			i, n;
	CFStringRef		path;
	CFDictionaryRef		services;

	path     = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@"),
					    kSCPrefNetworkServices);
	services = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);

	if ((services != NULL) &&
	    (CFGetTypeID(services) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	n     = __copyDictEntries(services, &keys, &vals);
	for (i = 0; i < n; i++) {
		SCNetworkServicePrivateRef	sp;
		CFDictionaryRef			entity;

		if (CFGetTypeID(vals[i]) != CFDictionaryGetTypeID()) {
			continue;
		}
		/* a service must carry an "Interface" entity */
		entity = CFDictionaryGetValue((CFDictionaryRef)vals[i],
					      kSCEntNetInterface);
		if ((entity == NULL) ||
		    (CFGetTypeID(entity) != CFDictionaryGetTypeID())) {
			continue;
		}
		sp = __SCNetworkServiceCreatePrivate(NULL, prefs,
						     (CFStringRef)keys[i], NULL);
		if (sp != NULL) {
			CFArrayAppendValue(array, sp);
			CFRelease(sp);
		}
	}
	if (keys != NULL)	free(keys);
	if (vals != NULL)	free(vals);
	return array;
}

CFStringRef
SCNetworkServiceGetServiceID(SCNetworkServiceRef service)
{
	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkServicePrivateRef)service)->serviceID;
}

SCNetworkInterfaceRef
SCNetworkServiceGetInterface(SCNetworkServiceRef service)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	if (sp->interface == NULL) {
		CFDictionaryRef	entity = __serviceInterfaceEntity(sp);

		if (entity != NULL) {
			sp->interface = _SCNetworkInterfaceCreateWithEntity(
						NULL, entity, service);
		}
	}
	return sp->interface;
}

#pragma mark -
#pragma mark Enabled state

/*
 * A service is disabled by an "__INACTIVE__" key in its dictionary, as a
 * protocol is in SCNetworkProtocol.c (Darwin's __getPrefsEnabled and
 * __setPrefsEnabled, SCNetworkConfigurationInternal.c).
 */
static Boolean
__getPrefsEnabled(SCPreferencesRef prefs, CFStringRef path)
{
	CFDictionaryRef	config;

	config = SCPreferencesPathGetValue(prefs, path);
	if ((config != NULL) &&
	    (CFGetTypeID(config) == CFDictionaryGetTypeID()) &&
	    CFDictionaryContainsKey(config, kSCResvInactive)) {
		return FALSE;
	}
	return TRUE;
}

static Boolean
__setPrefsEnabled(SCPreferencesRef prefs, CFStringRef path, Boolean enabled)
{
	CFDictionaryRef		curConfig;
	CFMutableDictionaryRef	newConfig	= NULL;
	Boolean			ok;

	/* preserve the current configuration */
	curConfig = SCPreferencesPathGetValue(prefs, path);
	if (curConfig != NULL) {
		if (CFGetTypeID(curConfig) != CFDictionaryGetTypeID()) {
			_SCErrorSet(kSCStatusFailed);
			return FALSE;
		}
		newConfig = CFDictionaryCreateMutableCopy(NULL, 0, curConfig);
		if (enabled) {
			CFDictionaryRemoveValue(newConfig, kSCResvInactive);
		} else {
			CFDictionarySetValue(newConfig, kSCResvInactive,
					     kCFBooleanTrue);
		}
	} else if (!enabled) {
		newConfig = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(newConfig, kSCResvInactive,
				     kCFBooleanTrue);
	}

	if ((curConfig == newConfig) ||
	    ((curConfig != NULL) && (newConfig != NULL) &&
	     CFEqual(curConfig, newConfig))) {
		/* no change */
		if (newConfig != NULL) {
			CFRelease(newConfig);
		}
		ok = TRUE;
	} else if (newConfig != NULL) {
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

/*
 * Is `interface`, or an interface it is layered on, a member of a Bond or
 * Bridge in `prefs`? (Darwin's __SCNetworkInterfaceIsMember.)
 */
static Boolean
__SCNetworkInterfaceIsMember(SCPreferencesRef prefs,
			     SCNetworkInterfaceRef interface)
{
	CFArrayRef	interfaces;
	Boolean		match	= FALSE;
	CFMutableSetRef	members;

	members = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

	interfaces = SCBondInterfaceCopyAll(prefs);
	if (interfaces != NULL) {
		__SCBondInterfaceListCollectMembers(interfaces, members);
		CFRelease(interfaces);
	}
	interfaces = SCBridgeInterfaceCopyAll(prefs);
	if (interfaces != NULL) {
		__SCBridgeInterfaceListCollectMembers(interfaces, members);
		CFRelease(interfaces);
	}

	while ((interface != NULL) && !match) {
		match = CFSetContainsValue(members, interface);
		interface = SCNetworkInterfaceGetInterface(interface);
	}

	CFRelease(members);
	return match;
}

Boolean
SCNetworkServiceGetEnabled(SCNetworkServiceRef service)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFStringRef			path;
	Boolean				enabled;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path    = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
	enabled = __getPrefsEnabled(sp->prefs, path);
	CFRelease(path);
	return enabled;
}

Boolean
SCNetworkServiceSetEnabled(SCNetworkServiceRef service, Boolean enabled)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	/* a service on a Bond or Bridge member must stay disabled */
	if (enabled) {
		SCNetworkInterfaceRef	interface;

		interface = SCNetworkServiceGetInterface(service);
		if ((interface != NULL) &&
		    __SCNetworkInterfaceIsMember(sp->prefs, interface)) {
			_SCErrorSet(kSCStatusKeyExists);
			return FALSE;
		}
	}

	path = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
	ok   = __setPrefsEnabled(sp->prefs, path, enabled);
	CFRelease(path);
	return ok;
}

CFStringRef
SCNetworkServiceGetName(SCNetworkServiceRef service)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	SCNetworkInterfaceRef		interface;
	CFDictionaryRef			entity;
	CFStringRef			name;
	CFStringRef			path;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	if (sp->name != NULL) {
		return sp->name;
	}

	/* a stored UserDefinedName wins */
	path   = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
	entity = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((entity != NULL) &&
	    (CFGetTypeID(entity) == CFDictionaryGetTypeID())) {
		name = CFDictionaryGetValue(entity, kSCPropUserDefinedName);
		if ((name != NULL) &&
		    (CFGetTypeID(name) == CFStringGetTypeID())) {
			sp->name = (CFStringRef)CFRetain(name);
			return sp->name;
		}
	}

	/* otherwise fall back to the interface's display name */
	interface = SCNetworkServiceGetInterface(service);
	if (interface != NULL) {
		return SCNetworkInterfaceGetLocalizedDisplayName(interface);
	}
	return NULL;
}

Boolean
SCNetworkServiceSetName(SCNetworkServiceRef service, CFStringRef name)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFDictionaryRef			entity;
	CFMutableDictionaryRef		newEntity;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((name != NULL) &&
	    (CFGetTypeID(name) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path   = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
	entity = SCPreferencesPathGetValue(sp->prefs, path);
	if ((entity != NULL) &&
	    (CFGetTypeID(entity) == CFDictionaryGetTypeID())) {
		newEntity = CFDictionaryCreateMutableCopy(NULL, 0, entity);
	} else {
		newEntity = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	if (name != NULL) {
		CFDictionarySetValue(newEntity, kSCPropUserDefinedName, name);
	} else {
		CFDictionaryRemoveValue(newEntity, kSCPropUserDefinedName);
	}

	ok = SCPreferencesPathSetValue(sp->prefs, path, newEntity);
	CFRelease(newEntity);
	CFRelease(path);

	if (ok) {
		if (sp->name != NULL) {
			CFRelease(sp->name);
		}
		sp->name = (name != NULL) ? CFStringCreateCopy(NULL, name)
					  : NULL;
	}
	return ok;
}

Boolean
SCNetworkServiceRemove(SCNetworkServiceRef service)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	/* set membership is removed by the SCNetworkSet iteration */
	path = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
	ok   = SCPreferencesPathRemoveValue(sp->prefs, path);
	CFRelease(path);
	return ok;
}


#pragma mark -
#pragma mark Protocols on a service

SCNetworkProtocolRef
SCNetworkServiceCopyProtocol(SCNetworkServiceRef service,
			     CFStringRef protocolType)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFDictionaryRef			entity;
	CFDictionaryRef			service_entity;
	CFStringRef			path;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	if ((protocolType == NULL) ||
	    (CFGetTypeID(protocolType) != CFStringGetTypeID()) ||
	    CFEqual(protocolType, kSCEntNetInterface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path           = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
	service_entity = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((service_entity == NULL) ||
	    (CFGetTypeID(service_entity) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	entity = CFDictionaryGetValue(service_entity, protocolType);
	if ((entity == NULL) ||
	    (CFGetTypeID(entity) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusNoKey);
		return NULL;
	}
	return (SCNetworkProtocolRef)
		__SCNetworkProtocolCreatePrivate(NULL, protocolType, service);
}

CFArrayRef
SCNetworkServiceCopyProtocols(SCNetworkServiceRef service)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFMutableArrayRef		array;
	const void			**keys;
	const void			**vals;
	CFIndex				i, n;
	CFStringRef			path;
	CFDictionaryRef			service_entity;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path           = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
	service_entity = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((service_entity == NULL) ||
	    (CFGetTypeID(service_entity) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	n     = __copyDictEntries(service_entity, &keys, &vals);
	for (i = 0; i < n; i++) {
		SCNetworkProtocolPrivateRef	pp;

		/* a protocol entity is a dictionary that is not "Interface" */
		if (CFGetTypeID(vals[i]) != CFDictionaryGetTypeID()) {
			continue;
		}
		if (CFEqual((CFStringRef)keys[i], kSCEntNetInterface)) {
			continue;
		}
		pp = __SCNetworkProtocolCreatePrivate(NULL,
						      (CFStringRef)keys[i],
						      service);
		if (pp != NULL) {
			CFArrayAppendValue(array, pp);
			CFRelease(pp);
		}
	}
	if (keys != NULL)	free(keys);
	if (vals != NULL)	free(vals);
	return array;
}

Boolean
SCNetworkServiceAddProtocolType(SCNetworkServiceRef service,
				CFStringRef protocolType)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFDictionaryRef			entity;
	CFDictionaryRef			newEntity;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__SCNetworkProtocolIsValidType(protocolType)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path   = __SCNetworkServiceEntityPath(sp->serviceID, protocolType);
	entity = SCPreferencesPathGetValue(sp->prefs, path);
	if (entity != NULL) {
		/* the protocol is already configured */
		_SCErrorSet(kSCStatusKeyExists);
		CFRelease(path);
		return FALSE;
	}

	/* a fresh, empty protocol entity — configure it via the protocol */
	newEntity = CFDictionaryCreate(NULL, NULL, NULL, 0,
				       &kCFTypeDictionaryKeyCallBacks,
				       &kCFTypeDictionaryValueCallBacks);
	ok = SCPreferencesPathSetValue(sp->prefs, path, newEntity);
	CFRelease(newEntity);
	CFRelease(path);
	return ok;
}

Boolean
SCNetworkServiceRemoveProtocolType(SCNetworkServiceRef service,
				   CFStringRef protocolType)
{
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;
	CFDictionaryRef			entity;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__SCNetworkProtocolIsValidType(protocolType)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path   = __SCNetworkServiceEntityPath(sp->serviceID, protocolType);
	entity = SCPreferencesPathGetValue(sp->prefs, path);
	if (entity == NULL) {
		/* the protocol is not configured */
		_SCErrorSet(kSCStatusNoKey);
		CFRelease(path);
		return FALSE;
	}

	ok = SCPreferencesPathRemoveValue(sp->prefs, path);
	CFRelease(path);
	return ok;
}


#pragma mark -
#pragma mark SCNetworkService SPIs

Boolean
__SCNetworkServiceExistsForInterface(CFArrayRef services, SCNetworkInterfaceRef interface)
{
	CFIndex	i;
	CFIndex	n;

	n = isA_CFArray(services) ? CFArrayGetCount(services) : 0;
	for (i = 0; i < n; i++) {
		SCNetworkServiceRef	service;
		SCNetworkInterfaceRef	service_interface;

		service = CFArrayGetValueAtIndex(services, i);

		service_interface = SCNetworkServiceGetInterface(service);
		while (service_interface != NULL) {
			if (CFEqual(interface, service_interface)) {
				return TRUE;
			}

			service_interface = SCNetworkInterfaceGetInterface(service_interface);
		}
	}

	return FALSE;
}

CFStringRef
__SCNetworkServiceNextName(SCNetworkServiceRef service)
{
	CFArrayRef		components;
	CFIndex			n;
	CFStringRef		name;
	CFMutableArrayRef	newComponents;
	SInt32			suffix	= 2;

	name = SCNetworkServiceGetName(service);
	if (name == NULL) {
		return NULL;
	}

	components = CFStringCreateArrayBySeparatingStrings(NULL, name, CFSTR(" "));
	if (components != NULL) {
		newComponents = CFArrayCreateMutableCopy(NULL, 0, components);
		CFRelease(components);
	} else {
		newComponents = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		CFArrayAppendValue(newComponents, name);
	}

	n = CFArrayGetCount(newComponents);
	if (n > 1) {
		CFStringRef	str;

		str = CFArrayGetValueAtIndex(newComponents, n - 1);
		suffix = CFStringGetIntValue(str);
		if (suffix++ > 0) {
			CFArrayRemoveValueAtIndex(newComponents, n - 1);
		} else {
			suffix = 2;
		}
	}

	name = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), (int)suffix);
	CFArrayAppendValue(newComponents, name);
	CFRelease(name);

	name = CFStringCreateByCombiningStrings(NULL, newComponents, CFSTR(" "));
	CFRelease(newComponents);

	return name;
}

static CFDictionaryRef
_protocolTemplate(SCNetworkServiceRef service, CFStringRef protocolType)
{
	SCNetworkServicePrivateRef	sp		= (SCNetworkServicePrivateRef)service;
	CFDictionaryRef			template	= NULL;

	if (sp->interface != NULL) {
		template = __copyProtocolTemplate(SCNetworkInterfaceGetInterfaceType(sp->interface),
						  protocolType);
	}

	if (template == NULL) {
		template = CFDictionaryCreate(NULL,
					      NULL,
					      NULL,
					      0,
					      &kCFTypeDictionaryKeyCallBacks,
					      &kCFTypeDictionaryValueCallBacks);
	}

	return template;
}

Boolean
SCNetworkServiceEstablishDefaultConfiguration(SCNetworkServiceRef service)
{
	CFIndex				i;
	SCNetworkInterfaceRef		interface;
	CFIndex				n;
	CFArrayRef			protocolTypes;
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service) || (sp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	interface = SCNetworkServiceGetInterface(service);
	if (interface == NULL) {
		return FALSE;
	}

	protocolTypes = SCNetworkInterfaceGetSupportedProtocolTypes(interface);
	n = (protocolTypes != NULL) ? CFArrayGetCount(protocolTypes) : 0;
	for (i = 0; i < n; i++) {
		Boolean			enabled;
		CFDictionaryRef		newEntity	= NULL;
		Boolean			ok;
		SCNetworkProtocolRef	protocol	= NULL;
		CFStringRef		protocolType;

		protocolType = CFArrayGetValueAtIndex(protocolTypes, i);
		ok = SCNetworkServiceAddProtocolType(service, protocolType);
		if (!ok && (SCError() != kSCStatusKeyExists)) {
			// could not add protocol
			goto nextProtocol;
		}

		protocol = SCNetworkServiceCopyProtocol(service, protocolType);
		if (protocol == NULL) {
			// oops, somethings wrong (should never happen)
			goto nextProtocol;
		}

		newEntity = _protocolTemplate(service, protocolType);
		ok = SCNetworkProtocolSetConfiguration(protocol, newEntity);
		if (!ok) {
			// could not set default configuration
			goto nextProtocol;
		}

		enabled = !CFDictionaryContainsKey(newEntity, kSCResvInactive);
		ok = SCNetworkProtocolSetEnabled(protocol, enabled);
		if (!ok) {
			// could not enable/disable protocol
			goto nextProtocol;
		}

	    nextProtocol :

		if (newEntity != NULL) CFRelease(newEntity);
		if (protocol  != NULL) CFRelease(protocol);
	}

	return TRUE;
}

SCNetworkServiceRef
_SCNetworkServiceCopyActive(SCDynamicStoreRef store, CFStringRef serviceID)
{
	SCNetworkServicePrivateRef	sp;

	if (!isA_CFString(serviceID)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	sp = __SCNetworkServiceCreatePrivate(NULL, NULL, serviceID, NULL);
	if (sp == NULL) {
		return NULL;
	}
	if (store != NULL) {
		sp->store = (SCDynamicStoreRef)CFRetain(store);
	}
	return (SCNetworkServiceRef)sp;
}

SCNetworkServicePrimaryRank
SCNetworkServiceGetPrimaryRank(SCNetworkServiceRef service)
{
	CFDictionaryRef			entity;
	Boolean				ok	= TRUE;
	CFStringRef			path;
	SCNetworkServicePrimaryRank	rank	= kSCNetworkServicePrimaryRankDefault;
	CFStringRef			rankStr	= NULL;
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return rank;
	}

	if (sp->prefs != NULL) {
		path = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
		entity = SCPreferencesPathGetValue(sp->prefs, path);
		CFRelease(path);
		if (isA_CFDictionary(entity)) {
			rankStr = CFDictionaryGetValue(entity, kSCPropNetServicePrimaryRank);
			ok = __str_to_rank(rankStr, &rank);
		}
	} else if (sp->store != NULL) {
		path = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								   kSCDynamicStoreDomainState,
								   sp->serviceID,
								   NULL);
		entity = SCDynamicStoreCopyValue(sp->store, path);
		CFRelease(path);
		if (entity != NULL) {
			if (isA_CFDictionary(entity)) {
				rankStr = CFDictionaryGetValue(entity, kSCPropNetServicePrimaryRank);
				ok = __str_to_rank(rankStr, &rank);
			}
			CFRelease(entity);
		}
	} else {
		_SCErrorSet(kSCStatusInvalidArgument);
		return rank;
	}

	if (!ok) {
		rank = kSCNetworkServicePrimaryRankDefault;
		_SCErrorSet(kSCStatusInvalidArgument);
	} else if (rank == kSCNetworkServicePrimaryRankDefault) {
		_SCErrorSet(kSCStatusOK);
	}

	return rank;
}

Boolean
SCNetworkServiceSetPrimaryRank(SCNetworkServiceRef		service,
			       SCNetworkServicePrimaryRank	newRank)
{
	Boolean				ok;
	CFDictionaryRef			entity;
	CFMutableDictionaryRef		newEntity;
	CFStringRef			path	= NULL;
	CFStringRef			rankStr	= NULL;
	SCNetworkServicePrivateRef	sp	= (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	ok = __rank_to_str(newRank, &rankStr);
	if (!ok) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (sp->prefs != NULL) {
		if ((newRank != kSCNetworkServicePrimaryRankDefault) &&
		    (newRank != kSCNetworkServicePrimaryRankNever)) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}

		path = __SCNetworkServiceEntityPath(sp->serviceID, NULL);
		entity = SCPreferencesPathGetValue(sp->prefs, path);
		if (entity != NULL) {
			if (!isA_CFDictionary(entity)) {
				// if corrupt prefs
				_SCErrorSet(kSCStatusFailed);
				ok = FALSE;
				goto done;
			}
			newEntity = CFDictionaryCreateMutableCopy(NULL, 0, entity);
		} else {
			newEntity = CFDictionaryCreateMutable(NULL,
							      0,
							      &kCFTypeDictionaryKeyCallBacks,
							      &kCFTypeDictionaryValueCallBacks);
		}
		if (rankStr != NULL) {
			CFDictionarySetValue(newEntity, kSCPropNetServicePrimaryRank, rankStr);
		} else {
			CFDictionaryRemoveValue(newEntity, kSCPropNetServicePrimaryRank);
		}
		if (CFDictionaryGetCount(newEntity) > 0) {
			ok = SCPreferencesPathSetValue(sp->prefs, path, newEntity);
		} else {
			ok = SCPreferencesPathRemoveValue(sp->prefs, path);
		}
		CFRelease(newEntity);
	} else if (sp->store != NULL) {
		path = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								   kSCDynamicStoreDomainState,
								   sp->serviceID,
								   NULL);
		entity = SCDynamicStoreCopyValue(sp->store, path);
		if (entity != NULL) {
			if (!isA_CFDictionary(entity)) {
				// if corrupt prefs
				CFRelease(entity);
				_SCErrorSet(kSCStatusFailed);
				ok = FALSE;
				goto done;
			}
			newEntity = CFDictionaryCreateMutableCopy(NULL, 0, entity);
			CFRelease(entity);
		} else {
			newEntity = CFDictionaryCreateMutable(NULL,
							      0,
							      &kCFTypeDictionaryKeyCallBacks,
							      &kCFTypeDictionaryValueCallBacks);
		}
		if (rankStr != NULL) {
			CFDictionarySetValue(newEntity, kSCPropNetServicePrimaryRank, rankStr);
		} else {
			CFDictionaryRemoveValue(newEntity, kSCPropNetServicePrimaryRank);
		}
		if (CFDictionaryGetCount(newEntity) > 0) {
			ok = SCDynamicStoreSetValue(sp->store, path, newEntity);
		} else {
			ok = SCDynamicStoreRemoveValue(sp->store, path);
		}
		CFRelease(newEntity);
	} else {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

    done :

	if (path != NULL)	CFRelease(path);
	return ok;
}
