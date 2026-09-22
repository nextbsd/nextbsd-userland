/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The NextBSD Project
 *
 * Host-side tests for libds. A temporary tree stands in for /Local,
 * /Network and the system paths (ds_set_dirs, ds_set_system_root), so
 * nothing outside it is touched and no privilege is needed.
 */

#include <sys/stat.h>

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"
#include "plist.h"

static int failures, checks;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

static char root[256], local[300], network[300], sysroot[300];

static void
mkpath(const char *rel)
{
	char p[600];

	snprintf(p, sizeof(p), "%s/%s", sysroot, rel);
	if (ds_mkdirs(p, 0755) == -1) {
		perror(p);
		exit(2);
	}
}

static void
put_file(const char *path, const char *text)
{
	FILE *fp = fopen(path, "w");

	if (fp == NULL) {
		perror(path);
		exit(2);
	}
	fputs(text, fp);
	fclose(fp);
}

static void
test_names_and_escape(void)
{
	char *e;

	CHECK(ds_valid_name("jane"));
	CHECK(ds_valid_name("jane.doe-2_x"));
	CHECK(!ds_valid_name(""));
	CHECK(!ds_valid_name("-x"));
	CHECK(!ds_valid_name("a b"));
	CHECK(!ds_valid_name("a:b"));
	CHECK(!ds_valid_name("123456789012345678901234567890123"));
	CHECK(ds_valid_name("12345678901234567890123456789012"));
	e = ds_xml_escape("a&b<c>d");
	CHECK(e != NULL && strcmp(e, "a&amp;b&lt;c&gt;d") == 0);
	free(e);
}

static void
test_records_roundtrip(void)
{
	struct ds_users us;
	struct ds_groups gs;
	struct ds_user *u;
	struct ds_group *g;
	char upath[600], gpath[600], *text;
	struct stat st;

	snprintf(upath, sizeof(upath), "%s/%s", local, DS_USERS_PLIST);
	snprintf(gpath, sizeof(gpath), "%s/%s", local, DS_GROUPS_PLIST);

	/* Missing files load as empty. */
	errno = 0;
	CHECK(ds_users_load(&us, upath) == 0 && us.n == 0 && errno == ENOENT);
	CHECK(ds_groups_load(&gs, gpath) == 0 && gs.n == 0);

	u = ds_user_add(&us, "zed");
	CHECK(u != NULL);
	u->uid = 1002; u->gid = 1002;
	u = ds_user_add(&us, "jane");
	CHECK(u != NULL);
	u->uid = 1001; u->gid = 1001;
	CHECK(ds_set_string(&u->real, "Jane & Co <j>") == 0);
	CHECK(ds_set_string(&u->shell, "/bin/sh") == 0);
	u->hash = strdup("$6$rounds=5000$abc$xyz");
	u = ds_user_add(&us, "guest");
	CHECK(u != NULL);
	u->uid = 1003; u->gid = 1003; u->nopass = true;

	g = ds_group_add(&gs, "admin", 5000);
	CHECK(g != NULL);
	CHECK(ds_group_add_member(g, "jane") == 0);
	CHECK(ds_group_add_member(g, "jane") == 0);	/* idempotent */
	CHECK(g->nmembers == 1);
	CHECK(ds_group_add_member(g, "zed") == 0);
	g = ds_group_add(&gs, "empty", 20);
	CHECK(g != NULL);

	CHECK(ds_users_save(&us, upath) == 0);
	CHECK(ds_groups_save(&gs, gpath) == 0);
	CHECK(stat(upath, &st) == 0 && (st.st_mode & 0777) == 0644);
	text = ds_read_file(upath, NULL);
	CHECK(text != NULL);
	if (text != NULL) {
		/* Sorted by name, Gershwin's keys, entities escaped. */
		CHECK(strstr(text, "<key>guest</key>") < strstr(text, "<key>jane</key>"));
		CHECK(strstr(text, "<key>jane</key>") < strstr(text, "<key>zed</key>"));
		CHECK(strstr(text, "<string>Jane &amp; Co &lt;j&gt;</string>") != NULL);
		CHECK(strstr(text, "<key>noPassword</key>\n\t\t<true/>") != NULL);
		CHECK(strstr(text, "<key>passwordHash</key>\n\t\t<string>$6$rounds=5000$abc$xyz</string>") != NULL);
		CHECK(strstr(text, "<key>username</key>\n\t\t<string>jane</string>") != NULL);
		CHECK(strstr(text, "DOCTYPE plist") != NULL);
		free(text);
	}
	text = ds_read_file(gpath, NULL);
	CHECK(text != NULL);
	if (text != NULL) {
		CHECK(strstr(text, "<key>members</key>\n\t\t<array>\n\t\t\t<string>jane</string>\n\t\t\t<string>zed</string>") != NULL);
		CHECK(strstr(text, "<key>members</key>\n\t\t<array/>") != NULL);
		CHECK(strstr(text, "<key>groupname</key>\n\t\t<string>admin</string>") != NULL);
		free(text);
	}
	ds_users_free(&us);
	ds_groups_free(&gs);

	/* Reload: everything survives, defaults apply. */
	CHECK(ds_users_load(&us, upath) == 0 && us.n == 3);
	CHECK(ds_groups_load(&gs, gpath) == 0 && gs.n == 2);
	u = ds_user_find(&us, "jane");
	CHECK(u != NULL && u->uid == 1001 && u->gid == 1001);
	CHECK(u != NULL && strcmp(u->real, "Jane & Co <j>") == 0);
	CHECK(u != NULL && strcmp(u->shell, "/bin/sh") == 0);
	CHECK(u != NULL && u->hash != NULL && strcmp(u->hash, "$6$rounds=5000$abc$xyz") == 0);
	u = ds_user_find(&us, "zed");
	CHECK(u != NULL && strcmp(u->shell, DS_DEFAULT_SHELL) == 0 && u->real[0] == '\0');
	u = ds_user_find(&us, "guest");
	CHECK(u != NULL && u->nopass && u->hash == NULL);
	CHECK(ds_user_find_uid(&us, 1003) == u);
	g = ds_group_find(&gs, "admin");
	CHECK(g != NULL && g->gid == 5000 && g->nmembers == 2 && ds_group_has_member(g, "zed"));
	CHECK(ds_group_find_gid(&gs, 20) != NULL);

	/* Edits: remove a user everywhere, drop a member, remove a group. */
	CHECK(ds_groups_drop_member(&gs, "zed") == 1);
	CHECK(g->nmembers == 1);
	CHECK(ds_group_remove_member(g, "zed") == -1);
	CHECK(ds_user_remove(&us, "zed") == 0 && us.n == 2);
	CHECK(ds_user_remove(&us, "zed") == -1);
	CHECK(ds_group_remove(&gs, "empty") == 0 && gs.n == 1);
	CHECK(ds_users_save(&us, upath) == 0);
	CHECK(ds_groups_save(&gs, gpath) == 0);

	/* Next free id skips both plists (getpwuid/getgrgid on the host are
	 * consulted too; 1001 and 1003 are taken by us, 1002 is free). */
	CHECK(ds_next_id(&us, &gs, 1001) == 1002 || getpwuid(1002) != NULL);
	ds_users_free(&us);
	ds_groups_free(&gs);

	/* A file that does not parse is refused, not silently emptied. */
	put_file(upath, "<plist><dict><key>x</key>");
	CHECK(ds_users_load(&us, upath) == -1 && errno == EINVAL);
	put_file(upath, "<plist><array/></plist>");
	CHECK(ds_users_load(&us, upath) == -1);
	ds_users_free(&us);
	(void)unlink(upath);
	(void)unlink(gpath);
}

static void
test_passwords(void)
{
	struct ds_user u;
	char *h1, *h2;

	memset(&u, 0, sizeof(u));
	h1 = ds_hash_password("secret");
	h2 = ds_hash_password("secret");
	CHECK(h1 != NULL && h2 != NULL);
	if (h1 == NULL || h2 == NULL)
		return;
#ifdef __FreeBSD__
	/* Darwin's crypt(3) has no SHA-512 and ignores the setting, so
	 * these two only hold where the hashes are real. */
	CHECK(strcmp(h1, h2) != 0);			/* random salt */
	CHECK(strncmp(h1, "$6$rounds=5000$", 15) == 0);	/* Gershwin's form */
	CHECK(strlen(h1) > 90);
#endif
	u.hash = h1;
	CHECK(ds_verify_password(&u, "secret"));
	CHECK(!ds_verify_password(&u, "Secret"));
	CHECK(!ds_verify_password(&u, ""));
	u.hash = NULL;
	CHECK(!ds_verify_password(&u, "secret"));	/* no hash: login disabled */
	u.nopass = true;
	CHECK(ds_verify_password(&u, ""));
	CHECK(!ds_verify_password(&u, "x"));
	free(h1);
	free(h2);
}

static void
test_files(void)
{
	char path[600], tmpglob[600], *text;
	struct stat st;
	int fd, fd2;

	/* Atomic write: content, mode, no temp file left behind. */
	snprintf(path, sizeof(path), "%s/atomic.txt", local);
	CHECK(ds_write_atomic(path, "hello\n", 6, 0600) == 0);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size == 6);
	CHECK(ds_write_atomic(path, "bye\n", 4, 0644) == 0);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strcmp(text, "bye\n") == 0);
	free(text);
	snprintf(tmpglob, sizeof(tmpglob), "%s/.atomic.txt.", local);
	CHECK(access(tmpglob, F_OK) == -1);
	(void)unlink(path);
	CHECK(ds_read_file(path, NULL) == NULL && errno == ENOENT);

	/* Locking: two locks on the same directory serialize (second is
	 * non-blocking here via a fresh fd + LOCK_NB semantics are not
	 * exposed; just check lock/unlock succeed and the file appears). */
	fd = ds_lock(local);
	CHECK(fd >= 0);
	snprintf(path, sizeof(path), "%s/%s", local, DS_LOCK_FILE);
	CHECK(stat(path, &st) == 0);
	ds_unlock(fd);
	fd2 = ds_lock(local);
	CHECK(fd2 >= 0);
	ds_unlock(fd2);

	/* mkdirs. */
	snprintf(path, sizeof(path), "%s/a/b/c", local);
	CHECK(ds_mkdirs(path, 0755) == 0 && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
	CHECK(ds_mkdirs(path, 0755) == 0);
	(void)rmdir(path);
	snprintf(path, sizeof(path), "%s/a/b", local); (void)rmdir(path);
	snprintf(path, sizeof(path), "%s/a", local); (void)rmdir(path);
}

static void
test_homes(void)
{
	char home[600], skel[600], f[600], *text;
	struct stat st;

	/* The skeleton lives under the (test) system root. */
	mkpath("usr/share/skel");
	snprintf(skel, sizeof(skel), "%s/usr/share/skel/dot.zshrc", sysroot);
	put_file(skel, "# zshrc\n");
	snprintf(skel, sizeof(skel), "%s/usr/share/skel/dot.profile", sysroot);
	put_file(skel, "# profile\n");
	mkpath("Local/Users");

	snprintf(home, sizeof(home), "%s%s/jane", sysroot, DS_LOCAL_USERS);
	CHECK(ds_make_home(home, getuid(), getgid()) == 0);
	CHECK(stat(home, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0755);
	snprintf(f, sizeof(f), "%s/.zshrc", home);
	text = ds_read_file(f, NULL);
	CHECK(text != NULL && strcmp(text, "# zshrc\n") == 0);
	free(text);
	snprintf(f, sizeof(f), "%s/.profile", home);
	CHECK(stat(f, &st) == 0);
	snprintf(f, sizeof(f), "%s/.zprofile", home);
	CHECK(stat(f, &st) == -1);		/* no skeleton for it: not created */

	/* Existing dot files are kept. */
	snprintf(f, sizeof(f), "%s/.zshrc", home);
	put_file(f, "mine\n");
	CHECK(ds_make_home(home, getuid(), getgid()) == 0);
	text = ds_read_file(f, NULL);
	CHECK(text != NULL && strcmp(text, "mine\n") == 0);
	free(text);

	/* Removal refuses anything outside /Local/Users. */
	CHECK(ds_remove_home("/tmp") == -1 && errno == EPERM);
	snprintf(f, sizeof(f), "%s%s", sysroot, DS_LOCAL_USERS);
	CHECK(ds_remove_home(f) == -1 && errno == EPERM);
	snprintf(f, sizeof(f), "%s%s/../etc", sysroot, DS_LOCAL_USERS);
	CHECK(ds_remove_home(f) == -1 && errno == EPERM);
	CHECK(ds_remove_home(home) == 0);
	CHECK(stat(home, &st) == -1);
	CHECK(ds_remove_home(home) == 0);	/* already gone is fine */
}

static void
test_network_files(void)
{
	char server[256], path[600], *text;
	struct pl_node *root;
	const struct pl_node *txt;
	long long port;

	mkpath("etc");
	CHECK(ds_role(server, sizeof(server)) == DS_STANDALONE);

	/* Binding plist. */
	CHECK(ds_binding_read(server, sizeof(server)) == -1);
	CHECK(ds_binding_write("server.local") == 0);
	CHECK(ds_binding_read(server, sizeof(server)) == 0 && strcmp(server, "server.local") == 0);
	CHECK(ds_role(server, sizeof(server)) == DS_JOINED && strcmp(server, "server.local") == 0);
	snprintf(path, sizeof(path), "%s/%s", local, DS_BINDING_PLIST);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strstr(text, "<key>server</key>\n\t<string>server.local</string>") != NULL);
	CHECK(text != NULL && strstr(text, "<key>version</key>\n\t<integer>1</integer>") != NULL);
	free(text);
	CHECK(ds_binding_remove() == 0);
	CHECK(ds_binding_remove() == 0);
	CHECK(ds_role(server, sizeof(server)) == DS_STANDALONE);

	/* Exports: exactly the contract, recognised as ours, removable. */
	CHECK(!ds_exports_ours());
	CHECK(ds_exports_write() == 0);
	snprintf(path, sizeof(path), "%s%s", sysroot, DS_EXPORTS);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strcmp(text,
	    "# Written by dscli promote; dscli demote removes this file.\n"
	    "/Network/Library/DirectoryServices -ro\n"
	    "/Local/Users\n") == 0);
	free(text);
	CHECK(ds_exports_ours());
	CHECK(ds_role(server, sizeof(server)) == DS_SERVER);
	put_file(path, "/export/foo -ro\n");
	CHECK(!ds_exports_ours());			/* an admin's file is not ours */
	CHECK(ds_exports_remove() == 0);
	CHECK(ds_exports_remove() == 0);

	/* Bonjour service file. */
	CHECK(ds_service_file_write("Office <Server>") == 0);
	snprintf(path, sizeof(path), "%s%s/%s", sysroot, DS_SERVICE_DIR, DS_SERVICE_FILE);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL);
	if (text != NULL) {
		root = pl_parse(text, strlen(text));
		CHECK(root != NULL);
		if (root != NULL) {
			CHECK(strcmp(pl_dict_string(root, "Name"), "Office <Server>") == 0);
			CHECK(strcmp(pl_dict_string(root, "Type"), "_nextbsd-ds._tcp") == 0);
			CHECK(pl_dict_integer(root, "Port", &port) == 0 && port == 2049);
			txt = pl_dict_get(root, "TXT");
			CHECK(txt != NULL && strcmp(pl_dict_string(txt, "path"), "/Network/Library/DirectoryServices") == 0);
			CHECK(txt != NULL && strcmp(pl_dict_string(txt, "v"), "1") == 0);
			CHECK(txt != NULL && strcmp(pl_dict_string(txt, "name"), "Office <Server>") == 0);
			pl_free(root);
		}
		free(text);
	}
	CHECK(ds_service_file_remove() == 0);
	CHECK(access(path, F_OK) == -1);

	/* ntp.conf: the managed block is replaced in place, created when
	 * absent, and everything around it is preserved. */
	snprintf(path, sizeof(path), "%s%s", sysroot, DS_NTP_CONF);
	put_file(path,
	    "pool 0.freebsd.pool.ntp.org iburst\n"
	    "# BEGIN dscli directory server (managed by dscli join and leave; do not edit)\n"
	    "# END dscli directory server\n"
	    "restrict default limited kod nomodify notrap noquery nopeer\n");
	CHECK(ds_ntp_set_server("server.local") == 0);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strcmp(text,
	    "pool 0.freebsd.pool.ntp.org iburst\n"
	    "# BEGIN dscli directory server (managed by dscli join and leave; do not edit)\n"
	    "server server.local prefer iburst\n"
	    "# END dscli directory server\n"
	    "restrict default limited kod nomodify notrap noquery nopeer\n") == 0);
	free(text);
	CHECK(ds_ntp_set_server("other.local") == 0);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strstr(text, "server other.local prefer iburst\n") != NULL);
	CHECK(text != NULL && strstr(text, "server.local prefer") == NULL);
	free(text);
	CHECK(ds_ntp_set_server(NULL) == 0);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strcmp(text,
	    "pool 0.freebsd.pool.ntp.org iburst\n"
	    "# BEGIN dscli directory server (managed by dscli join and leave; do not edit)\n"
	    "# END dscli directory server\n"
	    "restrict default limited kod nomodify notrap noquery nopeer\n") == 0);
	free(text);
	put_file(path, "pool 0.freebsd.pool.ntp.org iburst");	/* no block, no newline */
	CHECK(ds_ntp_set_server("s") == 0);
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strcmp(text,
	    "pool 0.freebsd.pool.ntp.org iburst\n\n"
	    "# BEGIN dscli directory server (managed by dscli join and leave; do not edit)\n"
	    "server s prefer iburst\n"
	    "# END dscli directory server\n") == 0);
	free(text);
	(void)unlink(path);
	CHECK(ds_ntp_set_server("s") == 0);		/* created from nothing */
	text = ds_read_file(path, NULL);
	CHECK(text != NULL && strstr(text, "server s prefer iburst\n") != NULL);
	free(text);
	(void)unlink(path);
}

static void
test_dir_choice(void)
{
	char path[600];

	CHECK(!ds_from_network());
	CHECK(strcmp(ds_dir(), local) == 0);
	snprintf(path, sizeof(path), "%s/%s", network, DS_USERS_PLIST);
	put_file(path, "<plist><dict/></plist>");
	CHECK(ds_from_network());
	CHECK(strcmp(ds_dir(), network) == 0);
	(void)unlink(path);
	CHECK(!ds_from_network());
}

int
main(void)
{
	char tmpl[] = "/tmp/ds_test.XXXXXX";

	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return (2);
	}
	snprintf(root, sizeof(root), "%s", tmpl);
	snprintf(sysroot, sizeof(sysroot), "%s/sys", root);
	snprintf(local, sizeof(local), "%s/Local", root);
	snprintf(network, sizeof(network), "%s/Network", root);
	if (mkdir(sysroot, 0755) == -1 || mkdir(local, 0755) == -1 ||
	    mkdir(network, 0755) == -1) {
		perror("mkdir");
		return (2);
	}
	ds_set_dirs(local, network);
	ds_set_system_root(sysroot);

	test_names_and_escape();
	test_records_roundtrip();
	test_passwords();
	test_files();
	test_homes();
	test_network_files();
	test_dir_choice();

	printf("%s: %d checks, %d failures (tree: %s)\n",
	    failures ? "FAILED" : "OK", checks, failures, root);
	if (!failures) {
		char cmd[400];
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
		(void)system(cmd);
	}
	return (failures ? 1 : 0);
}
