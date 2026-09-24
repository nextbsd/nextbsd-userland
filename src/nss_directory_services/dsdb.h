/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
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
 * The DirectoryServices database: which plist is in use, a cached parse
 * of it, lookups, and packing records into caller-supplied buffers.
 * Everything here is plain libc so it can be unit-tested on any host;
 * the nsswitch glue lives in nss_directory_services.c.
 *
 * Callers hold dsdb_lock() across a refresh, the lookup and the pack, so
 * the record they read cannot be freed by a reload underneath them.
 */

#ifndef NSS_DS_DSDB_H
#define NSS_DS_DSDB_H

#include <sys/types.h>
#include <grp.h>
#include <pwd.h>
#include <stddef.h>

#define DS_LOCAL_DIR	"/Local/Library/DirectoryServices"
#define DS_NETWORK_DIR	"/Network/Library/DirectoryServices"
#define DS_USERS_PLIST	"Users.plist"
#define DS_GROUPS_PLIST	"Groups.plist"
#define DS_ETC_GROUP	"/etc/group"

#define DS_NOBODY_ID	65534
#define DS_DEFAULT_SHELL "/usr/sbin/nologin"

struct ds_user {
	char	*name;
	char	*real;		/* realName, "" when absent */
	char	*shell;		/* shell, DS_DEFAULT_SHELL when absent */
	char	*hash;		/* passwordHash, NULL when absent */
	uid_t	 uid;
	gid_t	 gid;
	int	 nopass;	/* noPassword */
};

struct ds_group {
	char	 *name;
	gid_t	  gid;
	char	**members;
	size_t	  nmembers;
};

enum dsdb_status {
	DSDB_OK,
	DSDB_UNAVAIL		/* file missing or unparsable: let `files` answer */
};

void	dsdb_lock(void);
void	dsdb_unlock(void);
void	dsdb_atfork_child(void);

/* Bring the cache up to date with whichever file is in use. Lock held. */
enum dsdb_status	dsdb_refresh_users(void);
enum dsdb_status	dsdb_refresh_groups(void);

/* True when the /Network copy of that plist is the one in use. Lock held. */
int	dsdb_users_from_network(void);

/* Lookups over the cached records. Lock held; results valid until unlock. */
const struct ds_user	*dsdb_user_by_name(const char *name);
const struct ds_user	*dsdb_user_by_uid(uid_t uid);
const struct ds_user	*dsdb_user_at(size_t index);
size_t			 dsdb_user_count(void);
const struct ds_group	*dsdb_group_by_name(const char *name);
const struct ds_group	*dsdb_group_by_gid(gid_t gid);
const struct ds_group	*dsdb_group_at(size_t index);
size_t			 dsdb_group_count(void);

/*
 * Pack a record into the caller's buffer. `privileged` selects the real
 * password field (the hash, or "" for noPassword) instead of "*".
 * `network` selects /Network/Users over /Local/Users for the home.
 * Return 0, or ERANGE when the buffer is too small.
 */
int	dsdb_pack_passwd(const struct ds_user *u, int privileged, int network,
	    struct passwd *pwd, char *buf, size_t buflen);
int	dsdb_pack_group(const struct ds_group *g, struct group *grp,
	    char *buf, size_t buflen);

/*
 * The group set for a user: the primary gid, every plist group whose
 * members list the user, wheel (gid 0) for members of `admin`, and the
 * hardware-access groups audio, video, render and input where `etcgroup`
 * defines them. Deduplicated. *grpcnt is set to the full count even when
 * it exceeds maxgrp, as getgrouplist(3) expects. Returns -1 when the user
 * is not in the users plist. Lock held.
 */
int	dsdb_membership(const char *name, gid_t agroup, gid_t *groups,
	    int maxgrp, int *grpcnt, const char *etcgroup);

#ifdef DSDB_TEST
/* Tests point the database at fixture directories. */
void	dsdb_set_dirs(const char *local, const char *network);
#endif

#endif /* NSS_DS_DSDB_H */
