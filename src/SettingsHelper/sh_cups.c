/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_cups.c — CUPSAdmin: Printers.app's operation (nextbsd/nextbsd#640).
 *
 *   add      lpadmin -p <printer> -E -v <uri> -m everywhere -D <info>
 *            -L <location> -o printer-is-shared=false
 *   delete   lpadmin -x <printer>
 *
 * CUPS asks for an administrator for both (cupsd.conf's CUPS-Add-Modify-
 * Printer and CUPS-Delete-Printer limits, SystemGroup admin _lpadmin);
 * root passes. lpadmin rather than IPP from here: its "-m everywhere"
 * asks the printer for its capabilities and makes the PPD, and resolves
 * a DNS-SD service name in the URI first, which is most of the work.
 * It also keeps libcups out of the helper: printing is an optional
 * package (nextbsd-printing-support), and without it there's no
 * lpadmin and the operation is SH_ERR_UNSUPPORTED.
 *
 * Only driverless printers for now: the filters and drivers the other
 * models need aren't in nextbsd-printing-support yet.
 *
 * lpadmin is run with fork and exec, never a shell, and every argument
 * is checked first. "-m everywhere" talks to the printer, so it's given
 * LPADMIN_LIMIT seconds; its messages go to the log.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	LPADMIN		"/usr/sbin/lpadmin"
#define	LPADMIN_LIMIT	90		/* seconds */

/*
 * A queue name as cupsd takes one (scheduler/ipp.c validate_name): no
 * spaces or control characters, none of / \ ? ' " #, at most 127 bytes.
 * And not starting with '-', so it can't read as an option.
 */
static bool
printer_ok(const char *name)
{
	const unsigned char *p;

	if (name[0] == '\0' || name[0] == '-' ||
	    strlen(name) > SH_PRINTER_MAX)
		return (false);
	for (p = (const unsigned char *)name; *p != '\0'; p++)
		if (*p <= ' ' || *p == 127 || strchr("/\\?'\"#", *p) != NULL)
			return (false);
	return (true);
}

/*
 * A device URI for a driverless queue: ipp, ipps or dnssd, then
 * printable ASCII without spaces (a URI's other characters are
 * %-encoded).
 */
static bool
uri_ok(const char *uri)
{
	const char *p;

	if (strncmp(uri, "ipp://", 6) != 0 && strncmp(uri, "ipps://", 7) != 0 &&
	    strncmp(uri, "dnssd://", 8) != 0)
		return (false);
	if (strlen(uri) > SH_DEVICE_URI_MAX || strchr(uri, '/')[2] == '\0')
		return (false);			/* no host */
	for (p = uri; *p != '\0'; p++)
		if (*p <= ' ' || *p >= 127)
			return (false);
	return (true);
}

/* Free text (UTF-8 is fine) without control characters. */
static bool
text_ok(const char *text)
{
	const unsigned char *p;

	if (strlen(text) > SH_PRINTER_TEXT_MAX)
		return (false);
	for (p = (const unsigned char *)text; *p != '\0'; p++)
		if (*p < ' ' || *p == 127)
			return (false);
	return (true);
}

/*
 * Run lpadmin with these arguments (NULL-terminated, argv[0] included);
 * true if it exited 0 within LPADMIN_LIMIT. What it says goes to the log.
 */
static bool
lpadmin(const char *const argv[])
{
	char out[512];
	size_t used = 0;
	struct pollfd pfd;
	time_t deadline;
	pid_t pid;
	ssize_t n;
	int fds[2], st = 0;
	bool done = false;

	if (pipe2(fds, O_CLOEXEC) == -1)
		return (false);
	pid = fork();
	if (pid == -1) {
		(void)close(fds[0]);
		(void)close(fds[1]);
		return (false);
	}
	if (pid == 0) {
		int fd = open("/dev/null", O_RDONLY);

		if (fd != -1)
			(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fds[1], STDOUT_FILENO);
		(void)dup2(fds[1], STDERR_FILENO);
		execv(LPADMIN, (char *const *)argv);
		_exit(127);
	}
	(void)close(fds[1]);

	/* Read what it says while waiting, so it never blocks on a full
	 * pipe; give up on it after the limit. */
	deadline = sh_now() + LPADMIN_LIMIT;
	pfd.fd = fds[0];
	pfd.events = POLLIN;
	while (!done) {
		if (poll(&pfd, 1, 250) > 0) {
			char discard[256];

			if (used < sizeof(out) - 1)
				n = read(fds[0], out + used, sizeof(out) - 1 - used);
			else
				n = read(fds[0], discard, sizeof(discard));
			if (n > 0 && used < sizeof(out) - 1)
				used += (size_t)n;
		}
		switch (waitpid(pid, &st, WNOHANG)) {
		case -1:
			if (errno == EINTR)
				break;
			st = -1;
			done = true;
			break;
		case 0:
			if (sh_now() > deadline) {
				sh_log("lpadmin: no answer in %d s; stopped",
				    LPADMIN_LIMIT);
				(void)kill(pid, SIGKILL);
				(void)waitpid(pid, &st, 0);
				st = -1;
				done = true;
			}
			break;
		default:
			done = true;
		}
	}
	(void)close(fds[0]);
	out[used] = '\0';
	if (used > 0 && out[used - 1] == '\n')
		out[used - 1] = '\0';
	if (out[0] != '\0')
		sh_log("lpadmin: %s", out);
	return (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int
sh_printer_admin(int op, const char *printer, const char *uri, const char *info,
    const char *location)
{
	struct stat st;

	if (!printer_ok(printer))
		return (SH_ERR_INVALID);
	if (stat(LPADMIN, &st) == -1)
		return (SH_ERR_UNSUPPORTED);	/* no printing package */

	switch (op) {
	case SH_CUPS_ADD: {
		const char *argv[] = { LPADMIN, "-p", printer, "-E", "-v", uri,
		    "-m", "everywhere", "-D", info, "-L", location,
		    "-o", "printer-is-shared=false", NULL };

		if (!uri_ok(uri) || !text_ok(info) || !text_ok(location))
			return (SH_ERR_INVALID);
		return (lpadmin(argv) ? SH_OK : SH_ERR_FAILED);
	}
	case SH_CUPS_DELETE: {
		const char *argv[] = { LPADMIN, "-x", printer, NULL };

		return (lpadmin(argv) ? SH_OK : SH_ERR_FAILED);
	}
	default:
		return (SH_ERR_INVALID);
	}
}
