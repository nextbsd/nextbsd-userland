/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_rights.c — rights, rules and the per-session credential cache.
 *
 * The rights are "org.nextbsd.settings.<area>", the settings apps', and
 * "org.nextbsd.installer", Installer.app's.
 *
 * The rules are in /System/Library/Security/SettingsRights.plist, and a
 * site overrides any right or rule in /Local/Library/Security/
 * SettingsRights.plist. The shape is Darwin's authorization database
 * (/System/Library/Security/authorization.plist):
 *
 *	rights = {
 *	    "org.nextbsd.settings.energy" = { class = rule;
 *	        rule = "authenticate-admin"; };
 *	};
 *	rules = {
 *	    allow = { class = allow; };
 *	    deny = { class = deny; };
 *	    "is-admin" = { class = user; group = admin;
 *	        "authenticate-user" = false; };
 *	    "authenticate-admin" = { class = user; group = admin;
 *	        timeout = 300; };
 *	    "authenticate-session-owner" = { class = user;
 *	        "session-owner" = true; timeout = 300; };
 *	};
 *
 * A right is either `class = rule` naming a rule (a string, or an array
 * whose first element is used), or a rule itself. Class `user` takes
 * `group`, `session-owner`, `authenticate-user` (default true) and
 * `timeout` (seconds, default 300, as on macOS). The files are read on
 * every request; they are a few hundred bytes.
 *
 * Credentials: an authentication leaves a credential (the account that
 * authenticated, and when) for the caller's session, the console
 * session's leader and its start time. A right whose rule asks for
 * authentication is granted while a credential that satisfies it is
 * younger than the rule's timeout. Timeout 0 means the credential is good
 * for one operation. Credentials die with the helper, and the helper
 * stays up while any is live (SettingsHelper.c).
 */
#include <sys/types.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	SYSTEM_RIGHTS	"/System/Library/Security/SettingsRights.plist"
#define	LOCAL_RIGHTS	"/Local/Library/Security/SettingsRights.plist"

#define	DEFAULT_TIMEOUT	300
#define	ONESHOT_LIFE	60	/* a timeout-0 credential unused this long */
#define	MAX_CREDS	32
#define	MAX_DEPTH	8

struct cred {
	bool		used;
	pid_t		leader;
	long long	leader_start;
	uid_t		uid;
	time_t		when;
	time_t		expires;
};

static struct cred creds[MAX_CREDS];

/* ---- the rules files --------------------------------------------------- */

static CFDictionaryRef
load_plist(const char *path)
{
	CFURLRef url;
	CFReadStreamRef stream;
	CFPropertyListRef plist = NULL;

	url = CFURLCreateFromFileSystemRepresentation(NULL,
	    (const UInt8 *)path, (CFIndex)strlen(path), false);
	if (url == NULL)
		return (NULL);
	stream = CFReadStreamCreateWithFile(NULL, url);
	CFRelease(url);
	if (stream == NULL)
		return (NULL);
	if (CFReadStreamOpen(stream))
		plist = CFPropertyListCreateWithStream(NULL, stream, 0,
		    kCFPropertyListImmutable, NULL, NULL);
	CFReadStreamClose(stream);
	CFRelease(stream);
	if (plist != NULL && CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		plist = NULL;
	}
	return (plist);
}

/* section ("rights" or "rules") -> name, Local before System. */
static CFDictionaryRef
lookup(CFDictionaryRef local, CFDictionaryRef sys, const char *section,
    CFStringRef name)
{
	CFDictionaryRef files[2] = { local, sys };
	CFStringRef key;
	CFTypeRef sec, v;
	int i;

	key = CFStringCreateWithCString(NULL, section, kCFStringEncodingUTF8);
	for (i = 0; i < 2; i++) {
		if (files[i] == NULL)
			continue;
		sec = CFDictionaryGetValue(files[i], key);
		if (sec == NULL || CFGetTypeID(sec) != CFDictionaryGetTypeID())
			continue;
		v = CFDictionaryGetValue(sec, name);
		if (v != NULL && CFGetTypeID(v) == CFDictionaryGetTypeID()) {
			CFRelease(key);
			return (v);
		}
	}
	CFRelease(key);
	return (NULL);
}

static CFTypeRef
get(CFDictionaryRef d, const char *k)
{
	CFStringRef key;
	CFTypeRef v;

	key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
	v = CFDictionaryGetValue(d, key);
	CFRelease(key);
	return (v);
}

static bool
get_bool(CFDictionaryRef d, const char *k, bool fallback)
{
	CFTypeRef v = get(d, k);

	if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID())
		return (CFBooleanGetValue(v));
	return (fallback);
}

static bool
equals(CFTypeRef v, const char *s)
{
	char buf[64];

	return (v != NULL && CFGetTypeID(v) == CFStringGetTypeID() &&
	    CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8) &&
	    strcmp(buf, s) == 0);
}

static bool
resolve(CFDictionaryRef local, CFDictionaryRef sys, CFDictionaryRef def,
    int depth, struct sh_rule *rule)
{
	CFTypeRef cls = get(def, "class"), v;

	if (depth > MAX_DEPTH)
		return (false);
	if (equals(cls, "allow")) {
		rule->kind = SH_RULE_ALLOW;
		return (true);
	}
	if (equals(cls, "deny")) {
		rule->kind = SH_RULE_DENY;
		return (true);
	}
	if (equals(cls, "rule")) {
		CFDictionaryRef next;

		v = get(def, "rule");
		if (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID())
			v = CFArrayGetCount(v) > 0 ?
			    CFArrayGetValueAtIndex(v, 0) : NULL;
		if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
			return (false);
		next = lookup(local, sys, "rules", v);
		return (next != NULL &&
		    resolve(local, sys, next, depth + 1, rule));
	}
	if (equals(cls, "user")) {
		bool owner = get_bool(def, "session-owner", false);
		bool authenticate = get_bool(def, "authenticate-user", true);

		rule->group[0] = '\0';
		v = get(def, "group");
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
			(void)CFStringGetCString(v, rule->group,
			    sizeof(rule->group), kCFStringEncodingUTF8);
		rule->timeout = DEFAULT_TIMEOUT;
		v = get(def, "timeout");
		if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
			int t;

			if (CFNumberGetValue(v, kCFNumberIntType, &t) && t >= 0)
				rule->timeout = t;
		}
		if (owner)
			rule->kind = authenticate ? SH_RULE_AUTH_SESSION_OWNER :
			    SH_RULE_ALLOW;	/* the console user owns it */
		else
			rule->kind = authenticate ? SH_RULE_AUTH_ADMIN :
			    SH_RULE_IS_ADMIN;
		return (true);
	}
	return (false);
}

static bool
valid_right_name(const char *right)
{
	const char *p;

	if ((strncmp(right, "org.nextbsd.settings.", 21) != 0 ||
	    right[21] == '\0') && strcmp(right, "org.nextbsd.installer") != 0)
		return (false);
	for (p = right; *p != '\0'; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		    *p == '.' || *p == '-'))
			return (false);
	return (true);
}

bool
sh_rule_for_right(const char *right, struct sh_rule *rule)
{
	CFDictionaryRef local, sys, def;
	CFStringRef name;
	bool ok = false;

	memset(rule, 0, sizeof(*rule));
	rule->kind = SH_RULE_DENY;
	if (!valid_right_name(right))
		return (false);
	local = load_plist(LOCAL_RIGHTS);
	sys = load_plist(SYSTEM_RIGHTS);
	name = CFStringCreateWithCString(NULL, right, kCFStringEncodingUTF8);
	def = lookup(local, sys, "rights", name);
	if (def != NULL)
		ok = resolve(local, sys, def, 0, rule);
	if (def != NULL && !ok)
		sh_log("right %s: its rule can't be read; denied", right);
	CFRelease(name);
	if (local != NULL)
		CFRelease(local);
	if (sys != NULL)
		CFRelease(sys);
	return (ok);
}

/* ---- credentials ------------------------------------------------------- */

bool
sh_rights_live(void)
{
	time_t now = sh_now();
	bool any = false;
	int i;

	for (i = 0; i < MAX_CREDS; i++) {
		if (creds[i].used && now >= creds[i].expires)
			memset(&creds[i], 0, sizeof(creds[i]));
		any |= creds[i].used;
	}
	return (any);
}

static bool
same_session(const struct cred *c, const struct sh_caller *caller)
{
	return (c->used && c->leader == caller->console.leader &&
	    c->leader_start == caller->console.leader_start);
}

static bool
satisfies(uid_t uid, const struct sh_rule *rule,
    const struct sh_caller *caller)
{
	if (rule->kind == SH_RULE_AUTH_SESSION_OWNER)
		return (uid == caller->console.uid);
	if (rule->group[0] == '\0')
		return (true);
	return (sh_user_in_group(uid, rule->group));
}

/* The best credential of the caller's session for `rule`, or NULL. */
static struct cred *
find_cred(const struct sh_caller *caller, const struct sh_rule *rule,
    int *seconds_left)
{
	struct cred *best = NULL;
	time_t now = sh_now(), left, best_left = 0;
	int i;

	(void)sh_rights_live();
	for (i = 0; i < MAX_CREDS; i++) {
		struct cred *c = &creds[i];

		if (!same_session(c, caller) || !satisfies(c->uid, rule, caller))
			continue;
		if (rule->timeout == 0)
			left = c->expires - now;
		else
			left = c->when + rule->timeout - now;
		if (left > 0 && left > best_left) {
			best = c;
			best_left = left;
		}
	}
	if (seconds_left != NULL)
		*seconds_left = best != NULL ? (int)best_left : 0;
	return (best);
}

int
sh_rights_check(const struct sh_caller *caller, const char *right,
    bool consume, int *seconds_left)
{
	struct sh_rule rule;
	struct cred *c;

	if (seconds_left != NULL)
		*seconds_left = 0;
	if (!sh_rule_for_right(right, &rule))
		return (SH_ERR_UNKNOWN_RIGHT);
	if (caller->root)
		return (SH_OK);
	if (!caller->in_console)
		return (SH_ERR_NOT_CONSOLE);
	switch (rule.kind) {
	case SH_RULE_ALLOW:
		return (SH_OK);
	case SH_RULE_IS_ADMIN:
		return (satisfies(caller->euid, &rule, caller) ? SH_OK :
		    SH_ERR_DENIED);
	case SH_RULE_AUTH_ADMIN:
	case SH_RULE_AUTH_SESSION_OWNER:
		c = find_cred(caller, &rule, seconds_left);
		if (c == NULL)
			return (SH_ERR_AUTH_NEEDED);
		if (consume && rule.timeout == 0)
			memset(c, 0, sizeof(*c));
		return (SH_OK);
	default:
		return (SH_ERR_DENIED);
	}
}

int
sh_rights_authorize(const struct sh_caller *caller, const char *right,
    const char *user, const char *password, int *seconds_left)
{
	struct sh_rule rule;
	struct cred *c;
	uid_t uid;
	int i, rc;

	*seconds_left = 0;
	rc = sh_rights_check(caller, right, false, seconds_left);
	if (rc != SH_ERR_AUTH_NEEDED)
		return (rc);
	(void)sh_rule_for_right(right, &rule);

	if (!sh_pam_check(user, password, &uid)) {
		sh_log("pid %d: %s: authentication as \"%s\" failed",
		    (int)caller->pid, right, user);
		sleep(1);	/* as authd: slow down guessing */
		return (SH_ERR_AUTH_FAILED);
	}
	if (!satisfies(uid, &rule, caller)) {
		sh_log("pid %d: %s: \"%s\" may not allow this", (int)caller->pid,
		    right, user);
		return (SH_ERR_NOT_ADMIN);
	}

	/* Replace the session's credential for this account, or the
	 * oldest one if the table is full. */
	c = NULL;
	for (i = 0; i < MAX_CREDS && c == NULL; i++)
		if (same_session(&creds[i], caller) && creds[i].uid == uid)
			c = &creds[i];
	for (i = 0; i < MAX_CREDS && c == NULL; i++)
		if (!creds[i].used)
			c = &creds[i];
	if (c == NULL) {
		c = &creds[0];
		for (i = 1; i < MAX_CREDS; i++)
			if (creds[i].when < c->when)
				c = &creds[i];
	}
	memset(c, 0, sizeof(*c));
	c->used = true;
	c->leader = caller->console.leader;
	c->leader_start = caller->console.leader_start;
	c->uid = uid;
	c->when = sh_now();
	c->expires = c->when + (rule.timeout > 0 ? rule.timeout : ONESHOT_LIFE);
	*seconds_left = rule.timeout > 0 ? rule.timeout : ONESHOT_LIFE;
	sh_log("pid %d: %s: allowed by \"%s\" for %d s", (int)caller->pid,
	    right, user, *seconds_left);
	return (SH_OK);
}

void
sh_rights_forget(const struct sh_caller *caller)
{
	int i;

	for (i = 0; i < MAX_CREDS; i++)
		if (same_session(&creds[i], caller))
			memset(&creds[i], 0, sizeof(creds[i]));
}
