/*
 * SCNetworkSet.c — the SCNetworkSet object (freebsd-launchd-mach port of
 * Apple's SystemConfiguration.fproj SCNetworkSet.c).
 *
 * A network set — a "location" — is an ordered collection of network
 * services. It is persisted in the preferences plist at /Sets/<setID>;
 * a service is a member of a set via a __LINK__ entry at
 * /Sets/<setID>/Network/Service/<serviceID> pointing back at the real
 * /NetworkServices/<serviceID>. The active set is named by the
 * top-level "CurrentSet" preferences key.
 *
 * Apple's SCNetworkSet.c additionally manages the special "Automatic"
 * default set, per-interface "deep" configuration, and VPN selection;
 * the port keeps the core model: create / copy / list a set, name it,
 * add and remove services, the service order, the current set, and
 * the default-configuration SPI from configd-596.15 at the end of the
 * file (without its Thunderbolt-bridge and iOS branches).
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <pthread.h>
#include <stdlib.h>


#pragma mark -
#pragma mark CoreFoundation runtime type

static CFTypeID	__kSCNetworkSetTypeID	= _kCFRuntimeNotATypeID;

static void
__SCNetworkSetDeallocate(CFTypeRef cf)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)cf;

	if (sp->setID != NULL)	CFRelease(sp->setID);
	if (sp->prefs != NULL)	CFRelease(sp->prefs);
	if (sp->name != NULL)	CFRelease(sp->name);
}

static Boolean
__SCNetworkSetEqual(CFTypeRef cf1, CFTypeRef cf2)
{
	SCNetworkSetPrivateRef	s1	= (SCNetworkSetPrivateRef)cf1;
	SCNetworkSetPrivateRef	s2	= (SCNetworkSetPrivateRef)cf2;

	if (s1 == s2) {
		return TRUE;
	}
	return CFEqual(s1->setID, s2->setID);
}

static CFHashCode
__SCNetworkSetHash(CFTypeRef cf)
{
	return CFHash(((SCNetworkSetPrivateRef)cf)->setID);
}

static CFStringRef
__SCNetworkSetCopyDescription(CFTypeRef cf)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)cf;

	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("<SCNetworkSet %@>"), sp->setID);
}

static const CFRuntimeClass __SCNetworkSetClass = {
	.version	= 0,
	.className	= "SCNetworkSet",
	.finalize	= __SCNetworkSetDeallocate,
	.equal		= __SCNetworkSetEqual,
	.hash		= __SCNetworkSetHash,
	.copyDebugDesc	= __SCNetworkSetCopyDescription,
};

static void
__SCNetworkSetClassInitialize(void)
{
	__kSCNetworkSetTypeID = _CFRuntimeRegisterClass(&__SCNetworkSetClass);
}

CFTypeID
SCNetworkSetGetTypeID(void)
{
	static pthread_once_t	once	= PTHREAD_ONCE_INIT;

	(void) pthread_once(&once, __SCNetworkSetClassInitialize);
	return __kSCNetworkSetTypeID;
}

SCNetworkSetPrivateRef
__SCNetworkSetCreatePrivate(CFAllocatorRef	allocator,
			    SCPreferencesRef	prefs,
			    CFStringRef		setID)
{
	SCNetworkSetPrivateRef	sp;
	CFIndex			extra;

	extra = sizeof(SCNetworkSetPrivate) - sizeof(CFRuntimeBase);
	sp = (SCNetworkSetPrivateRef)
	     _CFRuntimeCreateInstance(allocator,
				      SCNetworkSetGetTypeID(),
				      extra, NULL);
	if (sp == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	sp->setID = CFStringCreateCopy(NULL, setID);
	sp->prefs = (prefs != NULL)
		    ? (SCPreferencesRef)CFRetain(prefs) : NULL;
	sp->name  = NULL;
	sp->established = FALSE;	/* "new" (not yet established) set */
	return sp;
}


#pragma mark -
#pragma mark Path helpers

/* "/Sets/<setID>" */
static CFStringRef
__setPath(CFStringRef setID)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
					kSCPrefSets, setID);
}

/*
 * "/Sets/<setID>/Network/Service" (serviceID NULL) or
 * "/Sets/<setID>/Network/Service/<serviceID>"
 */
static CFStringRef
__setServicePath(CFStringRef setID, CFStringRef serviceID)
{
	if (serviceID == NULL) {
		return CFStringCreateWithFormat(NULL, NULL,
				CFSTR("/%@/%@/%@/%@"),
				kSCPrefSets, setID,
				kSCCompNetwork, kSCCompService);
	}
	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("/%@/%@/%@/%@/%@"),
			kSCPrefSets, setID,
			kSCCompNetwork, kSCCompService, serviceID);
}

/* "/Sets/<setID>/Network/Global/IPv4" — holds the service order */
static CFStringRef
__setGlobalIPv4Path(CFStringRef setID)
{
	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("/%@/%@/%@/%@/%@"),
			kSCPrefSets, setID,
			kSCCompNetwork, kSCCompGlobal, kSCEntNetIPv4);
}

/* the __LINK__ target for a service membership: "/NetworkServices/<id>" */
static CFStringRef
__netServicePath(CFStringRef serviceID)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
					kSCPrefNetworkServices, serviceID);
}

/*
 * Copy the keys of `dict` into a caller-freed array; the array must be
 * free()d. Returns the count, or 0 (array left NULL) for an empty or
 * non-dictionary argument.
 */
static CFIndex
__copyDictKeys(CFDictionaryRef dict, const void ***keys)
{
	CFIndex	n;

	*keys = NULL;
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		return 0;
	}
	n = CFDictionaryGetCount(dict);
	if (n <= 0) {
		return 0;
	}
	*keys = (const void **)malloc((size_t)n * sizeof(void *));
	CFDictionaryGetKeysAndValues(dict, *keys, NULL);
	return n;
}


#pragma mark -
#pragma mark SCNetworkSet APIs

SCNetworkSetRef
SCNetworkSetCreate(SCPreferencesRef prefs)
{
	CFArrayRef		components;
	CFStringRef		path;
	CFStringRef		prefix;
	CFStringRef		setID;
	SCNetworkSetPrivateRef	sp;

	/* mint a unique /Sets/<setID> entry */
	prefix = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@"),
					  kSCPrefSets);
	path = SCPreferencesPathCreateUniqueChild(prefs, prefix);
	CFRelease(prefix);
	if (path == NULL) {
		return NULL;
	}
	components = CFStringCreateArrayBySeparatingStrings(NULL, path,
							    CFSTR("/"));
	CFRelease(path);
	/* "/Sets/<id>" splits to ["", "Sets", id] */
	if ((components == NULL) || (CFArrayGetCount(components) < 3)) {
		if (components != NULL) CFRelease(components);
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	setID = CFArrayGetValueAtIndex(components, 2);
	sp = __SCNetworkSetCreatePrivate(NULL, prefs, setID);
	CFRelease(components);
	return (SCNetworkSetRef)sp;
}

SCNetworkSetRef
SCNetworkSetCopy(SCPreferencesRef prefs, CFStringRef setID)
{
	CFDictionaryRef		dict;
	CFStringRef		path;
	SCNetworkSetPrivateRef	sp;

	if ((setID == NULL) ||
	    (CFGetTypeID(setID) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = __setPath(setID);
	dict = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusNoKey);
		return NULL;
	}
	sp = __SCNetworkSetCreatePrivate(NULL, prefs, setID);
	if (sp != NULL) {
		/* mark set as "old" (already established) */
		sp->established = TRUE;
	}
	return (SCNetworkSetRef)sp;
}

CFArrayRef
SCNetworkSetCopyAll(SCPreferencesRef prefs)
{
	CFMutableArrayRef	array;
	const void		**keys;
	CFIndex			i, n;
	CFStringRef		path;
	CFDictionaryRef		sets;

	path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@"),
					kSCPrefSets);
	sets = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if ((sets != NULL) &&
	    (CFGetTypeID(sets) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	n     = __copyDictKeys(sets, &keys);
	for (i = 0; i < n; i++) {
		SCNetworkSetPrivateRef	sp;

		sp = __SCNetworkSetCreatePrivate(NULL, prefs,
						 (CFStringRef)keys[i]);
		if (sp != NULL) {
			/* mark set as "old" (already established) */
			sp->established = TRUE;
			CFArrayAppendValue(array, sp);
			CFRelease(sp);
		}
	}
	if (keys != NULL)	free(keys);
	return array;
}

SCNetworkSetRef
SCNetworkSetCopyCurrent(SCPreferencesRef prefs)
{
	CFArrayRef	components;
	CFStringRef	currentID;
	SCNetworkSetRef	set	= NULL;

	currentID = SCPreferencesGetValue(prefs, kSCPrefCurrentSet);
	if ((currentID == NULL) ||
	    (CFGetTypeID(currentID) != CFStringGetTypeID())) {
		return NULL;
	}
	/* the value is the set's path "/Sets/<setID>" */
	components = CFStringCreateArrayBySeparatingStrings(NULL, currentID,
							    CFSTR("/"));
	if ((components != NULL) && (CFArrayGetCount(components) == 3)) {
		set = SCNetworkSetCopy(prefs,
				       CFArrayGetValueAtIndex(components, 2));
	}
	if (components != NULL) {
		CFRelease(components);
	}
	return set;
}

Boolean
SCNetworkSetSetCurrent(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path = __setPath(sp->setID);
	ok   = SCPreferencesSetValue(sp->prefs, kSCPrefCurrentSet, path);
	CFRelease(path);
	return ok;
}

CFStringRef
SCNetworkSetGetSetID(SCNetworkSetRef set)
{
	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkSetPrivateRef)set)->setID;
}

CFStringRef
SCNetworkSetGetName(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFStringRef		name;
	CFStringRef		path;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	if (sp->name != NULL) {
		return sp->name;
	}
	path = __setPath(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) == CFDictionaryGetTypeID())) {
		name = CFDictionaryGetValue(dict, kSCPropUserDefinedName);
		if ((name != NULL) &&
		    (CFGetTypeID(name) == CFStringGetTypeID())) {
			sp->name = (CFStringRef)CFRetain(name);
			return sp->name;
		}
	}
	return NULL;
}

Boolean
SCNetworkSetSetName(SCNetworkSetRef set, CFStringRef name)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((name != NULL) &&
	    (CFGetTypeID(name) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = __setPath(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) == CFDictionaryGetTypeID())) {
		newDict = CFDictionaryCreateMutableCopy(NULL, 0, dict);
	} else {
		newDict = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	if (name != NULL) {
		CFDictionarySetValue(newDict, kSCPropUserDefinedName, name);
	} else {
		CFDictionaryRemoveValue(newDict, kSCPropUserDefinedName);
	}

	ok = SCPreferencesPathSetValue(sp->prefs, path, newDict);
	CFRelease(newDict);
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
SCNetworkSetRemove(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFStringRef		currentPath;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path = __setPath(sp->setID);

	/* the active set may not be removed */
	currentPath = SCPreferencesGetValue(sp->prefs, kSCPrefCurrentSet);
	if ((currentPath != NULL) &&
	    (CFGetTypeID(currentPath) == CFStringGetTypeID()) &&
	    CFEqual(currentPath, path)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		CFRelease(path);
		return FALSE;
	}

	ok = SCPreferencesPathRemoveValue(sp->prefs, path);
	CFRelease(path);
	return ok;
}


#pragma mark -
#pragma mark Services in a set

/* TRUE iff a service named `name` is already a member of `set` */
static Boolean
__setHasServiceNamed(SCNetworkSetRef set, CFStringRef name)
{
	Boolean		found	= FALSE;
	CFIndex		i;
	CFIndex		n;
	CFArrayRef	services;

	if (name == NULL) {
		return FALSE;
	}
	services = SCNetworkSetCopyServices(set);
	if (services == NULL) {
		return FALSE;
	}
	n = CFArrayGetCount(services);
	for (i = 0; (i < n) && !found; i++) {
		CFStringRef	otherName;

		otherName = SCNetworkServiceGetName(CFArrayGetValueAtIndex(services, i));
		found = (otherName != NULL) && CFEqual(name, otherName);
	}
	CFRelease(services);
	return found;
}

static int
_serviceOrder(SCNetworkServiceRef service)
{
	SCNetworkInterfaceRef	interface;

	interface = SCNetworkServiceGetInterface(service);
	if (interface == NULL) {
		return 100000;	/* if unknown interface, sort last */
	}

	return __SCNetworkInterfaceOrder(interface);
}

static void
_serviceOrder_add(SCNetworkSetRef set, SCNetworkServiceRef service)
{
	CFIndex			i;
	CFIndex			n;
	CFMutableArrayRef	newOrder;
	CFArrayRef		order;
	CFStringRef		serviceID;
	CFIndex			serviceOrder;
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFIndex			slot;

	order = SCNetworkSetGetServiceOrder(set);
	if (order != NULL) {
		newOrder = CFArrayCreateMutableCopy(NULL, 0, order);
	} else {
		newOrder = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	}
	n = CFArrayGetCount(newOrder);

	serviceID = SCNetworkServiceGetServiceID(service);
	if (CFArrayContainsValue(newOrder, CFRangeMake(0, n), serviceID)) {
		/* if serviceID already present */
		goto done;
	}

	serviceOrder = _serviceOrder(service);

	slot = 0;
	for (i = 0; i < n; i++) {
		int			slotOrder;
		SCNetworkServiceRef	slotService;
		CFStringRef		slotServiceID;

		slotServiceID = CFArrayGetValueAtIndex(newOrder, i);
		if (!isA_CFString(slotServiceID)) {
			/* if bad prefs */
			continue;
		}

		slotService = SCNetworkServiceCopy(sp->prefs, slotServiceID);
		if (slotService == NULL) {
			/* if serviceID not valid */
			continue;
		}

		slotOrder = _serviceOrder(slotService);
		if (serviceOrder >= slotOrder) {
			/* add the service *after* this one */
			slot = i + 1;
		}

		CFRelease(slotService);
	}

	CFArrayInsertValueAtIndex(newOrder, slot, serviceID);
	(void) SCNetworkSetSetServiceOrder(set, newOrder);

    done :

	CFRelease(newOrder);
}

static void
_serviceOrder_remove(SCNetworkSetRef set, SCNetworkServiceRef service)
{
	CFMutableArrayRef	newOrder;
	CFArrayRef		order;
	CFStringRef		serviceID;

	order = SCNetworkSetGetServiceOrder(set);
	if (order == NULL) {
		return;
	}

	serviceID = SCNetworkServiceGetServiceID(service);

	newOrder = CFArrayCreateMutableCopy(NULL, 0, order);
	while (TRUE) {
		CFIndex	i;

		i = CFArrayGetFirstIndexOfValue(newOrder,
						CFRangeMake(0, CFArrayGetCount(newOrder)),
						serviceID);
		if (i == kCFNotFound) {
			break;
		}

		CFArrayRemoveValueAtIndex(newOrder, i);
	}
	(void) SCNetworkSetSetServiceOrder(set, newOrder);
	CFRelease(newOrder);
}

CFArrayRef
SCNetworkSetCopyServices(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFMutableArrayRef	array;
	CFDictionaryRef		dict;
	const void		**keys;
	CFIndex			i, n;
	CFStringRef		path;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = __setServicePath(sp->setID, NULL);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	n     = __copyDictKeys(dict, &keys);
	for (i = 0; i < n; i++) {
		SCNetworkServiceRef	service;

		/* each membership key is a serviceID; keep it only if the
		 * referenced service still exists */
		service = SCNetworkServiceCopy(sp->prefs,
					       (CFStringRef)keys[i]);
		if (service != NULL) {
			CFArrayAppendValue(array, service);
			CFRelease(service);
		}
	}
	if (keys != NULL)	free(keys);
	return array;
}

Boolean
SCNetworkSetAddService(SCNetworkSetRef set, SCNetworkServiceRef service)
{
	SCNetworkSetPrivateRef		sp	= (SCNetworkSetPrivateRef)set;
	SCNetworkServicePrivateRef	svcp	= (SCNetworkServicePrivateRef)service;
	CFStringRef			link;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!isA_SCNetworkService(service) || (svcp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	/* a service's name is unique within a set, as on Apple's */
	if (__setHasServiceNamed(set, SCNetworkServiceGetName(service))) {
		_SCErrorSet(kSCStatusKeyExists);
		return FALSE;
	}

	/* link /Sets/<set>/Network/Service/<svc> -> /NetworkServices/<svc> */
	path = __setServicePath(sp->setID, svcp->serviceID);
	link = __netServicePath(svcp->serviceID);
	ok   = SCPreferencesPathSetLink(sp->prefs, path, link);
	CFRelease(path);
	CFRelease(link);
	if (!ok) {
		return FALSE;
	}

	/* add service to ServiceOrder */
	_serviceOrder_add(set, service);

	/* mark set as no longer "new" */
	sp->established = TRUE;

	return TRUE;
}

Boolean
SCNetworkSetRemoveService(SCNetworkSetRef set, SCNetworkServiceRef service)
{
	SCNetworkSetPrivateRef		sp	= (SCNetworkSetPrivateRef)set;
	SCNetworkServicePrivateRef	svcp	= (SCNetworkServicePrivateRef)service;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = __setServicePath(sp->setID, svcp->serviceID);
	if (SCPreferencesPathGetLink(sp->prefs, path) == NULL) {
		/* the service is not a member of this set */
		_SCErrorSet(kSCStatusNoKey);
		CFRelease(path);
		return FALSE;
	}
	ok = SCPreferencesPathRemoveValue(sp->prefs, path);
	CFRelease(path);
	if (ok) {
		/* remove service from ServiceOrder */
		_serviceOrder_remove(set, service);
	}
	return ok;
}


#pragma mark -
#pragma mark Service order

CFArrayRef
SCNetworkSetGetServiceOrder(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFArrayRef		order;
	CFStringRef		path;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	path = __setGlobalIPv4Path(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		return NULL;
	}
	order = CFDictionaryGetValue(dict, kSCPropNetServiceOrder);
	if ((order == NULL) ||
	    (CFGetTypeID(order) != CFArrayGetTypeID())) {
		return NULL;
	}
	return order;
}

Boolean
SCNetworkSetSetServiceOrder(SCNetworkSetRef set, CFArrayRef newOrder)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFIndex			i, n;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newOrder == NULL) ||
	    (CFGetTypeID(newOrder) != CFArrayGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	n = CFArrayGetCount(newOrder);
	for (i = 0; i < n; i++) {
		CFTypeRef	sid = CFArrayGetValueAtIndex(newOrder, i);

		if (CFGetTypeID(sid) != CFStringGetTypeID()) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}
	}

	path = __setGlobalIPv4Path(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) == CFDictionaryGetTypeID())) {
		newDict = CFDictionaryCreateMutableCopy(NULL, 0, dict);
	} else {
		newDict = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	CFDictionarySetValue(newDict, kSCPropNetServiceOrder, newOrder);

	ok = SCPreferencesPathSetValue(sp->prefs, path, newDict);
	CFRelease(newDict);
	CFRelease(path);
	return ok;
}


#pragma mark -
#pragma mark SCNetworkSet SPIs

CFArrayRef
SCNetworkSetCopyAvailableInterfaces(SCNetworkSetRef set)
{
	CFMutableArrayRef	available;
	CFMutableSetRef		excluded	= NULL;
	CFIndex			i;
	CFArrayRef		interfaces;
	CFIndex			n_interfaces;
	CFIndex			n_exclusions	= 0;
	SCPreferencesRef	prefs;
	SCNetworkSetPrivateRef	setPrivate	= (SCNetworkSetPrivateRef)set;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	prefs = setPrivate->prefs;

	interfaces = _SCNetworkInterfaceCopyAllWithPreferences(prefs);
	n_interfaces = CFArrayGetCount(interfaces);
	if (n_interfaces == 0) {
		return interfaces;
	}

	if (prefs != NULL) {
		CFArrayRef	bonds;
		CFArrayRef	bridges;

		excluded = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

		bonds = SCBondInterfaceCopyAll(prefs);
		if (bonds != NULL) {
			__SCBondInterfaceListCollectMembers(bonds, excluded);
			CFRelease(bonds);
		}

		bridges = SCBridgeInterfaceCopyAll(prefs);
		if (bridges != NULL) {
			__SCBridgeInterfaceListCollectMembers(bridges, excluded);
			CFRelease(bridges);
		}

		n_exclusions = CFSetGetCount(excluded);
	}

	if (n_exclusions == 0) {
		if (excluded != NULL) {
			CFRelease(excluded);
		}

		return interfaces;
	}

	available = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	for (i = 0; i < n_interfaces; i++) {
		SCNetworkInterfaceRef	interface;

		interface = CFArrayGetValueAtIndex(interfaces, i);
		if (CFSetContainsValue(excluded, interface)) {
			// if excluded
			continue;
		}

		CFArrayAppendValue(available, interface);
	}

	CFRelease(interfaces);
	CFRelease(excluded);

	return available;
}

static CFSetRef	/* of SCNetworkInterfaceRef's */
copyExcludedInterfaces(SCPreferencesRef prefs)
{
	CFMutableSetRef	excluded;
	CFArrayRef	interfaces;

	excluded = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

	// exclude Bond [member] interfaces
	interfaces = SCBondInterfaceCopyAll(prefs);
	if (interfaces != NULL) {
		__SCBondInterfaceListCollectMembers(interfaces, excluded);
		CFRelease(interfaces);
	}

	// exclude Bridge [member] interfaces
	interfaces = SCBridgeInterfaceCopyAll(prefs);
	if (interfaces != NULL) {
		__SCBridgeInterfaceListCollectMembers(interfaces, excluded);
		CFRelease(interfaces);
	}

	return excluded;
}

static CFArrayRef
copyServices(SCNetworkSetRef set)
{
	CFArrayRef		services;
	SCNetworkSetPrivateRef	setPrivate	= (SCNetworkSetPrivateRef)set;

	// first, assume that we only want to add new services
	// for those interfaces that are not represented in the
	// current set.
	services = SCNetworkSetCopyServices(set);
	if ((services != NULL) && setPrivate->established) {
		// but, if we are given an existing (or "established") set
		// than we only want to add new services for those interfaces
		// that are not represented in *any* set.
		CFRelease(services);
		services = SCNetworkServiceCopyAll(setPrivate->prefs);
	}

	return services;
}

/* add a default service for `interface`; FALSE on failure */
static Boolean
__addDefaultService(SCNetworkSetRef set, SCNetworkInterfaceRef interface)
{
	Boolean			ok;
	SCNetworkServiceRef	service;
	SCNetworkSetPrivateRef	setPrivate	= (SCNetworkSetPrivateRef)set;

	service = SCNetworkServiceCreate(setPrivate->prefs, interface);
	if (service == NULL) {
		return FALSE;
	}

	ok = SCNetworkServiceEstablishDefaultConfiguration(service);
	if (!ok) {
		goto fail;
	}

	while (TRUE) {
		CFStringRef	newName;

		ok = SCNetworkSetAddService(set, service);
		if (ok) {
			break;
		}

		if (SCError() != kSCStatusKeyExists) {
			goto fail;
		}

		// we have two interfaces with the same service
		// name, acquire a new, hopefully unique, name

		newName = __SCNetworkServiceNextName(service);
		if (newName == NULL) {
			ok = FALSE;
			goto fail;
		}

		ok = SCNetworkServiceSetName(service, newName);
		CFRelease(newName);
		if (!ok) {
			goto fail;
		}
	}

	CFRelease(service);
	return TRUE;

    fail :

	(void) SCNetworkServiceRemove(service);
	CFRelease(service);
	return FALSE;
}

static Boolean
__SCNetworkSetEstablishDefaultConfigurationForInterfaces(SCNetworkSetRef set, CFArrayRef interfaces)
{
	CFSetRef		excluded;
	CFIndex			i;
	CFIndex			n;
	Boolean			ok		= TRUE;
	CFArrayRef		services;
	SCNetworkSetPrivateRef	setPrivate	= (SCNetworkSetPrivateRef)set;
	Boolean			updated		= FALSE;

	// copy network services
	services = copyServices(set);

	// copy network interfaces to be excluded
	excluded = copyExcludedInterfaces(setPrivate->prefs);

	n = (interfaces != NULL) ? CFArrayGetCount(interfaces) : 0;
	for (i = 0; ok && (i < n); i++) {
		SCNetworkInterfaceRef	interface;
		CFArrayRef		protocol_types;

		interface = CFArrayGetValueAtIndex(interfaces, i);
		if (CFSetContainsValue(excluded, interface)) {
			// if this interface is a member of a Bond or Bridge
			continue;
		}

		if (__SCNetworkServiceExistsForInterface(services, interface)) {
			// if this is not a new interface
			continue;
		}

		protocol_types = SCNetworkInterfaceGetSupportedProtocolTypes(interface);
		if ((protocol_types == NULL) || (CFArrayGetCount(protocol_types) == 0)) {
			// Apple's layers PPP on these; there is no PPP here
			continue;
		}

		ok = __addDefaultService(set, interface);
		if (ok) {
			updated = TRUE;
		}
	}

	if (services != NULL)	CFRelease(services);
	CFRelease(excluded);

	if (ok && !updated) {
		// if no changes were made
		_SCErrorSet(kSCStatusOK);
	}

	return updated;
}

Boolean
SCNetworkSetEstablishDefaultConfiguration(SCNetworkSetRef set)
{
	CFArrayRef		interfaces;
	SCNetworkSetPrivateRef	setPrivate	= (SCNetworkSetPrivateRef)set;
	Boolean			updated		= FALSE;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	interfaces = _SCNetworkInterfaceCopyAllWithPreferences(setPrivate->prefs);
	if (interfaces != NULL) {
		updated = __SCNetworkSetEstablishDefaultConfigurationForInterfaces(set, interfaces);
		CFRelease(interfaces);
	}

	return updated;
}

Boolean
SCNetworkSetEstablishDefaultInterfaceConfiguration(SCNetworkSetRef set, SCNetworkInterfaceRef interface)
{
	CFArrayRef	interfaces;
	Boolean		updated;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	interfaces = CFArrayCreate(NULL, (const void **)&interface, 1, &kCFTypeArrayCallBacks);
	updated = __SCNetworkSetEstablishDefaultConfigurationForInterfaces(set, interfaces);
	CFRelease(interfaces);

	return updated;
}
