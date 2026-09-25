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
 * Shared helpers for the NextBSD account tools (E18 U9, U14).
 *
 * libds owns the plists and nothing else, by the decision recorded on
 * nextbsd-userland#287. The pieces below are the ones more than one tool
 * needs and that are not about plists: hashing a password, deciding
 * whether a new account belongs in the directory or in master.passwd, and
 * resolving what this machine is -- server, bound client or standalone --
 * so a client does not grow local accounts the server will shadow.
 *
 * They live here rather than in each tool so the rules cannot drift
 * between adduser(8), passwd(1) and pw(8).
 */

#ifndef ACCT_H
#define ACCT_H

#include <sys/types.h>

#include <stdbool.h>
#include <stddef.h>

/*
 * Identity ranges. System accounts stay in master.passwd; regular users
 * and their private groups start above the gap. `admin` is reserved at
 * the bottom of the range so it is stable across installs.
 */
#define ACCT_SYSTEM_MAX		999	/* <= this is a system account */
#define ACCT_ADMIN_ID		5000	/* the admin group, and the seeded admin user */
#define ACCT_FIRST_ID		5001	/* first regular user and private group */
#define ACCT_LAST_ID		65533	/* stop before nobody */

#define ACCT_ADMIN_GROUP	"admin"
#define ACCT_DEFAULT_SHELL	"/bin/zsh"

/*
 * Paths, overridable at build time so the tools can be exercised against a
 * fixture directory on a build host without running as root or touching the
 * real system.
 */
#ifndef ACCT_SHELLS
#define ACCT_SHELLS		"/etc/shells"
#endif
/*
 * "This record is not ours to change." One value across all five tools: they
 * used to exit 2 (adduser, rmuser), 1 (passwd, chpass) and 77 (pw) for the
 * identical condition.
 */
#ifndef ACCT_EX_REFUSED
#define ACCT_EX_REFUSED		2
#endif
#ifndef ACCT_LOCAL_DOMAIN
#define ACCT_LOCAL_DOMAIN	"/Local/Library/DirectoryServices/Domain.plist"
#endif
#ifndef ACCT_NETWORK_DOMAIN
#define ACCT_NETWORK_DOMAIN	"/Network/Library/DirectoryServices/Domain.plist"
#endif
#ifndef ACCT_BINDING_PLIST
#define ACCT_BINDING_PLIST	"/Local/Library/DirectoryServices/Binding.plist"
#endif
#ifndef ACCT_CREATEHOMEDIR
#define ACCT_CREATEHOMEDIR	"/usr/sbin/createhomedir"
#endif
#ifndef ACCT_LOCAL_USERS
#define ACCT_LOCAL_USERS	"/Local/Users"
#endif
#ifndef ACCT_NETWORK_USERS_DIR
#define ACCT_NETWORK_USERS_DIR	"/Network/Users"
#endif

/*
 * Hash a password for storage in passwordHash, into the caller's buffer.
 * The format comes from login.conf's passwd_format when it names one
 * crypt(3) implements, defaulting to SHA-512, which is what Gershwin's
 * dscli also writes, so either tool's hashes work with the other.
 *
 * Refuses rather than downgrading: if crypt(3) cannot produce the format
 * asked for it falls back silently, and this returns false instead of
 * storing something weaker.
 *
 * The caller passes a buffer rather than getting a pointer back because an
 * earlier version returned static storage, and the first test to hold two
 * results at once compared the same buffer with itself. A function whose
 * second call invalidates the first result is a trap however well it is
 * documented.
 */
bool		 acct_hash_password(const char *password, char *out,
		     size_t len);

/*
 * True when this name must go to master.passwd rather than the directory:
 * a leading underscore, which is Darwin's and the ports framework's
 * convention for a service account.
 */
bool		 acct_is_system_name(const char *name);

/*
 * Why a name is unusable, as a phrase for a diagnostic, or NULL when it is
 * fine. Here rather than in each tool so adduser(8), pw(8) and rmuser(8)
 * cannot disagree about what a name may be.
 *
 * A leading digit is refused. A wholly numeric name is ambiguous with a uid
 * everywhere it appears, and a leading digit still confuses chown(8) and
 * anything else that accepts either form in the same argument.
 */
const char	*acct_name_problem(const char *name);

/* True when the id is inside the system range. */
bool		 acct_is_system_id(uid_t id);

/*
 * The shell is listed in /etc/shells. A missing or unreadable
 * /etc/shells means we cannot tell, so this returns true rather than
 * blocking every account on a broken file.
 */
bool		 acct_shell_listed(const char *shell);

/*
 * When this machine is bound to a directory server, copy its name into
 * `server` and return true. Local account tools refuse in that case: the
 * account belongs on the server, and a local one would be shadowed by
 * the /Network copy of the plists anyway.
 *
 * Asked in one order, the first answer winning -- the same order the ds*
 * commands and Gershwin's dscli use:
 *
 *	ACCT_LOCAL_DOMAIN exists	this machine IS the server. False, and
 *					/Network is not consulted at all.
 *	ACCT_NETWORK_DOMAIN exists	bound. True.
 *	neither				standalone. False.
 *
 * So false means "manage the account here" for both a server and a standalone
 * machine, which is what the caller needs; only a bound client refuses.
 * ACCT_BINDING_PLIST names the server for the message and decides nothing.
 */
bool		 acct_bound_server(char *server, size_t len);

/*
 * Build a home for `user` from /System/Library/User Template by running
 * createhomedir(8). Returns 0, or -1 with a message already printed.
 * Never fatal to the account itself: the account exists by this point,
 * and createhomedir can be re-run.
 */
int		 acct_make_home(const char *user, bool quiet);

/*
 * The sentinel that marks a locked account. passwd -l prefixes the stored
 * hash with it and passwd -u strips it back off. A prefix rather than a new
 * key, so the hash survives the round trip and so every consumer that
 * compares through crypt(3) refuses a locked account without being taught
 * anything: no salt can produce this string, and Gershwin's dshelper is one
 * of those consumers.
 */
#define ACCT_LOCK_PREFIX	"*LOCKED*"

/* True when the stored hash carries the lock sentinel. */
bool		 acct_hash_locked(const char *hash);

/*
 * Verify a plaintext password against a stored crypt(3) hash. False for a
 * locked account, for an empty stored hash, and for anything crypt cannot
 * process. The comparison is constant-time in the length of the hash, so a
 * caller cannot learn where it stopped matching.
 */
bool		 acct_verify_password(const char *plain, const char *stored);

/* Where per-user cron and at state lives, for removal. */
#ifndef ACCT_CRON_TABS
#define ACCT_CRON_TABS		"/var/cron/tabs"
#endif
#ifndef ACCT_AT_JOBS
#define ACCT_AT_JOBS		"/var/at/jobs"
#endif

/*
 * How many login sessions the named user has open, from the utmpx
 * database. Used to warn before killing them, because a silent SIGKILL of
 * somebody's shell is a surprise worth announcing.
 */
int		 acct_sessions(const char *name);

/*
 * SIGKILL every process owned by uid, except our own and our parent's, so
 * a tool does not kill the shell that invoked it. Returns how many signals
 * were sent, or -1 on failure to enumerate.
 */
int		 acct_kill_uid(uid_t uid);

/*
 * Remove a user's home. Refuses anything that is not directly under
 * /Local/Users or /Network/Users, so a bad name cannot turn into a wider
 * delete. On a server the /Network root is not tried at all, since anything
 * there could only be the machine's own export seen back through a mount.
 * Returns 0, -1 with errno set, or 1 when there was nothing there.
 */
int		 acct_remove_home(const char *name);

/* Remove the user's crontab and queued at(1) jobs. Absent is success. */
int		 acct_remove_cron(const char *name);
int		 acct_remove_at(const char *name);

/*
 * Wipe a buffer that held a password. explicit_bzero(3) where the platform
 * has it; a volatile-pointer memset otherwise, so the host build for the
 * tests still compiles and still actually clears.
 */
void		 acct_zero(void *buf, size_t len);

#endif /* ACCT_H */
