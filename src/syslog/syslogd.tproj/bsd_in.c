/*
 * bsd_in.c — FreeBSD-only input module for Apple syslogd.
 *
 * Apple's syslogd never had a UNIX-socket input module: macOS apps
 * use the ASL client library (libsystem_asl) which sends messages
 * over Mach IPC. FreeBSD's existing userland (every program that
 * calls libc's syslog(3)) writes RFC3164 datagrams to /var/run/log
 * (world-writable) and /var/run/logpriv (root-only).
 *
 * This module takes those two sockets and feeds each datagram into
 * asl_input_parse(), the same entry point klog_in / udp_in use.
 * That makes Apple's syslogd a drop-in replacement for the stock
 * /usr/sbin/syslogd for plain BSD syslog(3) clients.
 *
 * launchd owns the sockets (nextbsd-userland#494), the way Darwin's
 * syslogd gets /var/run/syslog: the job plist declares them under
 * Sockets (BSDSystemLogger, BSDSystemLoggerPriv), launchctl binds them
 * when the job is loaded, and this module adopts the descriptors from
 * the launchd check-in reply (global.launch_dict), as udp_in does for
 * NetworkListener. The paths therefore exist from the moment the
 * LaunchDaemons are loaded, and anything logged before syslogd is
 * running queues in the socket instead of failing. That matters for
 * LOG_CONS callers such as login(1): a failed send makes libc write
 * the line to /dev/console. Binding the sockets here is kept only as
 * a fallback for when the check-in carries no descriptor.
 *
 * Phase J3 of the ASL port: pkgdemon.github.io/freebsd-asl-plan.html
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "daemon.h"

#define MY_ID		"bsd_in"
#define BSD_SOCK_PATH	"/var/run/log"
#define BSD_PRIV_PATH	"/var/run/logpriv"
#define MAXLINE		4096

#define NSOCK 2
static int sockfd[NSOCK] = { -1, -1 };
static dispatch_source_t in_src[NSOCK];
static dispatch_queue_t in_queue;
static const char *sock_path[NSOCK] = { BSD_SOCK_PATH, BSD_PRIV_PATH };
/* launchd Sockets keys in the job plist, one per path above. */
static const char *sock_name[NSOCK] = { "BSDSystemLogger", "BSDSystemLoggerPriv" };
/* Set when sockfd[i] came from launchd: launchd owns the path, so
 * bsd_in_close() must not unlink it. */
static int sock_from_launchd[NSOCK];

static char dline[MAXLINE + 1];

/* Polling-thread fallback. libdispatch's DISPATCH_SOURCE_TYPE_READ
 * doesn't wake up for AF_UNIX SOCK_DGRAM message arrivals on
 * FreeBSD in our build — likely a kqueue/EVFILT_READ interaction
 * with our libdispatch port. Use a real pthread with poll(2) on
 * both fds, just like phase_e_libdispatch_mach_recv did for Mach
 * RECV sources. Replace once dispatch's source firing is fixed. */
#include <pthread.h>
#include <poll.h>
static pthread_t bsd_in_poll_thread;
static int bsd_in_poll_started;

static void
bsd_in_acceptmsg(int fd)
{
	struct sockaddr_un from;
	socklen_t fromlen = sizeof(from);
	ssize_t len;
	asl_msg_t *m;

	/* Phase J runtime debug: log every dispatch fire, BEFORE recvfrom. */
	{ FILE *_d0 = _syslogd_trace_open("/tmp/bsd_in_recv.log");
	  if (_d0) { fprintf(_d0, "[%d] dispatch fired on fd=%d\n", getpid(), fd); fclose(_d0); } }

	len = recvfrom(fd, dline, MAXLINE, 0, (struct sockaddr *)&from, &fromlen);
	{ FILE *_d0 = _syslogd_trace_open("/tmp/bsd_in_recv.log");
	  if (_d0) { fprintf(_d0, "[%d]   recvfrom rc=%zd errno=%d\n", getpid(), len, errno); fclose(_d0); } }
	if (len <= 0) return;
	dline[len] = '\0';

	/* trim trailing newline if present (BSD syslog(3) appends '\n') */
	while (len > 0 && (dline[len - 1] == '\n' || dline[len - 1] == '\0')) {
		dline[--len] = '\0';
	}
	if (len == 0) return;

	/* Phase J runtime debug. */
	FILE *_d = _syslogd_trace_open("/tmp/bsd_in_recv.log");
	if (_d) { fprintf(_d, "[%d] recv fd=%d len=%zd: %.120s\n", getpid(), fd, len, dline); fclose(_d); }

	m = asl_input_parse(dline, len, NULL, SOURCE_BSD_SOCK);
	_d = _syslogd_trace_open("/tmp/bsd_in_recv.log");
	if (_d) { fprintf(_d, "[%d]   asl_input_parse -> %p\n", getpid(), (void*)m); fclose(_d); }
	process_message(m, SOURCE_BSD_SOCK);
	_d = _syslogd_trace_open("/tmp/bsd_in_recv.log");
	if (_d) { fprintf(_d, "[%d]   process_message OK\n", getpid()); fclose(_d); }
}

/*
 * Adopt the launchd-created socket for sock_name[idx] from the check-in
 * reply. Returns 0 and sets sockfd[idx] on success, -1 if launchd did
 * not hand one over (no check-in, or no such Sockets entry).
 */
static int
bsd_in_adopt_one(int idx)
{
	launch_data_t sockets_dict, fd_array, fd_dict;
	int fd, fl;

	if (global.launch_dict == NULL)
	{
		asldebug("%s: launchd dict is NULL\n", MY_ID);
		return -1;
	}

	sockets_dict = launch_data_dict_lookup(global.launch_dict, LAUNCH_JOBKEY_SOCKETS);
	if (sockets_dict == NULL)
	{
		asldebug("%s: launchd lookup of LAUNCH_JOBKEY_SOCKETS failed\n", MY_ID);
		return -1;
	}

	fd_array = launch_data_dict_lookup(sockets_dict, sock_name[idx]);
	if (fd_array == NULL)
	{
		asldebug("%s: launchd lookup of %s failed\n", MY_ID, sock_name[idx]);
		return -1;
	}

	fd_dict = fd_array;
	if (launch_data_get_type(fd_array) == LAUNCH_DATA_ARRAY)
	{
		if (launch_data_array_get_count(fd_array) == 0)
		{
			asldebug("%s: launchd fd array for %s is empty\n", MY_ID, sock_name[idx]);
			return -1;
		}
		fd_dict = launch_data_array_get_index(fd_array, 0);
	}

	if (fd_dict == NULL || launch_data_get_type(fd_dict) != LAUNCH_DATA_FD)
	{
		asldebug("%s: launchd entry for %s is not a descriptor\n", MY_ID, sock_name[idx]);
		return -1;
	}

	fd = launch_data_get_fd(fd_dict);
	if (fd < 0)
	{
		asldebug("%s: launchd descriptor for %s is invalid\n", MY_ID, sock_name[idx]);
		return -1;
	}

	/* The recv-loop thread uses blocking recvfrom(). */
	fl = fcntl(fd, F_GETFL, 0);
	if (fl != -1) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

	/* Ours now: a later re-init must not pick up a stale number. */
	launch_data_set_fd(fd_dict, -1);

	sockfd[idx] = fd;
	sock_from_launchd[idx] = 1;
	asldebug("%s: adopted launchd socket %s for %s (fd %d)\n", MY_ID,
	    sock_name[idx], sock_path[idx], fd);
	return 0;
}

static int
bsd_in_open_one(int idx)
{
	struct sockaddr_un sun;
	int fd;
	mode_t perm;

	(void)unlink(sock_path[idx]);

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		asldebug("%s: socket(%s): %s\n", MY_ID, sock_path[idx],
		    strerror(errno));
		return -1;
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, sock_path[idx], sizeof(sun.sun_path));

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		asldebug("%s: bind(%s): %s\n", MY_ID, sock_path[idx],
		    strerror(errno));
		close(fd);
		return -1;
	}

	/* /var/run/log world-writable, /var/run/logpriv root-only */
	perm = (idx == 0) ? 0666 : 0600;
	(void)chmod(sock_path[idx], perm);

	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
		asldebug("%s: O_NONBLOCK(%s): %s\n", MY_ID, sock_path[idx],
		    strerror(errno));
		close(fd);
		return -1;
	}

	sockfd[idx] = fd;
	/* Make socket BLOCKING for the recv-loop thread (recvfrom blocks). */
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl != -1) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	asldebug("%s: listening on %s (fd %d)\n", MY_ID, sock_path[idx], fd);
	return 0;
}

/* One blocking-recvfrom thread per socket. poll(2) seemed to vanish
 * the process inside libdispatch's thread management; blocking
 * recvfrom on a single fd is a simpler syscall and one thread per
 * fd matches what older daemons do anyway. */
struct bsd_in_thread_arg {
	int idx;
};

static void *
bsd_in_recv_loop(void *arg)
{
	int idx = ((struct bsd_in_thread_arg *)arg)->idx;
	free(arg);

	{ FILE *_d = _syslogd_trace_open("/tmp/bsd_in_recv.log");
	  if (_d) { fprintf(_d, "[%d] recv thread RUNNING idx=%d fd=%d\n",
	    getpid(), idx, sockfd[idx]); fclose(_d); } }

	for (;;) {
		int fd = sockfd[idx];
		if (fd < 0) { usleep(100000); continue; }
		bsd_in_acceptmsg(fd);
	}
	return NULL;
}

int
bsd_in_init(void)
{
	static dispatch_once_t once;
	int i, ok = 0;
	FILE *dbg;

	dbg = _syslogd_trace_open("/tmp/bsd_in_init.log");
	if (dbg) { fprintf(dbg, "[%d] bsd_in_init: entered\n", getpid()); fclose(dbg); }

	dispatch_once(&once, ^{
		in_queue = dispatch_queue_create(MY_ID, NULL);
	});

	asldebug("%s: init\n", MY_ID);

	for (i = 0; i < NSOCK; i++) {
		if (sockfd[i] >= 0) { ok++; continue; }
		int rc = bsd_in_adopt_one(i);
		if (rc != 0) rc = bsd_in_open_one(i);
		dbg = _syslogd_trace_open("/tmp/bsd_in_init.log");
		if (dbg) {
			fprintf(dbg, "[%d] bsd_in_init: open(%s) rc=%d errno=%d (%s)\n",
			    getpid(), sock_path[i], rc, errno, strerror(errno));
			fclose(dbg);
		}
		if (rc == 0) ok++;
	}

	if (ok > 0 && !bsd_in_poll_started) {
		for (int j = 0; j < NSOCK; j++) {
			if (sockfd[j] < 0) continue;
			struct bsd_in_thread_arg *a = malloc(sizeof(*a));
			a->idx = j;
			pthread_t t;
			pthread_create(&t, NULL, bsd_in_recv_loop, a);
			pthread_detach(t);
		}
		bsd_in_poll_started = 1;
		dbg = _syslogd_trace_open("/tmp/bsd_in_init.log");
		if (dbg) { fprintf(dbg, "[%d] bsd_in_init: recv threads launched\n", getpid()); fclose(dbg); }
	}

	dbg = _syslogd_trace_open("/tmp/bsd_in_init.log");
	if (dbg) { fprintf(dbg, "[%d] bsd_in_init: returning ok=%d\n", getpid(), ok); fclose(dbg); }

	return (ok > 0) ? 0 : -1;
}

int
bsd_in_close(void)
{
	int i, did = 0;

	for (i = 0; i < NSOCK; i++) {
		if (sockfd[i] < 0) continue;
		close(sockfd[i]);
		/* A launchd-owned path stays; launchd still holds the socket. */
		if (!sock_from_launchd[i]) (void)unlink(sock_path[i]);
		sock_from_launchd[i] = 0;
		sockfd[i] = -1;
		did++;
	}
	/* Polling thread keeps running across reset; only stops when
	 * syslogd exits. Detaching avoids the cancel/release dance. */
	return (did > 0) ? 0 : 1;
}

int
bsd_in_reset(void)
{
	return 0;
}
