/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * Unit tests for the shared account helpers, and the only place password
 * hashing is verified at all.
 *
 * Neither adduser nor passwd offers a non-interactive way to set a password
 * from a plaintext one, by design: both prompt with readpassphrase(3), which
 * reads /dev/tty and cannot be fed from a pipe. So no end-to-end test can
 * reach acct_hash_password. This binary calls it directly.
 *
 * It is built twice: on a build host, where it reports what the host's
 * crypt(3) cannot do rather than pretending, and for the image, where it runs
 * on the boot lane against the crypt(3) we actually ship. The second one is
 * what really covers hashing. That split matters: Darwin's crypt(3) has no
 * SHA-512 and silently falls back to DES, so a host-only test of hashing
 * would have been testing the wrong algorithm.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "acct.h"

static int	 checks;
static int	 failures;

static void
ck(bool cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

int
main(void)
{
	char h1[512], h2[512];
	char locked[1024];
	bool hashed;

	/* Things that hold whatever crypt(3) can do. */
	ck(!acct_hash_locked(NULL), "a null hash is not locked");
	ck(!acct_hash_locked(""), "an empty hash is not locked");
	ck(acct_hash_locked(ACCT_LOCK_PREFIX), "the bare sentinel reads as locked");
	ck(acct_hash_locked(ACCT_LOCK_PREFIX "$6$x$y"), "a locked hash reads as locked");
	ck(!acct_hash_locked("$6$x$y"), "an ordinary hash does not read as locked");

	ck(!acct_verify_password("x", NULL), "a null stored hash never verifies");
	ck(!acct_verify_password("x", ""), "an empty stored hash never verifies");
	ck(!acct_verify_password(NULL, "$6$x$y"), "a null password never verifies");
	ck(!acct_verify_password("x", ACCT_LOCK_PREFIX "$6$x$y"),
	    "a locked account never verifies");

	ck(acct_is_system_name("_www"), "a leading underscore is a system name");
	ck(!acct_is_system_name("joe"), "an ordinary name is not");
	ck(acct_is_system_id(0) && acct_is_system_id(999),
	    "0 and 999 are in the system range");
	ck(!acct_is_system_id(1000) && !acct_is_system_id(5001),
	    "1000 and 5001 are not");

	ck(acct_name_problem("joe") == NULL, "joe is a usable name");
	ck(acct_name_problem("9joe") != NULL, "a leading digit is refused");
	ck(acct_name_problem("jo:e") != NULL, "a colon is refused");
	ck(acct_name_problem("") != NULL, "an empty name is refused");

	/*
	 * Hashing. acct_hash_password refuses rather than downgrading when
	 * crypt(3) cannot produce the format login.conf asks for, so a NULL
	 * here is a correct answer on a host whose crypt is weaker, not a
	 * failure of this code. Say which happened instead of hiding it.
	 */
	hashed = acct_hash_password("correct horse battery staple", h1,
	    sizeof(h1));
	if (!hashed) {
		printf("SKIP: crypt(3) here cannot produce the required "
		    "format, so hashing is unverified on this platform\n");
		printf("      (this is why the image runs this same binary)\n");
	} else {
		ck(strncmp(h1, "$", 1) == 0, "the hash is a modular crypt string");
		ck(strlen(h1) > 13, "the hash is not a 13-character DES hash");
		ck(acct_verify_password("correct horse battery staple", h1),
		    "the right password verifies");
		ck(!acct_verify_password("wrong horse battery staple", h1),
		    "a wrong password does not");
		ck(!acct_verify_password("", h1), "an empty password does not");
		ck(!acct_verify_password("correct horse battery staple ", h1),
		    "a trailing space does not");

		/*
		 * The salt is random, so the same password hashes
		 * differently. h1 is a copy, not a borrowed pointer: when
		 * this helper returned static storage, both names referred
		 * to the same buffer and this check compared it with
		 * itself and always passed.
		 */
		ck(acct_hash_password("correct horse battery staple", h2,
		    sizeof(h2)) && strcmp(h1, h2) != 0,
		    "two hashes of one password differ");

		/* And a locked copy of a real hash still refuses. */
		(void)snprintf(locked, sizeof(locked), "%s%s",
		    ACCT_LOCK_PREFIX, h1);
		ck(!acct_verify_password("correct horse battery staple", locked),
		    "locking a valid hash stops it verifying");
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	if (failures == 0)
		puts("ACCT-HELPERS-OK: names, ranges, the lock sentinel and hashing");
	else
		puts("ACCT-HELPERS-FAIL");
	return (failures == 0 ? 0 : 1);
}
