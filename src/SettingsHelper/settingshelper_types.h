/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * settingshelper_types.h — C types and constants for settingshelper.defs,
 * shared by SettingsHelper (the server) and its clients (the settings
 * apps' NBAuthorization, apps-settings Frameworks/NBSettings). MIG emits
 * no typedef for a named `type`, so both sides import this header.
 *
 * Every argument is inline, a fixed-size array or a scalar, as in
 * wlan.defs: nothing here travels out of line, and a password never sits
 * in an out-of-line page.
 */
#ifndef _SETTINGSHELPER_TYPES_H
#define _SETTINGSHELPER_TYPES_H

#include <stdint.h>

#define	SH_SERVICE_NAME		"org.nextbsd.SettingsHelper"

/* A right: "org.nextbsd.settings.<area>", NUL-terminated. */
#define	SH_RIGHT_MAX		127
typedef char	SHRightName[SH_RIGHT_MAX + 1];

/* A user name, as given to PAM. */
#define	SH_USER_MAX		255
typedef char	SHUserName[SH_USER_MAX + 1];

/* A password, NUL-terminated; empty for an account without one. */
#define	SH_PASSWORD_MAX		255
typedef char	SHPassword[SH_PASSWORD_MAX + 1];

/*
 * The caller's authorization in external form, carried by every
 * operation. Today it is all zeroes and the helper decides from its own
 * per-session credentials. It is the size of Darwin's
 * AuthorizationExternalForm, so that when an authd takes over the rights
 * checks, the operations carry its token without a change to the wire.
 */
#define	SH_AUTH_FORM_SIZE	32
typedef char	SHAuthForm[SH_AUTH_FORM_SIZE];

/*
 * The answer to every call. A routine's kern_return_t only says whether
 * the message went through.
 */
enum {
	SH_OK = 0,
	SH_ERR_NOT_CONSOLE = 1,	/* caller isn't in the console session */
	SH_ERR_DENIED = 2,	/* the rule refuses this caller */
	SH_ERR_AUTH_NEEDED = 3,	/* authenticate first (sh_authorize) */
	SH_ERR_AUTH_FAILED = 4,	/* wrong name or password */
	SH_ERR_NOT_ADMIN = 5,	/* authenticated, but not an admin */
	SH_ERR_UNKNOWN_RIGHT = 6,
	SH_ERR_INVALID = 7,	/* an argument out of range */
	SH_ERR_UNSUPPORTED = 8,	/* not on this machine */
	SH_ERR_FAILED = 9,	/* the change itself failed */
	SH_ERR_EXISTS = 10,	/* an account or group of that name exists */
	SH_ERR_LAST_ADMIN = 11,	/* it would leave no administrator */
	SH_ERR_LOGGED_IN = 12,	/* the account is the console user's */
	SH_ERR_BUSY = 13	/* another update job or install is running */
};

/*
 * What a right's rule asks for, as sh_right_info reports it, so the
 * client knows what to show. The names are those of Darwin's
 * authorization database rules.
 */
enum {
	SH_RULE_DENY = 0,
	SH_RULE_ALLOW = 1,		/* "allow" */
	SH_RULE_IS_ADMIN = 2,		/* "is-admin": no prompt */
	SH_RULE_AUTH_ADMIN = 3,		/* "authenticate-admin" */
	SH_RULE_AUTH_SESSION_OWNER = 4	/* "authenticate-session-owner" */
};

/*
 * sh_set_pm_preferences: one power source's Energy Saver settings, by
 * index. Each entry is a value, SH_PM_UNCHANGED, or SH_PM_DEFAULT (the
 * system's default for that source). Timers are minutes, 0 for never;
 * the others are 0 or 1. New settings take the next free index; an index
 * never changes meaning.
 */
#define	SH_PM_UNCHANGED		(-1)
#define	SH_PM_DEFAULT		(-2)

enum {
	SH_PM_DISPLAY_SLEEP = 0,	/* "Display Sleep Timer" */
	SH_PM_SYSTEM_SLEEP = 1,		/* "System Sleep Timer" */
	SH_PM_DISK_SLEEP = 2,		/* "Disk Sleep Timer" */
	SH_PM_WAKE_ON_LAN = 3,		/* "Wake On LAN" */
	SH_PM_REDUCE_BRIGHTNESS = 4,	/* "ReduceBrightness" */
	SH_PM_TTYS_PREVENT_SLEEP = 5,	/* "TTYSPreventSleep" */
	SH_PM_KEY_LIMIT = 6		/* first unused index */
};

#define	SH_PM_VALUES		16
typedef int32_t	SHPMValues[SH_PM_VALUES];

/* sh_set_pm_preferences' power source. */
enum {
	SH_PM_SOURCE_AC = 0,		/* "AC Power" */
	SH_PM_SOURCE_BATTERY = 1,	/* "Battery Power" */
	SH_PM_SOURCE_UPS = 2		/* "UPS Power" */
};

/*
 * sh_set_time_zone's zone: a name under /usr/share/zoneinfo
 * ("America/Chicago"), NUL-terminated.
 */
#define	SH_ZONE_MAX		127
typedef char	SHZoneName[SH_ZONE_MAX + 1];

/*
 * sh_set_network_time's time server: a host name or an address,
 * NUL-terminated; empty to leave /etc/ntp.conf's servers as they are.
 */
#define	SH_NTP_SERVER_MAX	255
typedef char	SHNTPServer[SH_NTP_SERVER_MAX + 1];

/*
 * The Users & Login operations' full name (an account's realName),
 * NUL-terminated UTF-8. The short name is an SHUserName.
 */
#define	SH_FULL_NAME_MAX	255
typedef char	SHFullName[SH_FULL_NAME_MAX + 1];

/*
 * sh_cups_admin: what to do, and its strings, NUL-terminated. A printer
 * is a CUPS queue name (127 bytes at most, CUPS's own limit); the
 * device URI is ipp://, ipps:// or dnssd://; info and location are
 * free text without control characters, and may be empty.
 */
enum {
	SH_CUPS_ADD = 0,
	SH_CUPS_DELETE = 1
};

#define	SH_PRINTER_MAX		127
typedef char	SHPrinterName[SH_PRINTER_MAX + 1];
#define	SH_DEVICE_URI_MAX	1023
typedef char	SHDeviceURI[SH_DEVICE_URI_MAX + 1];
#define	SH_PRINTER_TEXT_MAX	255
typedef char	SHPrinterText[SH_PRINTER_TEXT_MAX + 1];

/*
 * sh_commit_network_preferences: a service's ID, an IPv4 or IPv6 address
 * as text, and a list of addresses or domain names, one per line, each
 * NUL-terminated.
 */
#define	SH_NET_SERVICE_MAX	63
typedef char	SHNetServiceID[SH_NET_SERVICE_MAX + 1];

#define	SH_NET_ADDRESS_MAX	63
typedef char	SHNetAddress[SH_NET_ADDRESS_MAX + 1];

#define	SH_NET_LIST_MAX		1023
#define	SH_NET_LIST_ENTRIES	8
typedef char	SHNetList[SH_NET_LIST_MAX + 1];

/* How a service gets its IPv4 address: Darwin's ConfigMethods. */
enum {
	SH_NET_V4_DHCP = 0,		/* "DHCP" */
	SH_NET_V4_DHCP_MANUAL = 1,	/* "INFORM": DHCP with a manual address */
	SH_NET_V4_MANUAL = 2,		/* "Manual" */
	SH_NET_V4_OFF = 3		/* the IPv4 entity inactive */
};

/* And its IPv6 address. */
enum {
	SH_NET_V6_AUTOMATIC = 0,	/* "Automatic" */
	SH_NET_V6_MANUAL = 1,		/* "Manual" */
	SH_NET_V6_LINK_LOCAL = 2,	/* "LinkLocal" */
	SH_NET_V6_OFF = 3		/* the IPv6 entity inactive */
};

/*
 * sh_set_computer_name: the ComputerName (UTF-8) and the LocalHostName
 * (a DNS label), NUL-terminated.
 */
#define	SH_COMPUTER_NAME_MAX	255
typedef char	SHComputerName[SH_COMPUTER_NAME_MAX + 1];

#define	SH_LOCAL_NAME_MAX	63
typedef char	SHLocalHostName[SH_LOCAL_NAME_MAX + 1];

/*
 * sh_set_system_locale's locale: a name under /usr/share/locale
 * ("de_DE.UTF-8"), or "C" or "POSIX", NUL-terminated.
 */
#define	SH_LOCALE_MAX		63
typedef char	SHLocaleName[SH_LOCALE_MAX + 1];

/*
 * The Update operations (Update.app, nextbsd/nextbsd#661). A version is
 * a package version ("27.0.0.181"); the text is a short status line.
 * Both NUL-terminated.
 */
#define	SH_UPDATE_VERSION_MAX	63
typedef char	SHUpdateVersion[SH_UPDATE_VERSION_MAX + 1];
#define	SH_UPDATE_TEXT_MAX	127
typedef char	SHUpdateText[SH_UPDATE_TEXT_MAX + 1];

/* sh_update_status' job: the one running, or the last one. */
enum {
	SH_UPDATE_JOB_NONE = 0,
	SH_UPDATE_JOB_CHECK = 1,
	SH_UPDATE_JOB_DOWNLOAD = 2,
	SH_UPDATE_JOB_INSTALL = 3
};

/* And how it went. */
enum {
	SH_UPDATE_IDLE = 0,		/* no job since the helper started */
	SH_UPDATE_RUNNING = 1,
	SH_UPDATE_DONE = 2,
	SH_UPDATE_FAILED = 3,
	SH_UPDATE_CANCELLED = 4
};

/* sh_update_status' restart: is an installed update waiting for one? */
enum {
	SH_UPDATE_RESTART_NONE = 0,
	SH_UPDATE_RESTART_PENDING = 1,	/* installed since this boot */
	SH_UPDATE_RESTART_DONE = 2	/* installed, and restarted since */
};

/*
 * Installer.app's operations. The target disk is a GEOM disk name
 * ("ada1", "da0", "nvd1"), NUL-terminated; sh_install_start's mode says
 * what the user was promised, installing NextBSD from the install image
 * or copying the running system, and must match what the system is.
 */
#define	SH_DISK_MAX		31
typedef char	SHDiskName[SH_DISK_MAX + 1];

enum {
	SH_INSTALL_IMAGE = 0,		/* do-install.sh */
	SH_INSTALL_COPY = 1		/* do-install.sh -c */
};

/* sh_install_progress's state. */
enum {
	SH_INSTALL_IDLE = 0,		/* none started (or long forgotten) */
	SH_INSTALL_RUNNING = 1,
	SH_INSTALL_DONE = 2,
	SH_INSTALL_FAILED = 3
};

/* And its phase, do-install.sh's PHASE lines. */
enum {
	SH_INSTALL_PREPARE = 0,		/* partitioning, filesystems */
	SH_INSTALL_COPYING = 1,		/* the system onto the disk */
	SH_INSTALL_SETUP = 2,		/* boot configuration, boot code */
	SH_INSTALL_FINISH = 3
};

#endif /* _SETTINGSHELPER_TYPES_H */
