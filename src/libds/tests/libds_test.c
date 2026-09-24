/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * Unit tests for libds (nextbsd/nextbsd-userland#287).
 *
 * Everything happens inside a fixture directory under a temporary
 * location, so no root is needed and nothing real is touched. The output
 * is checked twice: once by libds itself, and once by the NSS module's
 * separate CoreFoundation-free parser, which is what the running system
 * will actually use to read these files.
 */

#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libds.h"
#include "plist.h"		/* the NSS module's own reader */

static int	 failures;
static int	 checks;
static char	 fixture[PATH_MAX];

static void
ck(bool cond, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (cond)
		return;
	failures++;
	va_start(ap, fmt);
	fputs("FAIL: ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

static char *
slurp(const char *path, size_t *lenp)
{
	struct stat st;
	char *buf;
	FILE *f;

	if (stat(path, &st) == -1 || (f = fopen(path, "r")) == NULL)
		return (NULL);
	if ((buf = malloc((size_t)st.st_size + 1)) == NULL) {
		fclose(f);
		return (NULL);
	}
	*lenp = fread(buf, 1, (size_t)st.st_size, f);
	buf[*lenp] = '\0';
	fclose(f);
	return (buf);
}

static void
write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

static void
fixture_path(char *out, size_t len, const char *name)
{
	(void)snprintf(out, len, "%s/%s", fixture, name);
}

/* The seeded shape, as nextbsd-overlays ships it. */
static const char seed_users[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
"<plist version=\"1.0\">\n"
"<dict>\n"
"\t<key>admin</key>\n"
"\t<dict>\n"
"\t\t<key>gid</key>\n"
"\t\t<integer>5000</integer>\n"
"\t\t<key>noPassword</key>\n"
"\t\t<true/>\n"
"\t\t<key>realName</key>\n"
"\t\t<string>Local Administrator</string>\n"
"\t\t<key>shell</key>\n"
"\t\t<string>/bin/zsh</string>\n"
"\t\t<key>uid</key>\n"
"\t\t<integer>5000</integer>\n"
"\t\t<key>username</key>\n"
"\t\t<string>admin</string>\n"
"\t</dict>\n"
"</dict>\n"
"</plist>\n";

static const char seed_groups[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
"<plist version=\"1.0\">\n"
"<dict>\n"
"\t<key>admin</key>\n"
"\t<dict>\n"
"\t\t<key>gid</key>\n"
"\t\t<integer>5000</integer>\n"
"\t\t<key>groupname</key>\n"
"\t\t<string>admin</string>\n"
"\t\t<key>members</key>\n"
"\t\t<array>\n"
"\t\t\t<string>admin</string>\n"
"\t\t</array>\n"
"\t</dict>\n"
"</dict>\n"
"</plist>\n";

static void
seed(void)
{
	char p[PATH_MAX];

	fixture_path(p, sizeof(p), DS_USERS_PLIST);
	write_file(p, seed_users);
	fixture_path(p, sizeof(p), DS_GROUPS_PLIST);
	write_file(p, seed_groups);
}

/* ---- tests ---------------------------------------------------------- */

static void
test_read_seeded(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	struct ds_grouprec g;
	bool has;

	seed();
	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "open seeded read-only");
	if (h == NULL)
		return;
	ck(ds_user_count(h) == 1, "one seeded user, got %zu",
	    ds_user_count(h));
	ck(ds_user_get(h, "admin", &u) == DS_OK, "get admin");
	ck(strcmp(u.username, "admin") == 0, "username is admin, got %s",
	    u.username);
	ck(u.uid == 5000, "uid 5000, got %u", (unsigned)u.uid);
	ck(u.gid == 5000, "gid 5000, got %u", (unsigned)u.gid);
	ck(strcmp(u.realName, "Local Administrator") == 0,
	    "realName, got %s", u.realName);
	ck(strcmp(u.shell, "/bin/zsh") == 0, "shell, got %s", u.shell);
	ck(u.noPassword, "noPassword is true");
	ck(!u.hasHash, "seeded admin carries no hash");
	ck(ds_user_get(h, "nosuch", &u) == DS_ENOENT, "missing user is ENOENT");

	ck(ds_group_get(h, "admin", &g) == DS_OK, "get admin group");
	ck(g.gid == 5000, "group gid 5000, got %u", (unsigned)g.gid);
	ck(ds_group_member_count(h, "admin") == 1, "one member");
	ck(ds_group_has_member(h, "admin", "admin", &has) == DS_OK && has,
	    "admin is a member of admin");

	/* A read-only handle refuses to mutate. */
	ck(ds_user_del(h, "admin") == DS_EINVAL, "read-only refuses delete");
	ck(ds_commit(h) == DS_EINVAL, "read-only refuses commit");
	ds_close(h);
}

static void
test_add_user_and_membership(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	bool has;

	seed();
	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open read-write");
	if (h == NULL)
		return;
	memset(&u, 0, sizeof(u));
	(void)strlcpy(u.username, "bob", sizeof(u.username));
	(void)strlcpy(u.realName, "Bob Tester", sizeof(u.realName));
	(void)strlcpy(u.shell, "/bin/zsh", sizeof(u.shell));
	(void)strlcpy(u.passwordHash, "$6$abc$def", sizeof(u.passwordHash));
	u.hasHash = true;
	u.uid = 5001;
	u.gid = 5001;

	ck(ds_user_add(h, &u) == DS_OK, "add bob");
	ck(ds_user_add(h, &u) == DS_EEXIST, "adding bob twice is EEXIST");
	ck(ds_dirty(h), "handle is dirty after an add");
	ck(ds_group_add_member(h, "admin", "bob") == DS_OK, "promote bob");
	ck(ds_group_add_member(h, "admin", "bob") == DS_OK,
	    "promoting twice succeeds and is a no-op");
	ck(ds_group_member_count(h, "admin") == 2,
	    "two members after promote, got %zu",
	    ds_group_member_count(h, "admin"));
	ck(ds_group_add_member(h, "nosuch", "bob") == DS_ENOENT,
	    "promoting into a missing group is ENOENT");
	ck(ds_commit(h) == DS_OK, "commit");
	ck(!ds_dirty(h), "clean after commit");
	ds_close(h);

	/* Read it back from disk with a fresh handle. */
	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "reopen");
	if (h == NULL)
		return;
	ck(ds_user_count(h) == 2, "two users on disk, got %zu",
	    ds_user_count(h));
	memset(&u, 0, sizeof(u));
	ck(ds_user_get(h, "bob", &u) == DS_OK, "bob survived the commit");
	ck(u.uid == 5001, "bob uid, got %u", (unsigned)u.uid);
	ck(u.hasHash && strcmp(u.passwordHash, "$6$abc$def") == 0,
	    "bob's hash round-tripped, got %s", u.passwordHash);
	ck(!u.noPassword, "bob has no noPassword key");
	ck(ds_group_has_member(h, "admin", "bob", &has) == DS_OK && has,
	    "bob is in admin on disk");
	ck(ds_user_get_by_uid(h, 5001, &u) == DS_OK &&
	    strcmp(u.username, "bob") == 0, "lookup by uid");
	ds_close(h);
}

static void
test_modify_and_delete(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	bool has;

	/* Starts from the state test_add_user_and_membership left. */
	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open for modify");
	if (h == NULL)
		return;
	ck(ds_user_get(h, "bob", &u) == DS_OK, "read bob");
	(void)strlcpy(u.shell, "/bin/sh", sizeof(u.shell));
	u.hasHash = false;
	u.passwordHash[0] = '\0';
	u.noPassword = true;
	ck(ds_user_set(h, &u) == DS_OK, "set bob");
	(void)strlcpy(u.username, "ghost", sizeof(u.username));
	ck(ds_user_set(h, &u) == DS_ENOENT, "set on a missing user is ENOENT");

	ck(ds_group_del_member(h, "admin", "bob") == DS_OK, "demote bob");
	ck(ds_group_del_member(h, "admin", "bob") == DS_ENOENT,
	    "demoting twice is ENOENT");
	ck(ds_commit(h) == DS_OK, "commit the modify");
	ds_close(h);

	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "reopen after modify");
	if (h == NULL)
		return;
	ck(ds_user_get(h, "bob", &u) == DS_OK, "bob still there");
	ck(strcmp(u.shell, "/bin/sh") == 0, "shell changed, got %s", u.shell);
	ck(!u.hasHash, "hash was dropped");
	ck(u.noPassword, "noPassword was set");
	ck(ds_group_has_member(h, "admin", "bob", &has) == DS_OK && !has,
	    "bob is no longer in admin");
	ds_close(h);

	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open for delete");
	if (h == NULL)
		return;
	ck(ds_user_del(h, "bob") == DS_OK, "delete bob");
	ck(ds_user_del(h, "bob") == DS_ENOENT, "deleting twice is ENOENT");
	ck(ds_commit(h) == DS_OK, "commit the delete");
	ds_close(h);

	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "reopen after delete");
	if (h != NULL) {
		ck(ds_user_count(h) == 1, "back to one user, got %zu",
		    ds_user_count(h));
		ds_close(h);
	}
}

static void
test_name_validation(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	const char *bad[] = { "", "a:b", "a/b", "a b", "-lead", ".lead",
	    "a,b", NULL };
	size_t i;

	seed();
	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open for validation");
	if (h == NULL)
		return;
	for (i = 0; bad[i] != NULL; i++) {
		memset(&u, 0, sizeof(u));
		(void)strlcpy(u.username, bad[i], sizeof(u.username));
		u.uid = 6000 + (uid_t)i;
		u.gid = 6000 + (gid_t)i;
		ck(ds_user_add(h, &u) == DS_EINVAL,
		    "rejects the name \"%s\"", bad[i]);
	}
	ck(!ds_dirty(h), "no rejected name made the handle dirty");
	ds_close(h);
}

static void
test_next_uid(void)
{
	struct ds_handle *h;
	uid_t id;

	seed();			/* admin is 5000 */
	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "open for uid search");
	if (h == NULL)
		return;
	ck(ds_user_next_uid(h, 5000, 5100, false, &id) == DS_OK && id == 5001,
	    "first free uid above 5000 is 5001, got %u", (unsigned)id);
	ck(ds_user_next_uid(h, 5000, 5000, false, &id) == DS_ENOENT,
	    "an exhausted range is ENOENT");
	ck(ds_user_next_uid(h, 5100, 5000, false, &id) == DS_EINVAL,
	    "an inverted range is EINVAL");
	ds_close(h);
}

/* The output must be readable by the NSS module's independent parser. */
static void
test_nss_parser_agrees(void)
{
	char path[PATH_MAX];
	struct pl_node *root;
	const struct pl_node *rec;
	const char *s;
	char *buf;
	size_t len;
	long long v;

	fixture_path(path, sizeof(path), DS_USERS_PLIST);
	if ((buf = slurp(path, &len)) == NULL) {
		ck(false, "could not read back %s", path);
		return;
	}
	root = pl_parse(buf, len);
	free(buf);
	ck(root != NULL, "the NSS parser accepts what libds wrote");
	if (root == NULL)
		return;
	ck(root->type == PL_DICT, "NSS parser sees a dictionary");
	rec = pl_dict_get(root, "admin");
	ck(rec != NULL, "NSS parser finds the admin record");
	if (rec != NULL) {
		s = pl_dict_string(rec, "username");
		ck(s != NULL && strcmp(s, "admin") == 0,
		    "NSS parser reads username");
		ck(pl_dict_integer(rec, "uid", &v) == 0 && v == 5000,
		    "NSS parser reads uid");
	}
	pl_free(root);
}

/*
 * Free text survives the trip out through CoreFoundation and back in
 * through the NSS module's parser. realName comes from an adduser prompt,
 * so it can contain any of the five characters XML escapes; CF writes
 * them as entities and the module's own reader has to decode them. If
 * these two ever disagree, a user's name silently corrupts.
 */
static void
test_xml_escaping_round_trip(void)
{
	static const char nasty[] = "Ampersand & \"Quote\" <Angle> 'Apos'";
	struct ds_handle *h;
	struct ds_userrec u;
	struct pl_node *root;
	const struct pl_node *rec;
	const char *s;
	char path[PATH_MAX], *buf;
	size_t len;

	seed();
	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open for escaping test");
	if (h == NULL)
		return;
	ck(ds_user_get(h, "admin", &u) == DS_OK, "read admin");
	(void)strlcpy(u.realName, nasty, sizeof(u.realName));
	ck(ds_user_set(h, &u) == DS_OK, "store the awkward realName");
	ck(ds_commit(h) == DS_OK, "commit it");
	ds_close(h);

	/* libds reads its own output back unchanged. */
	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "reopen");
	if (h != NULL) {
		memset(&u, 0, sizeof(u));
		ck(ds_user_get(h, "admin", &u) == DS_OK, "reread admin");
		ck(strcmp(u.realName, nasty) == 0,
		    "libds round-trips it: got [%s]", u.realName);
		ds_close(h);
	}

	/* And so does the module's independent parser. */
	fixture_path(path, sizeof(path), DS_USERS_PLIST);
	if ((buf = slurp(path, &len)) == NULL) {
		ck(false, "could not read back the users plist");
		return;
	}
	/* The file on disk really is escaped, not raw. */
	ck(strstr(buf, "&amp;") != NULL, "the file contains &amp;");
	ck(strstr(buf, "&lt;") != NULL, "the file contains &lt;");
	root = pl_parse(buf, len);
	free(buf);
	ck(root != NULL, "the NSS parser accepts the escaped file");
	if (root == NULL)
		return;
	rec = pl_dict_get(root, "admin");
	s = rec != NULL ? pl_dict_string(rec, "realName") : NULL;
	ck(s != NULL && strcmp(s, nasty) == 0,
	    "the NSS parser decodes the entities the same way: got [%s]",
	    s == NULL ? "(null)" : s);
	pl_free(root);
}

/* Mode and owner come from the file being replaced. */
static void
test_mode_preserved(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	char path[PATH_MAX];
	struct stat before, after;

	seed();
	fixture_path(path, sizeof(path), DS_USERS_PLIST);
	ck(chmod(path, 0600) == 0, "tighten the fixture to 0600");
	ck(stat(path, &before) == 0, "stat before");

	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open to rewrite");
	if (h == NULL)
		return;
	ck(ds_user_get(h, "admin", &u) == DS_OK, "read admin");
	(void)strlcpy(u.realName, "Changed", sizeof(u.realName));
	ck(ds_user_set(h, &u) == DS_OK, "modify admin");
	ck(ds_commit(h) == DS_OK, "commit");
	ds_close(h);

	ck(stat(path, &after) == 0, "stat after");
	ck((after.st_mode & 07777) == (before.st_mode & 07777),
	    "mode preserved: was 0%o, now 0%o",
	    before.st_mode & 07777, after.st_mode & 07777);
	ck(after.st_uid == before.st_uid, "owner preserved");
}

/* A commit leaves no temporary files behind. */
static void
test_no_temp_files_left(void)
{
	DIR *d;
	struct dirent *de;
	int strays = 0;

	if ((d = opendir(fixture)) == NULL) {
		ck(false, "opendir fixture");
		return;
	}
	while ((de = readdir(d)) != NULL)
		if (de->d_name[0] == '.' && strstr(de->d_name, ".plist.") != NULL)
			strays++;
	closedir(d);
	ck(strays == 0, "no temporary files left behind, found %d", strays);
}

/*
 * Groups.plist is committed before Users.plist. Proven by making
 * Users.plist unwritable: the commit must fail, and Groups.plist must
 * already carry the change.
 */
static void
test_groups_committed_first(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	char upath[PATH_MAX];
	bool has;

	seed();
	fixture_path(upath, sizeof(upath), DS_USERS_PLIST);

	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open for order test");
	if (h == NULL)
		return;
	memset(&u, 0, sizeof(u));
	(void)strlcpy(u.username, "carol", sizeof(u.username));
	u.uid = 5002;
	u.gid = 5002;
	ck(ds_user_add(h, &u) == DS_OK, "stage carol");
	ck(ds_group_add_member(h, "admin", "carol") == DS_OK, "stage promote");

	/*
	 * Replace Users.plist with a directory: rename(2) over it fails
	 * with EISDIR, so the users half cannot land while the groups half
	 * can. Tests the ordering without needing to crash mid-commit.
	 */
	ck(unlink(upath) == 0, "remove the users fixture");
	ck(mkdir(upath, 0700) == 0, "block the users path with a directory");

	ck(ds_commit(h) == DS_EIO, "commit fails on the users half");
	ck(ds_dirty(h), "the handle stays dirty after a failed commit");
	ds_close(h);

	ck(rmdir(upath) == 0, "unblock the users path");

	/* Groups must already have the membership: it went first. */
	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "reopen after failure");
	if (h == NULL)
		return;
	ck(ds_group_has_member(h, "admin", "carol", &has) == DS_OK && has,
	    "Groups.plist was written before Users.plist");
	ck(ds_user_get(h, "carol", &u) == DS_ENOENT,
	    "Users.plist did not land, so carol does not exist");
	ds_close(h);
	/*
	 * And the interrupted state is the safe one: a membership naming a
	 * user who does not exist grants nothing.
	 */
}

/* Concurrent writers must serialise; none may lose another's record. */
static void
test_concurrent_writers(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	pid_t kids[6];
	size_t i;
	int status, ok = 0;

	seed();
	for (i = 0; i < 6; i++) {
		if ((kids[i] = fork()) == 0) {
			char name[DS_NAME_MAX];
			struct ds_handle *ch;
			struct ds_userrec cu;

			ds_set_dirs(fixture, NULL);
			(void)snprintf(name, sizeof(name), "kid%zu", i);
			if (ds_open(DS_LOCAL, DS_RDWR, &ch) != DS_OK)
				_exit(1);
			memset(&cu, 0, sizeof(cu));
			(void)strlcpy(cu.username, name, sizeof(cu.username));
			cu.uid = (uid_t)(7000 + i);
			cu.gid = (gid_t)(7000 + i);
			/*
			 * Hold the snapshot a moment before committing. With
			 * the lock this is harmless, because the writers
			 * serialise. Without it, every child would be
			 * committing a view taken before its siblings wrote,
			 * and records would be lost. The pause is what makes
			 * this test catch a missing lock every time instead
			 * of most of the time.
			 */
			usleep(40000 * ((unsigned)i + 1));
			if (ds_user_add(ch, &cu) != DS_OK ||
			    ds_commit(ch) != DS_OK) {
				ds_close(ch);
				_exit(1);
			}
			ds_close(ch);
			_exit(0);
		}
	}
	for (i = 0; i < 6; i++)
		if (waitpid(kids[i], &status, 0) > 0 && WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0)
			ok++;
	ck(ok == 6, "all six writers succeeded, got %d", ok);

	ck(ds_open(DS_LOCAL, DS_RDONLY, &h) == DS_OK, "reopen after the race");
	if (h == NULL)
		return;
	ck(ds_user_count(h) == 7,
	    "admin plus six children survived, got %zu", ds_user_count(h));
	for (i = 0; i < 6; i++) {
		char name[DS_NAME_MAX];

		(void)snprintf(name, sizeof(name), "kid%zu", i);
		ck(ds_user_get(h, name, &u) == DS_OK, "%s survived", name);
	}
	ds_close(h);
}

/* A file that is absent reads as empty, so a first write creates it. */
static void
test_absent_files(void)
{
	struct ds_handle *h;
	struct ds_userrec u;
	char p[PATH_MAX];
	struct stat st;

	fixture_path(p, sizeof(p), DS_USERS_PLIST);
	(void)unlink(p);
	fixture_path(p, sizeof(p), DS_GROUPS_PLIST);
	(void)unlink(p);

	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_OK, "open an empty domain");
	if (h == NULL)
		return;
	ck(ds_user_count(h) == 0, "no users, got %zu", ds_user_count(h));
	memset(&u, 0, sizeof(u));
	(void)strlcpy(u.username, "first", sizeof(u.username));
	u.uid = 5000;
	u.gid = 5000;
	ck(ds_user_add(h, &u) == DS_OK, "add into an empty domain");
	ck(ds_commit(h) == DS_OK, "commit creates the file");
	ds_close(h);

	fixture_path(p, sizeof(p), DS_USERS_PLIST);
	ck(stat(p, &st) == 0, "Users.plist was created");
	ck((st.st_mode & 07777) == 0644, "a new file is 0644, got 0%o",
	    st.st_mode & 07777);
	/* Groups had no change staged, so it must not have been created. */
	fixture_path(p, sizeof(p), DS_GROUPS_PLIST);
	ck(stat(p, &st) == -1, "an unchanged file is not written");
}

/* A file that is present but not a plist must not be silently replaced. */
static void
test_garbage_is_not_clobbered(void)
{
	struct ds_handle *h;
	char p[PATH_MAX], *back;
	size_t len;

	fixture_path(p, sizeof(p), DS_USERS_PLIST);
	write_file(p, "this is not a plist at all\n");
	ck(ds_open(DS_LOCAL, DS_RDWR, &h) == DS_EPARSE,
	    "a corrupt file is EPARSE, not an empty database");
	ck(h == NULL, "no handle is returned on a parse failure");
	back = slurp(p, &len);
	ck(back != NULL && strcmp(back, "this is not a plist at all\n") == 0,
	    "the corrupt file was left exactly as it was");
	free(back);
}

int
main(void)
{
	char tmpl[] = "/tmp/libds_test.XXXXXX";
	char cmd[PATH_MAX + 32];

	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return (2);
	}
	(void)strlcpy(fixture, tmpl, sizeof(fixture));
	ds_set_dirs(fixture, NULL);
	printf("fixture: %s\n", fixture);

	test_read_seeded();
	test_add_user_and_membership();
	test_nss_parser_agrees();
	test_modify_and_delete();
	test_name_validation();
	test_next_uid();
	test_xml_escaping_round_trip();
	test_mode_preserved();
	test_no_temp_files_left();
	test_groups_committed_first();
	test_concurrent_writers();
	test_absent_files();
	test_garbage_is_not_clobbered();

	(void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fixture);
	(void)system(cmd);

	printf("\n%d checks, %d failures\n", checks, failures);
	if (failures == 0)
		puts("LIBDS-OK: libds reads, writes and locks the plists");
	else
		puts("LIBDS-FAIL");
	return (failures == 0 ? 0 : 1);
}
