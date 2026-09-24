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
 * libds — read and write the DirectoryServices plists
 * (nextbsd/nextbsd-userland#287, E18 U15).
 *
 * The account tools and the ds* commands share this. It is a thin layer
 * over CoreFoundation's property list implementation, which is Apple's
 * own and already in the tree: nothing here serialises XML by hand.
 *
 * It is NOT for nss_directory_services. That module links nothing but
 * libc on purpose, because it is dlopened into every process that calls
 * getpwnam(3), and it keeps its own read-only parser. libds is for
 * ordinary programs, which may link whatever they like.
 *
 * Two rules the caller has to know about:
 *
 *   Domains do not fall back on write. Reads elsewhere prefer the
 *   /Network copy of a plist over the /Local one; a writer cannot guess,
 *   because adduser(8) means local and dspromote(8) means network. So the
 *   domain is an argument, not a discovery.
 *
 *   A commit writes Groups.plist before Users.plist, always. Promoting a
 *   user touches both files and there is no cross-file atomicity, so the
 *   order is chosen to fail safe: on an add, the membership lands first
 *   naming a user who does not exist yet, which is inert; on a removal,
 *   privilege is revoked before the identity is. An interrupted commit
 *   therefore never leaves a privilege half-granted.
 */

#ifndef LIBDS_H
#define LIBDS_H

#include <sys/types.h>

#include <stdbool.h>
#include <stddef.h>

#define DS_LOCAL_DIR	"/Local/Library/DirectoryServices"
#define DS_NETWORK_DIR	"/Network/Library/DirectoryServices"
#define DS_USERS_PLIST	"Users.plist"
#define DS_GROUPS_PLIST	"Groups.plist"
#define DS_LOCK_FILE	".lock"

/*
 * Field sizes. Records are fixed-width so the caller never owns a
 * pointer into the handle and nothing has to be freed.
 */
#define DS_NAME_MAX	33	/* login/group name, MAXLOGNAME + NUL */
#define DS_REAL_MAX	256	/* realName */
#define DS_SHELL_MAX	1024	/* shell path */
#define DS_HASH_MAX	512	/* passwordHash */

enum ds_domain {
	DS_LOCAL,
	DS_NETWORK
};

/* Open modes. DS_RDWR takes the domain lock and holds it until close. */
enum ds_mode {
	DS_RDONLY,
	DS_RDWR
};

enum ds_error {
	DS_OK = 0,
	DS_ENOENT,	/* no such record */
	DS_EEXIST,	/* record already present */
	DS_EPARSE,	/* the file is there but is not a plist we understand */
	DS_EIO,		/* read, write or rename failed; errno is set */
	DS_ELOCK,	/* the domain lock could not be taken; errno is set */
	DS_EINVAL,	/* bad argument, or a value that will not fit */
	DS_ERANGE	/* more members than the caller asked for */
};

/* Human-readable form of an error, for a tool's diagnostic. */
const char	*ds_strerror(enum ds_error err);

struct ds_handle;

struct ds_userrec {
	char	username[DS_NAME_MAX];
	char	realName[DS_REAL_MAX];
	char	shell[DS_SHELL_MAX];
	char	passwordHash[DS_HASH_MAX];
	uid_t	uid;
	gid_t	gid;
	bool	noPassword;
	bool	hasHash;	/* false when the record carries no hash */
};

struct ds_grouprec {
	char	groupname[DS_NAME_MAX];
	gid_t	gid;
};

/*
 * Open a domain. Both Users.plist and Groups.plist are read; a file that
 * does not exist yet reads as empty, so a first write creates it. With
 * DS_RDWR the lock is taken before either file is read, so what the
 * caller sees cannot change under it.
 */
enum ds_error	 ds_open(enum ds_domain domain, enum ds_mode mode,
		     struct ds_handle **out);
void		 ds_close(struct ds_handle *h);

/* The directory this handle writes to, for diagnostics. */
const char	*ds_dir(const struct ds_handle *h);

/* True when there are uncommitted changes. */
bool		 ds_dirty(const struct ds_handle *h);

/* ---- users ---------------------------------------------------------- */

enum ds_error	 ds_user_get(struct ds_handle *h, const char *name,
		     struct ds_userrec *out);
enum ds_error	 ds_user_get_by_uid(struct ds_handle *h, uid_t uid,
		     struct ds_userrec *out);
size_t		 ds_user_count(struct ds_handle *h);
enum ds_error	 ds_user_at(struct ds_handle *h, size_t i,
		     struct ds_userrec *out);

/* Fails with DS_EEXIST when the name is taken. */
enum ds_error	 ds_user_add(struct ds_handle *h, const struct ds_userrec *u);
/*
 * Update an existing record. DS_ENOENT when absent. Keys libds does not
 * know about are preserved, because Gershwin's dscli writes these same
 * files and an admin may have added something by hand. The keys libds does
 * own are cleared when the caller clears them.
 */
enum ds_error	 ds_user_set(struct ds_handle *h, const struct ds_userrec *u);
enum ds_error	 ds_user_del(struct ds_handle *h, const char *name);

/*
 * The first unused uid at or above `from`, skipping anything present in
 * either the plist or, when `skip_pwd` is true, the system passwd
 * database. DS_ENOENT when the space up to `to` is exhausted.
 */
enum ds_error	 ds_user_next_uid(struct ds_handle *h, uid_t from, uid_t to,
		     bool skip_pwd, uid_t *out);

/* ---- groups --------------------------------------------------------- */

enum ds_error	 ds_group_get(struct ds_handle *h, const char *name,
		     struct ds_grouprec *out);
enum ds_error	 ds_group_get_by_gid(struct ds_handle *h, gid_t gid,
		     struct ds_grouprec *out);
size_t		 ds_group_count(struct ds_handle *h);
enum ds_error	 ds_group_at(struct ds_handle *h, size_t i,
		     struct ds_grouprec *out);

enum ds_error	 ds_group_add(struct ds_handle *h, const struct ds_grouprec *g);
enum ds_error	 ds_group_del(struct ds_handle *h, const char *name);

size_t		 ds_group_member_count(struct ds_handle *h, const char *group);
enum ds_error	 ds_group_member_at(struct ds_handle *h, const char *group,
		     size_t i, char *buf, size_t buflen);
enum ds_error	 ds_group_has_member(struct ds_handle *h, const char *group,
		     const char *user, bool *out);
/* Adding a member already present succeeds and changes nothing. */
enum ds_error	 ds_group_add_member(struct ds_handle *h, const char *group,
		     const char *user);
enum ds_error	 ds_group_del_member(struct ds_handle *h, const char *group,
		     const char *user);

/* ---- commit --------------------------------------------------------- */

/*
 * Write both files, Groups.plist first. Each is written to a temporary
 * file in the same directory, fsynced, and renamed into place, so a
 * reader sees either the old file or the new one. Mode and owner are
 * carried over from the file being replaced; a file being created for
 * the first time gets the directory's owner and mode 0644.
 *
 * A file with no pending change is not rewritten. On failure the handle
 * stays dirty and nothing has been renamed for that file.
 */
enum ds_error	 ds_commit(struct ds_handle *h);

#ifdef LIBDS_TEST
/* Tests point the library at fixture directories. */
void		 ds_set_dirs(const char *local, const char *network);
#endif

#endif /* LIBDS_H */
