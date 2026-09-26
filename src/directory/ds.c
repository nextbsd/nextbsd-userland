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
 *   server   nothing mounts /Network on it, so it reads /Local and its homes
 *            are under /Local/Users -- correct, because those homes really are
 *            local. Every command asks about /Local first and stops there, so
 *            a /Network that somehow appeared could not change the answer
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
 * Domain.plist in /Local/Library/DirectoryServices, written by promotion and
 * removed by demotion. It is Gershwin's marker at Gershwin's path: `dscli
 * promote` writes an empty dict there and `dscli demote` removes it, so a
 * machine promoted by either tool is a server to both and neither invents a
 * file of its own. Its presence is the whole of the value -- reading a value
 * out of it would not recognise a machine dscli promoted.
 *
 * The previous design read the role off the /Local symlink it had just
 * created, which worked but used a side effect as state. A file survives a
 * reboot, an unmounted /Network and a stopped nfsd just as well.
 *
 * HOW EVERY COMMAND ASKS
 *
 * One question, one order, first answer wins -- here, in the account tools,
 * and in dscli before both:
 *
 *   /Local/Library/DirectoryServices/Domain.plist     this machine IS the
 *       server. /Network is not consulted at all.
 *   /Network/Library/DirectoryServices/Domain.plist   bound to one.
 *   neither                                          standalone.
 *
 * The server is asked about first because the whole of /Local is exported, so
 * its own marker arrives under /Network with everything else. Asked in the
 * wrong order, the one machine that owns the accounts would report itself
 * somebody else's client and every tool would refuse to manage them.
 *
 * Testing the marker under /Network rather than Users.plist also means a
 * client whose server has been demoted reads as standalone at once: the
 * marker leaves the export, and nothing has to notify the client. An
 * unmounted /Network is standalone too, deliberately -- the tools then do
 * what they would do on a machine that never joined. dsleave is the one
 * exception, because an unbindable client would otherwise be unbindable
 * forever.
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
#ifndef DS_DOMAIN
#define DS_DOMAIN	"/Local/Library/DirectoryServices/Domain.plist"
#endif
#ifndef DS_NETWORK_DOMAIN
#define DS_NETWORK_DOMAIN "/Network/Library/DirectoryServices/Domain.plist"
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
#ifndef DS_LAUNCHD_DIR
#define DS_LAUNCHD_DIR	"/System/Library/LaunchDaemons"
#endif
#ifndef DS_UMOUNT
#define DS_UMOUNT	"/sbin/umount"
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

/*
 * The managed block in /etc/exports. Nothing ships these markers: dspromote
 * appends the block to whatever the administrator has, or creates the file,
 * and dsdemote takes the block out again. See exports_set().
 */
#define EXPORTS_BEGIN "# BEGIN directory server (managed by dspromote and dsdemote; do not edit)"
#define EXPORTS_END   "# END directory server"

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
		(void)snprintf(path, sizeof(path), "%s/%s.plist",
		    DS_LAUNCHD_DIR, labels[i]);
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

/* ------------------------------------------------------------- the unmounts */

/*
 * Unmount what dsjoin mounted. Reported, never fatal: a busy mount is the
 * administrator's problem to clear, and refusing to finish dsleave over it
 * would leave the machine bound to a server it has been told to leave.
 *
 * This is not cosmetic. nss_directory_services prefers /Network whenever the
 * file is there (dsdb.c), so a mount that outlives the binding keeps the
 * machine resolving the old server's accounts while dsstatus reports
 * standalone -- and in that state every account tool's binding check passes,
 * so rmuser would remove_tree() a home under the server's export.
 *
 * Deliberately not -f: yanking a busy NFS mount to tidy up is worse than
 * saying so and leaving it.
 */
static void
unmount_network(void)
{
	static const char *const points[] = { DS_NETWORK_USERS, DS_NETWORK_DIR };
	/*
	 * execv(3) wants char *const argv[], so these are writable copies
	 * rather than a cast that throws const away -- which the tree builds
	 * with -Werror -Wcast-qual and rightly refuses.
	 */
	char prog[] = DS_UMOUNT;
	char mp[PATH_MAX];
	char *argv[3];
	size_t i;
	pid_t pid;
	int st;

	for (i = 0; i < nitems(points); i++) {
		if (!exists(points[i]))
			continue;
		if (strlcpy(mp, points[i], sizeof(mp)) >= sizeof(mp)) {
			warnx("%s: path too long to unmount", points[i]);
			continue;
		}
		argv[0] = prog;
		argv[1] = mp;
		argv[2] = NULL;
		if ((pid = fork()) == -1) {
			warn("fork");
			return;
		}
		if (pid == 0) {
			(void)execv(prog, argv);
			_exit(127);
		}
		if (waitpid(pid, &st, 0) == -1 || !WIFEXITED(st) ||
		    WEXITSTATUS(st) != 0)
			warnx("%s is still mounted; unmount it by hand",
			    points[i]);
		else
			say("  unmounted %s", points[i]);
	}
}

/* --------------------------------------------------------- atomic file swap */

/*
 * Replace `path` in one step. Used for the files that decide what this machine
 * IS: a Domain.plist that exists but is still being written would leave a
 * reader with a torn file, and an interrupted write would leave a machine
 * exporting and running nfsd with no marker to demote it by.
 *
 * The mode is carried from the file being replaced when there is one, so this
 * never quietly changes an administrator's permissions -- the bug that made
 * /etc/ntp.conf 0600 after every dsjoin.
 */
struct atomic {
	char	 tmp[PATH_MAX];
	char	 path[PATH_MAX];
	FILE	*f;
};

static FILE *
atomic_open(struct atomic *a, const char *path, mode_t fallback)
{
	struct stat st;
	int fd;

	if (strlcpy(a->path, path, sizeof(a->path)) >= sizeof(a->path) ||
	    (size_t)snprintf(a->tmp, sizeof(a->tmp), "%s.ds.XXXXXX", path) >=
	    sizeof(a->tmp)) {
		errno = ENAMETOOLONG;
		warn("%s", path);
		return (NULL);
	}
	if ((fd = mkstemp(a->tmp)) == -1) {
		warn("%s", a->tmp);
		return (NULL);
	}
	if (fchmod(fd, stat(path, &st) == 0 ? (st.st_mode & 07777) : fallback)
	    == -1) {
		warn("%s", a->tmp);
		(void)close(fd);
		(void)unlink(a->tmp);
		return (NULL);
	}
	if ((a->f = fdopen(fd, "w")) == NULL) {
		warn("%s", a->tmp);
		(void)close(fd);
		(void)unlink(a->tmp);
		return (NULL);
	}
	return (a->f);
}

/* fsync before rename, so the rename cannot land ahead of the bytes. */
static int
atomic_commit(struct atomic *a)
{
	int fd = fileno(a->f);

	if (fflush(a->f) != 0 || fsync(fd) == -1) {
		warn("%s", a->tmp);
		(void)fclose(a->f);
		(void)unlink(a->tmp);
		return (-1);
	}
	if (fclose(a->f) != 0 || rename(a->tmp, a->path) == -1) {
		warn("%s", a->path);
		(void)unlink(a->tmp);
		return (-1);
	}
	return (0);
}

/* Give up on a replacement, leaving the file it was for untouched. */
static void
atomic_abort(struct atomic *a)
{
	(void)fclose(a->f);
	(void)unlink(a->tmp);
}

/* ------------------------------------------------------ the NTP time source */

/*
 * Fill or empty the managed block in /etc/ntp.conf. The block's markers are
 * shipped by nextbsd-overlays with nothing between them, so the common case
 * is rewriting the body in place. A file that has lost its markers gets them
 * appended, and one we cannot read at all is reported rather than clobbered:
 * ntp.conf is the administrator's file and only this block is ours -- which
 * now includes its mode: the replacement goes through atomic_open(), which
 * carries the mode over. It previously came out 0600 root:wheel after every
 * dsjoin and dsleave, whatever it had been.
 */
static int
ntp_set(const char *server)
{
	char line[1024];
	struct atomic a;
	FILE *in, *out;
	bool in_block = false, seen = false;

	if ((in = fopen(DS_NTP_CONF, "r")) == NULL) {
		warn("%s", DS_NTP_CONF);
		return (-1);
	}
	if ((out = atomic_open(&a, DS_NTP_CONF, 0644)) == NULL) {
		(void)fclose(in);
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
	if (atomic_commit(&a) == -1)
		return (-1);
	say("  %s the time source in %s",
	    server != NULL ? "set" : "cleared", DS_NTP_CONF);
	return (0);
}

/* ------------------------------------------------------------- the exports */

/*
 * Fill or remove the managed block in /etc/exports, the way ntp_set() treats
 * ntp.conf: the file is the administrator's and only the block is ours. It
 * used to be opened "w", which threw away every export written by hand, and
 * dsdemote unlinked the whole file.
 *
 * Nothing ships these markers, so promotion rewrites the block in place when
 * one is there and appends it otherwise, creating the file when there is
 * none. Demotion drops the block, markers included. When the block was the
 * whole file the file goes too: mountd is unloaded by then, and exports
 * nothing from an empty file just as from a missing one (the difference is a
 * "can't open" line in syslog), so the only reader left is a person, to whom
 * an empty /etc/exports that dspromote invented looks like configuration. A
 * file that has no block -- including an empty one the administrator made --
 * is left exactly as found.
 *
 * The kernel permits one default export line per filesystem, so directories
 * that share a device have to share a line. On the usual single-filesystem
 * layout that is one line naming both paths; on a machine where /Local is its
 * own filesystem it is two.
 */
static void
exports_body(FILE *out, bool one_line)
{
	if (one_line)
		(void)fprintf(out, "%s %s -alldirs\n", DS_LOCAL_USERS,
		    DS_LOCAL_DIR);
	else {
		(void)fprintf(out, "%s -alldirs\n", DS_LOCAL_USERS);
		(void)fprintf(out, "%s -alldirs\n", DS_LOCAL_DIR);
	}
}

static int
exports_set(bool on)
{
	char line[1024];
	struct stat a, b;
	struct atomic at;
	FILE *in, *out;
	bool one_line = true;
	bool in_block = false, seen = false, kept = false, pending = false;

	if (on) {
		if (stat(DS_LOCAL_USERS, &a) == -1) {
			warn("%s", DS_LOCAL_USERS);
			return (-1);
		}
		if (stat(DS_LOCAL_DIR, &b) == -1) {
			warn("%s", DS_LOCAL_DIR);
			return (-1);
		}
		one_line = a.st_dev == b.st_dev;
	}
	if ((in = fopen(DS_EXPORTS, "r")) == NULL) {
		if (errno != ENOENT) {
			warn("%s", DS_EXPORTS);
			return (-1);
		}
		if (!on) {
			say("  %s does not exist; nothing to remove",
			    DS_EXPORTS);
			return (0);
		}
	}
	if ((out = atomic_open(&at, DS_EXPORTS, 0644)) == NULL) {
		if (in != NULL)
			(void)fclose(in);
		return (-1);
	}
	/*
	 * A blank line is held back until the next line shows whether it is
	 * the separator promotion put before the block -- which leaves with
	 * the block, so demotion gives the file back exactly as it was.
	 */
	while (in != NULL && fgets(line, sizeof(line), in) != NULL) {
		if (!in_block &&
		    strncmp(line, EXPORTS_BEGIN, strlen(EXPORTS_BEGIN)) == 0) {
			in_block = true; seen = true;
			if (on) {
				if (pending)
					(void)fputs("\n", out);
				(void)fputs(EXPORTS_BEGIN "\n", out);
				exports_body(out, one_line);
			}
			pending = false;
			continue;
		}
		if (in_block) {
			if (strncmp(line, EXPORTS_END,
			    strlen(EXPORTS_END)) == 0) {
				in_block = false;
				if (on)
					(void)fputs(EXPORTS_END "\n", out);
			}
			continue;	/* drop whatever the block held */
		}
		if (pending) {
			(void)fputs("\n", out);
			kept = true;
		}
		pending = strcmp(line, "\n") == 0;
		if (!pending) {
			(void)fputs(line, out);
			kept = true;
		}
	}
	if (pending) {
		(void)fputs("\n", out);
		kept = true;
	}
	if (in != NULL)
		(void)fclose(in);
	/* A block that lost its END marker gets one, or it would run to EOF. */
	if (in_block && on)
		(void)fputs(EXPORTS_END "\n", out);
	if (on && !seen) {
		if (kept)
			(void)fputs("\n", out);
		(void)fputs(EXPORTS_BEGIN "\n", out);
		exports_body(out, one_line);
		(void)fputs(EXPORTS_END "\n", out);
	}
	if (!on && !seen) {
		atomic_abort(&at);
		say("  %s has no managed block; left as it is", DS_EXPORTS);
		return (0);
	}
	if (!on && !kept) {
		atomic_abort(&at);
		if (unlink(DS_EXPORTS) == -1 && errno != ENOENT) {
			warn("%s", DS_EXPORTS);
			return (-1);
		}
		say("  removed %s; the block was all it held", DS_EXPORTS);
		return (0);
	}
	if (atomic_commit(&at) == -1)
		return (-1);
	say("  %s the managed block in %s", on ? "wrote" : "removed",
	    DS_EXPORTS);
	return (0);
}

/* True when some line of `path` starts with `marker`. */
static bool
has_line(const char *path, const char *marker)
{
	char line[1024];
	FILE *f;
	bool found = false;

	if ((f = fopen(path, "r")) == NULL)
		return (false);
	while (!found && fgets(line, sizeof(line), f) != NULL)
		found = strncmp(line, marker, strlen(marker)) == 0;
	(void)fclose(f);
	return (found);
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
 * Domain.plist marks a machine as a server. It is Gershwin's marker, at
 * Gershwin's path, with Gershwin's contents: an empty dict, whose presence is
 * the whole of the value. `dscli promote` writes @{} and `dscli demote`
 * removes it, so a machine promoted by either tool is a server to both.
 *
 * Nothing is recorded for a client: that is the /Network mount, which is what
 * dscli reads too.
 */
static int
domain_write(void)
{
	struct atomic a;
	FILE *f;

	if ((f = atomic_open(&a, DS_DOMAIN, 0644)) == NULL)
		return (-1);
	(void)fprintf(f,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n"
	    "<dict/>\n"
	    "</plist>\n");
	if (atomic_commit(&a) == -1)
		return (-1);
	say("  wrote %s", DS_DOMAIN);
	return (0);
}

/*
 * True when this machine has been promoted -- the file existing is the test,
 * matching dscli's `[fm fileExistsAtPath:DS_DOMAIN_PLIST]`. Reading a value
 * instead would not recognise a machine dscli promoted, because the dict it
 * writes is empty.
 *
 * atomic_open() and atomic_commit() still do the writing, so a half-written
 * file is never observed under this path at all.
 */
static bool
is_server(void)
{
	return (exists(DS_DOMAIN));
}

/*
 * The bound server's name. Binding.plist records where to mount from -- it is
 * what network-mount reads before /Network exists -- but it does not decide
 * the role: is_client() does. False here means only that the name could not be
 * read, never that this machine is unbound.
 */
static bool
binding_server(char *out, size_t len)
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
 * True when a directory is mounted and the thing behind it really is a
 * promoted server. The whole of /Local is exported, so the server's own
 * Domain.plist arrives under /Network with it; testing for that rather than
 * for Users.plist means a client whose server has been demoted reads as
 * standalone at once, with nothing to notify it.
 */
static bool
is_client(void)
{
	return (exists(DS_NETWORK_DOMAIN));
}

/*
 * One question, asked in one order, and the first answer wins.
 *
 *	/Local/...../Domain.plist	this machine IS the server. /Network is
 *					not consulted at all.
 *	/Network/.../Domain.plist	bound to one.
 *	neither				standalone.
 *
 * The server is asked about first and answers from /Local alone, so its own
 * exported Domain.plist -- which its clients see through /Network, and which
 * it would see itself if anything ever mounted /Network on it -- can never be
 * mistaken for a binding of its own. An unmounted /Network is standalone,
 * deliberately: the tools then do what they would do on any unjoined machine.
 */
static enum role
current_role(char *server, size_t len)
{
	if (server != NULL && len > 0)
		server[0] = '\0';
	if (is_server())
		return (R_SERVER);
	if (is_client()) {
		(void)binding_server(server, len);
		return (R_CLIENT);
	}
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
	/*
	 * Order matters, and it used to be wrong. The role marker is written
	 * LAST, after the exports and the Bonjour record, because it is what
	 * current_role() reads: written first, a failure in exports_set() or
	 * write_service() left a machine reporting "directory server" with
	 * "exports (none)", which dspromote then refused to retry ("already a
	 * directory server") and only dsdemote could undo.
	 *
	 * The jobs come after the marker for the same reason -- a load failure
	 * is reported but never fatal (see launchctl()), so by then the machine
	 * genuinely is a server and dsdemote can take it back.
	 */
	if (exports_set(true) == -1 || write_service() == -1)
		return (1);
	if (domain_write() == -1)
		return (1);
	launchctl("load", server_jobs, nitems(server_jobs));

	(void)printf("This machine is now a directory server.\n");
	return (0);
}

static int
do_demote(void)
{
	char server[MAXHOSTNAMELEN];
	int rv = 0;

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

	if (unlink(DS_DOMAIN) == -1 && errno != ENOENT) {
		warn("%s", DS_DOMAIN);
		return (1);
	}
	say("  removed %s", DS_DOMAIN);
	/*
	 * The marker is gone, so the machine IS standalone from here; what
	 * follows is cleanup, reported and reflected in the exit status but
	 * never a reason to stop.
	 */
	if (exports_set(false) == -1)
		rv = 1;
	if (unlink(DS_SERVICE_FILE) == -1 && errno != ENOENT) {
		warn("%s", DS_SERVICE_FILE);
		rv = 1;
	} else
		say("  withdrew %s", DS_SERVICE_TYPE);

	(void)printf("This machine is standalone again.\n");
	return (rv);
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
	/*
	 * Refused rather than rebound. Leaving implicitly would unmount homes
	 * from under anyone logged in. Binding.plist is checked alongside the
	 * role because a client whose /Network is down reads as standalone, and
	 * rebinding it would strand the old mount and the old ntp source.
	 */
	if (r == R_CLIENT || exists(DS_BINDING)) {
		(void)binding_server(cur, sizeof(cur));
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

	/*
	 * A client whose /Network is down reads as standalone, which is what
	 * every other tool wants -- but dsleave must still be able to undo the
	 * join, or a machine that cannot reach its server could never be
	 * unbound and Binding.plist, the client job and the managed ntp.conf
	 * block would have nothing left to clear them.
	 */
	switch (current_role(server, sizeof(server))) {
	case R_SERVER:
		warnx("this machine is a directory server; run dsdemote");
		return (1);
	case R_CLIENT:
		break;
	case R_STANDALONE:
		if (!exists(DS_BINDING)) {
			warnx("this machine is not bound to a directory server");
			return (1);
		}
		(void)binding_server(server, sizeof(server));
		say("  %s is not mounted; clearing the binding anyway",
		    DS_NETWORK_DIR);
		break;
	}
	launchctl("unload", client_jobs, nitems(client_jobs));
	if (unlink(DS_BINDING) == -1) {
		warn("%s", DS_BINDING);
		return (1);
	}
	say("  removed %s", DS_BINDING);
	unmount_network();
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
		    has_line(DS_EXPORTS, EXPORTS_BEGIN) ? DS_EXPORTS : "(none)");
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
