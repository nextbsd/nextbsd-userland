/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
 * Ported from Gershwin's dscli by Joe Maloney
 * (gershwin-desktop/gershwin-components, DirectoryServices/dscli, BSD-2-Clause).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * libds: the DirectoryServices account database for the tools that edit
 * it (dscli, and the passwd/chpass/pw wrappers of E18 U9). It reads and
 * writes Users.plist and Groups.plist in Gershwin's format, atomically and
 * under a lock, hashes and verifies passwords, and drives the launchd jobs
 * and files that promote, join and leave touch. Plain libc plus libcrypt;
 * no CoreFoundation, no daemon.
 *
 * Every function that fails returns -1 (or NULL) with errno set, and the
 * callers print the message; nothing here writes to stderr.
 */

#ifndef LIBDS_DS_H
#define LIBDS_DS_H

#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- paths --------------------------------------------------------------- */

#define DS_LOCAL_DIR		"/Local/Library/DirectoryServices"
#define DS_NETWORK_DIR		"/Network/Library/DirectoryServices"
#define DS_USERS_PLIST		"Users.plist"
#define DS_GROUPS_PLIST		"Groups.plist"
#define DS_BINDING_PLIST	"Binding.plist"
#define DS_LOCK_FILE		".lock"
#define DS_LOCAL_USERS		"/Local/Users"
#define DS_NETWORK_USERS	"/Network/Users"
#define DS_SKEL_DIR		"/usr/share/skel"
#define DS_DEFAULT_SHELL	"/bin/zsh"
#define DS_ADMIN_GROUP		"admin"
#define DS_ADMIN_GID		5000
#define DS_FIRST_ID		1001

#define DS_LAUNCHDAEMONS	"/System/Library/LaunchDaemons"
#define DS_JOB_RPCBIND		"org.nextbsd.rpcbind"
#define DS_JOB_MOUNTD		"org.nextbsd.mountd"
#define DS_JOB_NFSD		"org.nextbsd.nfsd"
#define DS_JOB_NETWORK_MOUNT	"org.nextbsd.network-mount"
#define DS_JOB_NTPD		"org.nextbsd.ntpd"
#define DS_NETWORK_MOUNT_CMD	"/usr/libexec/nextbsd/network-mount"
#define DS_EXPORTS		"/etc/exports"
#define DS_NTP_CONF		"/etc/ntp.conf"
#define DS_SERVICE_DIR		"/Local/Library/Preferences/mDNSResponder/Services"
#define DS_SERVICE_FILE		"org.nextbsd.directory.plist"
#define DS_SERVICE_TYPE		"_nextbsd-ds._tcp"
#define DS_SERVICE_PORT		2049

/*
 * The directory in use: /Network when its Users.plist exists, else
 * /Local, the same rule as nss_directory_services. ds_set_dirs() points
 * the library at other roots (tests). ds_set_system_root() prefixes every
 * system path (/etc/exports, the LaunchDaemons, ntp.conf, the service
 * directory) for tests too.
 */
void		 ds_set_dirs(const char *local, const char *network);
void		 ds_set_system_root(const char *root);
const char	*ds_local_dir(void);
const char	*ds_network_dir(void);
const char	*ds_dir(void);			/* the one in use */
bool		 ds_from_network(void);
int		 ds_path(char *buf, size_t len, const char *dir, const char *file);
/* A system path with the test root applied; returns buf. */
const char	*ds_syspath(char *buf, size_t len, const char *path);

/* ---- records ------------------------------------------------------------- */

struct ds_user {
	char	*name;
	char	*real;		/* realName; "" when unset */
	char	*shell;		/* never NULL */
	char	*hash;		/* passwordHash or NULL */
	uid_t	 uid;
	gid_t	 gid;
	bool	 nopass;	/* noPassword */
};

struct ds_group {
	char	 *name;
	gid_t	  gid;
	char	**members;
	size_t	  nmembers;
};

struct ds_users {
	struct ds_user	*v;
	size_t		 n;
};

struct ds_groups {
	struct ds_group	*v;
	size_t		 n;
};

/*
 * Load a plist. A missing file loads as empty and returns 0 (errno ENOENT
 * is left set so callers that care can tell). A file that exists but
 * does not parse is -1 with errno EINVAL: never edit what cannot be read.
 */
int	ds_users_load(struct ds_users *us, const char *path);
int	ds_groups_load(struct ds_groups *gs, const char *path);
/* Write the whole file atomically (temp + fsync + rename), mode 0644. */
int	ds_users_save(const struct ds_users *us, const char *path);
int	ds_groups_save(const struct ds_groups *gs, const char *path);
void	ds_users_free(struct ds_users *us);
void	ds_groups_free(struct ds_groups *gs);

struct ds_user	*ds_user_find(const struct ds_users *us, const char *name);
struct ds_user	*ds_user_find_uid(const struct ds_users *us, uid_t uid);
/* Append a record with defaults (uid/gid 0 = unset, shell DS_DEFAULT_SHELL). */
struct ds_user	*ds_user_add(struct ds_users *us, const char *name);
int		 ds_user_remove(struct ds_users *us, const char *name);
int		 ds_set_string(char **field, const char *value);

struct ds_group	*ds_group_find(const struct ds_groups *gs, const char *name);
struct ds_group	*ds_group_find_gid(const struct ds_groups *gs, gid_t gid);
struct ds_group	*ds_group_add(struct ds_groups *gs, const char *name, gid_t gid);
int		 ds_group_remove(struct ds_groups *gs, const char *name);
bool		 ds_group_has_member(const struct ds_group *g, const char *user);
int		 ds_group_add_member(struct ds_group *g, const char *user);
int		 ds_group_remove_member(struct ds_group *g, const char *user);
/* Drop a user from every group; returns how many groups changed. */
int		 ds_groups_drop_member(struct ds_groups *gs, const char *user);

/*
 * The first id >= start that no plist record and no other name service
 * (getpwuid/getgrgid) uses, for both a uid and the matching private gid.
 */
uid_t	ds_next_id(const struct ds_users *us, const struct ds_groups *gs,
	    uid_t start);

/* A user or group name: 1-32 chars of [A-Za-z0-9._-], not starting with '-'. */
bool	ds_valid_name(const char *name);

/* ---- passwords ----------------------------------------------------------- */

/* SHA-512 crypt with 5000 rounds and a random 16-char salt; malloc'd. */
char	*ds_hash_password(const char *password);
/* True when the password matches the record (noPassword accepts ""). */
bool	 ds_verify_password(const struct ds_user *u, const char *password);

/* ---- files and locking --------------------------------------------------- */

/* flock(2) the database's lock file; returns the fd to pass to ds_unlock. */
int	ds_lock(const char *dir);
void	ds_unlock(int fd);
/* Write data to path via a temp file in the same directory plus rename. */
int	ds_write_atomic(const char *path, const char *data, size_t len,
	    mode_t mode);
/* Read a whole file into a malloc'd NUL-terminated buffer. */
char	*ds_read_file(const char *path, size_t *len);
/* mkdir -p with a mode; existing directories are fine. */
int	ds_mkdirs(const char *path, mode_t mode);
/* Create a home: mkdir 0755, skeleton dot files, chown to uid:gid. */
int	ds_make_home(const char *home, uid_t uid, gid_t gid);
/* rm -rf, refusing anything outside DS_LOCAL_USERS. */
int	ds_remove_home(const char *home);

/* ---- processes ----------------------------------------------------------- */

/* fork + execvp + wait. Returns the exit status, or -1 with errno. */
int	ds_run(char *const argv[]);
/* launchctl load|unload [-w] DS_LAUNCHDAEMONS/label.plist */
int	ds_launchctl(const char *verb, bool persist, const char *label);
bool	ds_job_installed(const char *label);	/* the plist exists */
bool	ds_job_loaded(const char *label);	/* launchctl list label */

/* ---- promote, join, leave ------------------------------------------------ */

enum ds_role {
	DS_STANDALONE,
	DS_SERVER,		/* /etc/exports written by promote exists */
	DS_JOINED		/* Binding.plist exists */
};

enum ds_role	ds_role(char *server, size_t len);
int		ds_binding_read(char *server, size_t len);
int		ds_binding_write(const char *server);
int		ds_binding_remove(void);
int		ds_exports_write(void);
int		ds_exports_remove(void);
bool		ds_exports_ours(void);
int		ds_service_file_write(const char *display_name);
int		ds_service_file_remove(void);
/* Set (or with NULL, clear) the server inside ntp.conf's managed block. */
int		ds_ntp_set_server(const char *host);

/* Escape &, <, > for XML text; malloc'd. */
char	*ds_xml_escape(const char *s);

#endif /* LIBDS_DS_H */
