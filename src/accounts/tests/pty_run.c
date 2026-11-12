/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * pty_run — run a command with a controlling terminal and feed it lines.
 *
 * Why this exists. passwd(1) and chpass(1) read secrets with
 * readpassphrase(3) and RPP_REQUIRE_TTY, which opens /dev/tty. That is
 * correct: a password prompt must not be satisfiable by redirecting stdin.
 * It also means no shell script can drive them, so the on-image suite had no
 * way to test a password change end to end, which is the single most
 * important thing about these tools.
 *
 * So: allocate a pty, run the command on it, and write the supplied lines as
 * it asks for them. About what expect would do, without needing expect on
 * the image.
 *
 *   pty_run [-t secs] -i line [-i line ...] -- command [args ...]
 *
 * Every -i line is sent in order, each with a newline, and each only once
 * the child is at a prompt: it has printed output since the last line that
 * does not end in a newline, and the terminal has echo turned off. The
 * child's output goes to stdout so the caller can grep it, and the exit
 * status is the child's.
 *
 * Why wait for the prompt rather than for a pause. readpassphrase(3), and
 * getpass(3) on top of it, switches echo off with TCSAFLUSH before it prints
 * the prompt, and that discards anything already typed. A line written
 * before that point is thrown away, the next line answers the wrong
 * question, and the last prompt waits until -t runs out. The first version
 * of this tool sent a line 200ms after any output at all, which includes
 * passwd's "Changing local password for ..." banner, so whether the answer
 * survived depended on scheduling. On one CPU the child usually reached the
 * flush first. With two it often did not, and amd64 ACCT-LIFECYCLE failed
 * (nextbsd-userland#631).
 *
 * A test-only tool. It is installed with the on-image suite, not in a
 * shipped path.
 */

#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
/* forkpty(3): <libutil.h> on FreeBSD, <util.h> on Darwin. */
#ifdef __FreeBSD__
#include <libutil.h>
#else
#include <util.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINES 16

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: pty_run [-t secs] -i line [-i line ...] -- command [args ...]\n");
	exit(2);
}

int
main(int argc, char *argv[])
{
	const char *lines[MAX_LINES];
	int nlines = 0, ch, timeout = 30;
	int master, status;
	pid_t pid;
	fd_set rfds;
	struct timeval tv;
	char buf[4096];
	ssize_t n;
	int sent = 0;
	bool child_gone = false;
	bool partial = false;	/* output since the last line, no newline yet */
	struct termios t;
	time_t deadline;

	while ((ch = getopt(argc, argv, "i:t:")) != -1) {
		switch (ch) {
		case 'i':
			if (nlines >= MAX_LINES)
				errx(2, "at most %d input lines", MAX_LINES);
			lines[nlines++] = optarg;
			break;
		case 't':
			timeout = (int)strtol(optarg, NULL, 10);
			if (timeout <= 0)
				usage();
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		usage();

	if ((pid = forkpty(&master, NULL, NULL, NULL)) == -1)
		err(2, "forkpty");
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}

	/*
	 * Read whatever the child says and answer each prompt in order. A
	 * prompt is output that stops mid-line while the terminal does not
	 * echo: readpassphrase(3) prints its prompt only after it has turned
	 * echo off and flushed the input, so a line written then is the one it
	 * reads. Banners end in a newline and do not count, and neither does
	 * the newline readpassphrase prints after an answer, so one prompt gets
	 * one line. If the terminal's modes cannot be read, the mid-line test
	 * alone decides.
	 */
	deadline = time(NULL) + timeout;
	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(master, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		if (select(master + 1, &rfds, NULL, NULL, &tv) > 0) {
			n = read(master, buf, sizeof(buf));
			if (n <= 0) {
				child_gone = true;
			} else {
				(void)fwrite(buf, 1, (size_t)n, stdout);
				(void)fflush(stdout);
				partial = buf[n - 1] != '\n' &&
				    buf[n - 1] != '\r';
			}
		}
		if (child_gone)
			break;
		if (sent < nlines && partial &&
		    (tcgetattr(master, &t) == -1 || (t.c_lflag & ECHO) == 0)) {
			(void)write(master, lines[sent], strlen(lines[sent]));
			(void)write(master, "\n", 1);
			sent++;
			partial = false;
			continue;
		}
		if (time(NULL) >= deadline) {
			(void)fprintf(stderr, "pty_run: timed out\n");
			(void)kill(pid, SIGKILL);
			break;
		}
	}
	(void)close(master);
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR)
			err(2, "waitpid");
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (2);
}
