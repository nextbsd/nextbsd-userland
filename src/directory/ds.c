/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * dspromote, dsdemote, dsjoin, dsleave, dsstatus -- make this machine a
 * directory server, or bind it to one (nextbsd/nextbsd-userland#253, E18 U7).
 *
 * One binary behind five names, dispatching on argv[0], the way chpass(1)
 * carries chfn and chsh. The verbs do not overlap, so there is no shared
 * option parsing worth abstracting; what they share is the set of artefacts
 * below, and keeping them in one file is what keeps promote and demote
 * symmetrical.
 *
 * Local account management is NOT here. adduser(8), pw(8), passwd(1) and
 * chpass(1) write the property lists directly, and Gershwin's dscli manages
 * the same files. This program only moves a machine between three roles.
 *
 *   standalone   accounts in /Local, nothing exported, nothing mounted
 *   server       serves the accounts and the homes over NFS, advertised
 *                over Bonjour as _nextbsd-ds._tcp
 *   client       reads the server's accounts and mounts its homes
 *
 * WHERE THE AUTHORITATIVE PLISTS LIVE
 *
 * In /Local/Library/DirectoryServices, on every machine, in every role. A
 * server shares that directory. It does not move it.
 *
 * nss_directory_services prefers /Network/Library/DirectoryServices when that
 * exists, and computes a home from whichever file it read. That is precisely
 * what lets one layout serve all three roles with no rearranging:
 *
 *   server   has no /Network, so it reads /Local and its homes are under
 *            /Local/Users -- correct, because those homes really are local
 *   client   mounts the server's /Local paths onto its own /Network, reads
 *            /Network, and its homes are under /Network/Users, which is the
 *            mount of the server's /Local/Users
 *
 * An earlier version moved the plists to /Network on promotion and left
 * /Local behind as a symlink to them, reasoning that a server "has to read
 * the very file it serves". That does not follow: an NFS export path and the
 * client's mount point are independent, so a client can mount
 * server:/Local/Library/DirectoryServices at its own /Network path and
 * nothing needs to move. Dropping the move also drops the /Network/Users
 * symlink, the EXDEV-safe copy loop, and a layout that had to be documented
 * in the wiki because it surprised everyone who looked at it. It is also
 * truer to Darwin, where /Network means resources mounted from somewhere
 * else -- which a server's own accounts are not.
 *
 * WHAT MAKES A MACHINE A SERVER
 *
 * Role.plist, written by promotion and removed by demotion. The previous
 * design read the role off the /Local symlink it had just created, which
 * worked but used a side effect as state. A file that says `server` survives
 * a reboot, an unmounted /Network and a stopped nfsd just as well, and
 * anything else that needs to know can read it without an lstat(2) and a
 * paragraph of explanation.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Every path is overridable so the tests can stage a whole fake root. Nothing
 * in the shipped build defines any of them.
 */
#ifndef nitems
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

#ifndef DS_LOCAL_DIR
#define DS_LOCAL_DIR	"/Local/Library/DirectoryServices"
#endif
#ifndef DS_NETWORK_DIR
#define DS_NETWORK_DIR	"/Network/Library/DirectoryServices"
#endif
#ifndef DS_LOCAL_USERS
#define DS_LOCAL_USERS	"/Local/Users"
#endif
#ifndef DS_NETWORK_USERS
#define DS_NETWORK_USERS "/Network/Users"
#endif
#ifndef DS_BINDING
#define DS_BINDING	"/Local/Library/DirectoryServices/Binding.plist"
#endif
#ifndef DS_ROLE
#define DS_ROLE		"/Local/Library/DirectoryServices/Role.plist"
#endif
#ifndef DS_EXPORTS
#define DS_EXPORTS	"/etc/exports"
#endif
#ifndef DS_NTP_CONF
#define DS_NTP_CONF	"/etc/ntp.conf"
#endif
#ifndef DS_SERVICE_DIR
#define DS_SERVICE_DIR	"/Local/Library/Preferences/mDNSResponder/Services"
#endif
#ifndef DS_LAUNCHCTL
#define DS_LAUNCHCTL	"/bin/launchctl"
#endif

#define DS_SERVICE_FILE	DS_SERVICE_DIR "/org.nextbsd.directory.plist"
#define DS_SERVICE_TYPE	"_nextbsd-ds._tcp"
#define DS_SERVICE_PORT	2049		/* nfsd; what a client actually needs */

/*
 * The managed block in /etc/ntp.conf. This text is a contract with the file
 * nextbsd-overlays ships, which already carries both markers with an empty
 * body -- so dsjoin fills a block that is there rather than appending one.
 */
#define NTP_BEGIN "# BEGIN directory server (managed by dsjoin and dsleave; do not edit)"
#define NTP_END   "# END directory server"

/* The jobs each role turns on. Order matters: rpcbind before the rest. */
static const char *const server_jobs[] = {
	"org.nextbsd.rpcbind", "org.nextbsd.mountd", "org.nextbsd.nfsd"
};
static const char *const client_jobs[] = { "org.nextbsd.network-mount" };

enum mode { M_PROMOTE, M_DEMOTE, M_JOIN, M_LEAVE, M_STATUS };

static bool verbose;

static void
say(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	(void)vfprintf(stdout, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stdout);
}

/* ---------------------------------------------------------------- helpers */

static bool
is_dir(const char *p)
{
	struct stat st;

	return (stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}

static bool
exists(const char *p)
{
	struct stat st;

	return (lstat(p, &st) == 0);
}

/* mkdir -p, for a path we are about to own. */
static int
make_dirs(const char *path)
{
	char buf[PATH_MAX], *p;

	if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) == -1 && errno != EEXIST)
			return (-1);
		*p = '/';
	}
	return (mkdir(buf, 0755) == -1 && errno != EEXIST ? -1 : 0);
}

/*
 * Run launchctl. Reported but never fatal: a job that will not load leaves
 * the machine in a role it can be talked out of again, which is better than
 * aborting half way through with the artefacts already written.
 */
static void
launchctl(const char *verb, const char *const *labels, size_t n)
{
	char path[PATH_MAX];
	size_t i;
	pid_t pid;
	int st;

	for (i = 0; i < n; i++) {
		(void)snprintf(path, sizeof(path),
		    "/System/Library/LaunchDaemons/%s.plist", labels[i]);
		if (!exists(path)) {
			warnx("%s is not installed; skipping", labels[i]);
			continue;
		}
		if ((pid = fork()) == -1) {
			warn("fork");
			return;
		}
		if (pid == 0) {
			(void)execl(DS_LAUNCHCTL, "launchctl", verb, "-w",
			    path, (char *)NULL);
			_exit(127);
		}
		if (waitpid(pid, &st, 0) == -1 || !WIFEXITED(st) ||
		    WEXITSTATUS(st) != 0)
			warnx("launchctl %s -w %s did not succeed", verb,
			    labels[i]);
		else
			say("  launchctl %s %s", verb, labels[i]);
	}
}

/* ------------------------------------------------------------- the exports */

/*
 * The kernel permits one default export line per filesystem, so directories
 * that share a device have to share a line. On the usual single-filesystem
 * layout that is one line naming both paths; on a machine where /Local is its
 * own filesystem it is two.
 */
static int
write_exports(void)
{
	struct stat a, b;
	FILE *f;

	if (stat(DS_LOCAL_USERS, &a) == -1) {
		warn("%s", DS_LOCAL_USERS);
		return (-1);
	}
	if (stat(DS_LOCAL_DIR, &b) == -1) {
		warn("%s", DS_LOCAL_DIR);
		return (-1);
	}
	if ((f = fopen(DS_EXPORTS, "w")) == NULL) {
		warn("%s", DS_EXPORTS);
		return (-1);
	}
	(void)fprintf(f, "# Written by dspromote(8). Removed by dsdemote(8).\n");
	if (a.st_dev == b.st_dev)
		(void)fprintf(f, "%s %s -alldirs\n", DS_LOCAL_USERS,
		    DS_LOCAL_DIR);
	else {
		(void)fprintf(f, "%s -alldirs\n", DS_LOCAL_USERS);
		(void)fprintf(f, "%s -alldirs\n", DS_LOCAL_DIR);
	}
	if (fclose(f) != 0) {
		warn("%s", DS_EXPORTS);
		return (-1);
	}
	say("  wrote %s", DS_EXPORTS);
	return (0);
}

/* ------------------------------------------------------- the Bonjour record */

static int
write_service(void)
{
	char host[MAXHOSTNAMELEN];
	FILE *f;

	if (gethostname(host, sizeof(host)) == -1) {
		warn("gethostname");
		return (-1);
	}
	if (make_dirs(DS_SERVICE_DIR) == -1) {
		warn("%s", DS_SERVICE_DIR);
		return (-1);
	}
	if ((f = fopen(DS_SERVICE_FILE, "w")) == NULL) {
		warn("%s", DS_SERVICE_FILE);
		return (-1);
	}
	(void)fprintf(f,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict>\n"
	    "\t<key>Name</key>\n\t<string>%s</string>\n"
	    "\t<key>Type</key>\n\t<string>%s</string>\n"
	    "\t<key>Port</key>\n\t<integer>%d</integer>\n"
	    "</dict>\n"
	    "</plist>\n", host, DS_SERVICE_TYPE, DS_SERVICE_PORT);
	if (fclose(f) != 0) {
		warn("%s", DS_SERVICE_FILE);
		return (-1);
	}
	say("  advertised %s as %s", DS_SERVICE_TYPE, host);
	return (0);
}

/* ------------------------------------------------------ the NTP time source */

/*
 * Fill or empty the managed block in /etc/ntp.conf. The block's markers are
 * shipped by nextbsd-overlays with nothing between them, so the common case
 * is rewriting the body in place. A file that has lost its markers gets them
 * appended, and one we cannot read at all is reported rather than clobbered:
 * ntp.conf is the administrator's file and only this block is ours.
 */
static int
ntp_set(const char *server)
{
	char line[1024], tmp[PATH_MAX];
	FILE *in, *out;
	bool in_block = false, seen = false;
	int fd;

	if ((in = fopen(DS_NTP_CONF, "r")) == NULL) {
		warn("%s", DS_NTP_CONF);
		return (-1);
	}
	(void)snprintf(tmp, sizeof(tmp), "%s.ds.XXXXXX", DS_NTP_CONF);
	if ((fd = mkstemp(tmp)) == -1 || (out = fdopen(fd, "w")) == NULL) {
		warn("%s", tmp);
		(void)fclose(in);
		if (fd != -1) { (void)close(fd); (void)unlink(tmp); }
		return (-1);
	}
	while (fgets(line, sizeof(line), in) != NULL) {
		if (!in_block && strncmp(line, NTP_BEGIN, strlen(NTP_BEGIN)) == 0) {
			in_block = true; seen = true;
			(void)fputs(NTP_BEGIN "\n", out);
			if (server != NULL)
				(void)fprintf(out, "server %s iburst\n", server);
			continue;
		}
		if (in_block) {
			if (strncmp(line, NTP_END, strlen(NTP_END)) == 0) {
				in_block = false;
				(void)fputs(NTP_END "\n", out);
			}
			continue;	/* drop whatever the block held */
		}
		(void)fputs(line, out);
	}
	(void)fclose(in);
	if (!seen) {
		warnx("%s had no managed block; appending one", DS_NTP_CONF);
		(void)fputs("\n" NTP_BEGIN "\n", out);
		if (server != NULL)
			(void)fprintf(out, "server %s iburst\n", server);
		(void)fputs(NTP_END "\n", out);
	}
	if (fclose(out) != 0 || rename(tmp, DS_NTP_CONF) == -1) {
		warn("%s", DS_NTP_CONF);
		(void)unlink(tmp);
		return (-1);
	}
	say("  %s the time source in %s",
	    server != NULL ? "set" : "cleared", DS_NTP_CONF);
	return (0);
}

/* ---------------------------------------------------------- the binding file */

static int
binding_write(const char *server)
{
	FILE *f;

	if (make_dirs(DS_LOCAL_DIR) == -1) {
		warn("%s", DS_LOCAL_DIR);
		return (-1);
	}
	if ((f = fopen(DS_BINDING, "w")) == NULL) {
		warn("%s", DS_BINDING);
		return (-1);
	}
	(void)fprintf(f,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict>\n"
	    "\t<key>server</key>\n\t<string>%s</string>\n"
	    "\t<key>version</key>\n\t<integer>1</integer>\n"
	    "</dict>\n"
	    "</plist>\n", server);
	if (fclose(f) != 0) {
		warn("%s", DS_BINDING);
		return (-1);
	}
	say("  wrote %s", DS_BINDING);
	return (0);
}

/*
 * The role marker. Only two states are written, and only `server` is ever
 * recorded: standalone is the absence of the file, and a client is identified
 * by its Binding.plist instead.
 */
static int
role_write(void)
{
	FILE *f;

	if ((f = fopen(DS_ROLE, "w")) == NULL) {
		warn("%s", DS_ROLE);
		return (-1);
	}
	(void)fprintf(f,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict>\n"
	    "\t<key>role</key>\n\t<string>server</string>\n"
	    "\t<key>version</key>\n\t<integer>1</integer>\n"
	    "</dict>\n"
	    "</plist>\n");
	if (fclose(f) != 0) {
		warn("%s", DS_ROLE);
		return (-1);
	}
	say("  wrote %s", DS_ROLE);
	return (0);
}

/*
 * True when this machine has been promoted. Matched on the value rather than
 * merely on the file existing, so a truncated or half-written file reads as
 * "not a server" instead of stranding the machine in a role it cannot leave.
 */
static bool
role_is_server(void)
{
	char line[1024];
	bool found = false;
	FILE *f;

	if ((f = fopen(DS_ROLE, "r")) == NULL)
		return (false);
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, "<string>server</string>") != NULL) {
			found = true;
			break;
		}
	}
	(void)fclose(f);
	return (found);
}

/* The bound server's name, or false when this machine is not a client. */
static bool
binding_read(char *out, size_t len)
{
	char line[1024], *p, *q;
	FILE *f;
	bool in_key = false;

	if (out != NULL && len > 0)
		out[0] = '\0';
	if ((f = fopen(DS_BINDING, "r")) == NULL)
		return (false);
	while (fgets(line, sizeof(line), f) != NULL) {
		if (!in_key) {
			if ((p = strstr(line, "<key>server</key>")) == NULL)
				continue;
			in_key = true;
			p += sizeof("<key>server</key>") - 1;
		} else
			p = line;
		if ((p = strstr(p, "<string>")) == NULL)
			continue;
		p += sizeof("<string>") - 1;
		if ((q = strstr(p, "</string>")) == NULL)
			break;
		*q = '\0';
		if (out != NULL && len > 0)
			(void)strlcpy(out, p, len);
		break;
	}
	(void)fclose(f);
	return (true);
}

/* ------------------------------------------------------------- the domains */

enum role { R_STANDALONE, R_SERVER, R_CLIENT };

/*
 * Role.plist is what promotion writes and demotion removes, and it survives a
 * reboot, an unmounted /Network and a stopped nfsd -- none of which change
 * what this machine IS.
 */
static enum role
current_role(char *server, size_t len)
{
	if (role_is_server())
		return (R_SERVER);
	if (binding_read(server, len))
		return (R_CLIENT);
	return (R_STANDALONE);
}

static int
do_promote(void)
{
	char server[MAXHOSTNAMELEN];
	enum role r = current_role(server, sizeof(server));

	if (r == R_SERVER) {
		warnx("this machine is already a directory server");
		return (1);
	}
	if (r == R_CLIENT) {
		warnx("this machine is bound to %s; run dsleave first",
		    server[0] != '\0' ? server : "a directory server");
		return (1);
	}
	if (!is_dir(DS_LOCAL_DIR)) {
		warnx("%s is missing; there is nothing to serve", DS_LOCAL_DIR);
		return (1);
	}

	/*
	 * Nothing moves. The plists stay where every other tool already writes
	 * them, the homes stay in /Local/Users, and promotion is the act of
	 * sharing both -- so the server keeps reading exactly the files it
	 * serves, and its own homes stay local because they are.
	 */
	if (make_dirs(DS_LOCAL_USERS) == -1) {
		warn("%s", DS_LOCAL_USERS);
		return (1);
	}
	if (role_write() == -1)
		return (1);
	if (write_exports() == -1 || write_service() == -1)
		return (1);
	launchctl("load", server_jobs, nitems(server_jobs));

	(void)printf("This machine is now a directory server.\n");
	return (0);
}

static int
do_demote(void)
{
	char server[MAXHOSTNAMELEN];

	if (current_role(server, sizeof(server)) != R_SERVER) {
		warnx("this machine is not a directory server");
		return (1);
	}

	/*
	 * Stop serving first, so a client cannot be reading the export as it is
	 * withdrawn. Clients stay bound and will fail to mount -- there is no
	 * notification protocol, which is recorded as a known gap.
	 *
	 * There is nothing to move back: the plists never left /Local. Demotion
	 * only stops sharing them.
	 */
	launchctl("unload", server_jobs, nitems(server_jobs));

	if (unlink(DS_ROLE) == -1 && errno != ENOENT) {
		warn("%s", DS_ROLE);
		return (1);
	}
	say("  removed %s", DS_ROLE);
	if (unlink(DS_EXPORTS) == -1 && errno != ENOENT)
		warn("%s", DS_EXPORTS);
	else
		say("  removed %s", DS_EXPORTS);
	if (unlink(DS_SERVICE_FILE) == -1 && errno != ENOENT)
		warn("%s", DS_SERVICE_FILE);
	else
		say("  withdrew %s", DS_SERVICE_TYPE);

	(void)printf("This machine is standalone again.\n");
	return (0);
}

static int
do_join(const char *server)
{
	char cur[MAXHOSTNAMELEN];
	enum role r = current_role(cur, sizeof(cur));

	if (r == R_SERVER) {
		warnx("this machine is a directory server; run dsdemote first");
		return (1);
	}
	if (r == R_CLIENT) {
		/*
		 * Refused rather than rebound. Leaving implicitly would
		 * unmount homes from under anyone logged in.
		 */
		warnx("already bound to %s; run dsleave first",
		    cur[0] != '\0' ? cur : "a directory server");
		return (1);
	}
	if (server == NULL || server[0] == '\0') {
		warnx("no server given");
		return (2);
	}
	if (strspn(server, "abcdefghijklmnopqrstuvwxyz"
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") != strlen(server)) {
		warnx("%s is not a usable host name", server);
		return (2);
	}

	if (binding_write(server) == -1)
		return (1);
	if (ntp_set(server) == -1)
		warnx("the binding is written; the time source is not");
	launchctl("load", client_jobs, nitems(client_jobs));

	(void)printf("Joined %s.\n", server);
	return (0);
}

static int
do_leave(void)
{
	char server[MAXHOSTNAMELEN];

	if (current_role(server, sizeof(server)) != R_CLIENT) {
		warnx("this machine is not bound to a directory server");
		return (1);
	}
	launchctl("unload", client_jobs, nitems(client_jobs));
	if (unlink(DS_BINDING) == -1) {
		warn("%s", DS_BINDING);
		return (1);
	}
	say("  removed %s", DS_BINDING);
	if (ntp_set(NULL) == -1)
		warnx("the binding is gone; the time source is not cleared");

	(void)printf("Left %s.\n", server[0] != '\0' ? server : "the directory");
	return (0);
}

static int
do_status(void)
{
	char server[MAXHOSTNAMELEN];

	switch (current_role(server, sizeof(server))) {
	case R_SERVER:
		(void)printf("directory server\n");
		(void)printf("  accounts   %s (shared)\n", DS_LOCAL_DIR);
		(void)printf("  homes      %s (shared)\n", DS_LOCAL_USERS);
		(void)printf("  exports    %s\n",
		    exists(DS_EXPORTS) ? DS_EXPORTS : "(none)");
		(void)printf("  advertised %s\n",
		    exists(DS_SERVICE_FILE) ? DS_SERVICE_TYPE : "(no)");
		break;
	case R_CLIENT:
		(void)printf("bound to %s\n",
		    server[0] != '\0' ? server : "(unreadable binding)");
		(void)printf("  accounts   %s\n", DS_NETWORK_DIR);
		(void)printf("  homes      %s\n", DS_NETWORK_USERS);
		break;
	case R_STANDALONE:
		(void)printf("standalone\n");
		(void)printf("  accounts   %s\n", DS_LOCAL_DIR);
		(void)printf("  homes      %s\n", DS_LOCAL_USERS);
		break;
	}
	return (0);
}

/* ---------------------------------------------------------------- dispatch */

static enum mode
mode_from_name(const char *argv0)
{
	const char *base;

	base = (base = strrchr(argv0, '/')) != NULL ? base + 1 : argv0;
	if (strcmp(base, "dspromote") == 0) return (M_PROMOTE);
	if (strcmp(base, "dsdemote") == 0)  return (M_DEMOTE);
	if (strcmp(base, "dsjoin") == 0)    return (M_JOIN);
	if (strcmp(base, "dsleave") == 0)   return (M_LEAVE);
	if (strcmp(base, "dsstatus") == 0)  return (M_STATUS);
	errx(2, "%s: not one of dspromote, dsdemote, dsjoin, dsleave, dsstatus",
	    base);
}

static void
usage(enum mode m)
{
	switch (m) {
	case M_JOIN:   fprintf(stderr, "usage: dsjoin [-v] server\n"); break;
	case M_PROMOTE:fprintf(stderr, "usage: dspromote [-v]\n"); break;
	case M_DEMOTE: fprintf(stderr, "usage: dsdemote [-v]\n"); break;
	case M_LEAVE:  fprintf(stderr, "usage: dsleave [-v]\n"); break;
	case M_STATUS: fprintf(stderr, "usage: dsstatus\n"); break;
	}
	exit(2);
}

int
main(int argc, char *argv[])
{
	enum mode m = mode_from_name(argv[0]);
	int ch;

	while ((ch = getopt(argc, argv, "v")) != -1) {
		switch (ch) {
		case 'v': verbose = true; break;
		default:  usage(m);
		}
	}
	argc -= optind;
	argv += optind;

	if (m == M_JOIN) {
		if (argc != 1)
			usage(m);
	} else if (argc != 0)
		usage(m);

	/*
	 * Everything but dsstatus writes /etc and moves system files.
	 *
	 * Test hook, with its own macro for the reason given in adduser: a
	 * flag that lets this believe it is root, against a staged fake root,
	 * must not be reachable through a macro another component might
	 * define. Nothing in the shipped build defines DS_TEST.
	 */
#ifndef DS_TEST
	if (m != M_STATUS && geteuid() != 0)
		errx(1, "must be root");
#endif

	switch (m) {
	case M_PROMOTE: return (do_promote());
	case M_DEMOTE:  return (do_demote());
	case M_JOIN:    return (do_join(argv[0]));
	case M_LEAVE:   return (do_leave());
	case M_STATUS:  return (do_status());
	}
	return (1);
}
