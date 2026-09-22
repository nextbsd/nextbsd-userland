/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
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
 * Bits the account tools (passwd, chpass, pw) share: prompting for a
 * password with or without a terminal, and the "joined client" refusal.
 * Compiled into each program alongside libds (see acct.mk).
 */

#ifndef NEXTBSD_ACCT_H
#define NEXTBSD_ACCT_H

#include <stdbool.h>

/*
 * Read a password. On a terminal, prompt with echo off (and ask twice
 * when confirm is set); otherwise read one line from stdin, as scripts
 * expect. Returns malloc'd text, or NULL (message printed) on failure.
 */
char	*acct_read_password(const char *prog, const char *what, bool confirm);

/* Overwrite and free a password. */
void	acct_wipe(char *s);

/*
 * Edits are refused on a joined client: the database in use is the
 * server's read-only mount. Prints the reason and returns true.
 */
bool	acct_refuse_if_joined(const char *prog);

#endif /* NEXTBSD_ACCT_H */
