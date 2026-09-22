/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Joseph Maloney
 *
 * Host-side tests for the plist reader and the database layer. Fixtures
 * are written into a temporary directory tree that stands in for /Local
 * and /Network; nothing here touches nsswitch.
 */

#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dsdb.h"
#include "plist.h"

static int failures;
static int checks;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

static char root[256];
static char local[300], network[300], etcgroup[300];

static void
write_file(const char *dir, const char *name, const char *text)
{
	char path[600];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "w");
	if (fp == NULL) {
		perror(path);
		exit(2);
	}
	fputs(text, fp);
	fclose(fp);
}

/* Replace atomically the way dscli does: temp file + rename(2). */
static void
replace_file(const char *dir, const char *name, const char *text)
{
	char from[600], to[600];

	snprintf(from, sizeof(from), "%s/.%s.tmp", dir, name);
	snprintf(to, sizeof(to), "%s/%s", dir, name);
	write_file(dir, from + strlen(dir) + 1, text);
	if (rename(from, to) == -1) {
		perror("rename");
		exit(2);
	}
}

static void
remove_file(const char *dir, const char *name)
{
	char path[600];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	(void)unlink(path);
}

static const char users_v1[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
"<plist version=\"1.0\">\n"
"<dict>\n"
"  <!-- a comment between entries -->\n"
"  <key>jane</key>\n"
"  <dict>\n"
"    <key>username</key><string>jane</string>\n"
"    <key>uid</key><integer>1001</integer>\n"
"    <key>gid</key><integer>1001</integer>\n"
"    <key>realName</key><string>Jane &amp; Co &lt;j&gt; &#233;</string>\n"
"    <key>shell</key><string>/bin/zsh</string>\n"
"    <key>passwordHash</key><string>$6$rounds=5000$saltsalt$hashhash</string>\n"
"  </dict>\n"
"  <key>guest</key>\n"
"  <dict>\n"
"    <key>username</key><string>guest</string>\n"
"    <key>uid</key><string>1002</string>\n"
"    <key>gid</key><string>1002</string>\n"
"    <key>noPassword</key><true/>\n"
"  </dict>\n"
"  <key>bare</key>\n"
"  <dict>\n"
"    <key>noPassword</key><string>YES</string>\n"
"  </dict>\n"
"  <key>junk</key><string>not a record</string>\n"
"  <key></key><dict><key>uid</key><integer>7</integer></dict>\n"
"</dict>\n"
"</plist>\n";

static const char groups_v1[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<plist version=\"1.0\">\n"
"<dict>\n"
"  <key>jane</key>\n"
"  <dict>\n"
"    <key>groupname</key><string>jane</string>\n"
"    <key>gid</key><integer>1001</integer>\n"
"    <key>members</key><array><string>jane</string></array>\n"
"  </dict>\n"
"  <key>admin</key>\n"
"  <dict>\n"
"    <key>groupname</key><string>admin</string>\n"
"    <key>gid</key><integer>5000</integer>\n"
"    <key>members</key><array><string>jane</string><string>root</string></array>\n"
"  </dict>\n"
"  <key>staff</key>\n"
"  <dict>\n"
"    <key>gid</key><integer>20</integer>\n"
"    <key>members</key><array><string>jane</string><string>guest</string><string>jane</string></array>\n"
"  </dict>\n"
"  <key>empty</key>\n"
"  <dict>\n"
"    <key>gid</key><integer>30</integer>\n"
"  </dict>\n"
"</dict>\n"
"</plist>\n";

static const char etc_group_text[] =
"# $FreeBSD$\n"
"wheel:*:0:root\n"
"audio:*:1201:\n"
"video:*:1202:jane\n"
"operator:*:5:root\n";

static void
test_plist_reader(void)
{
	struct pl_node *root_node;
	const struct pl_node *rec, *mem;
	long long v;
	int b;

	root_node = pl_parse(users_v1, strlen(users_v1));
	CHECK(root_node != NULL);
	if (root_node == NULL)
		return;
	CHECK(root_node->type == PL_DICT);
	CHECK(root_node->nchildren == 5);
	rec = pl_dict_get(root_node, "jane");
	CHECK(rec != NULL && rec->type == PL_DICT);
	CHECK(pl_dict_integer(rec, "uid", &v) == 0 && v == 1001);
	CHECK(strcmp(pl_dict_string(rec, "realName"), "Jane & Co <j> \xc3\xa9") == 0);
	CHECK(pl_dict_bool(rec, "noPassword", &b) == -1);
	rec = pl_dict_get(root_node, "guest");
	CHECK(pl_dict_integer(rec, "uid", &v) == 0 && v == 1002);	/* string form */
	CHECK(pl_dict_bool(rec, "noPassword", &b) == 0 && b == 1);
	rec = pl_dict_get(root_node, "bare");
	CHECK(pl_dict_bool(rec, "noPassword", &b) == 0 && b == 1);	/* "YES" */
	pl_free(root_node);

	root_node = pl_parse(groups_v1, strlen(groups_v1));
	CHECK(root_node != NULL);
	if (root_node == NULL)
		return;
	mem = pl_dict_get(pl_dict_get(root_node, "admin"), "members");
	CHECK(mem != NULL && mem->type == PL_ARRAY && mem->nchildren == 2);
	pl_free(root_node);

	/* Malformed documents are rejected, not half-loaded. */
	CHECK(pl_parse("<plist><dict><key>a</key></dict></plist>", 41) == NULL);
	CHECK(pl_parse("<plist><dict><key>a</key><string>x</dict></plist>", 49) == NULL);
	CHECK(pl_parse("", 0) == NULL);
	CHECK(pl_parse("<plist><array><integer>12x</integer></array></plist>", 52) == NULL);
	root_node = pl_parse("<plist version=\"1.0\"><dict/></plist>", 37);
	CHECK(root_node != NULL && root_node->type == PL_DICT && root_node->nchildren == 0);
	pl_free(root_node);
}

static void
test_users(void)
{
	const struct ds_user *u;
	struct passwd pwd;
	char buf[512];
	int rv;

	dsdb_lock();
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_user_count() == 3);		/* junk and the empty key are skipped */
	CHECK(dsdb_users_from_network() == 0);

	u = dsdb_user_by_name("jane");
	CHECK(u != NULL);
	if (u != NULL) {
		CHECK(u->uid == 1001 && u->gid == 1001);
		CHECK(strcmp(u->shell, "/bin/zsh") == 0);
		CHECK(strcmp(u->real, "Jane & Co <j> \xc3\xa9") == 0);
		CHECK(u->hash != NULL && strncmp(u->hash, "$6$", 3) == 0);
		CHECK(u->nopass == 0);
	}
	CHECK(dsdb_user_by_uid(1002) != NULL && strcmp(dsdb_user_by_uid(1002)->name, "guest") == 0);
	u = dsdb_user_by_name("bare");
	CHECK(u != NULL);
	if (u != NULL) {
		CHECK(u->uid == DS_NOBODY_ID && u->gid == DS_NOBODY_ID);
		CHECK(strcmp(u->shell, DS_DEFAULT_SHELL) == 0);
		CHECK(u->real[0] == '\0' && u->hash == NULL && u->nopass == 1);
	}
	CHECK(dsdb_user_by_name("nobody-here") == NULL);
	CHECK(dsdb_user_by_uid(7) == NULL);

	/* Enumeration follows file order. */
	CHECK(dsdb_user_at(0) != NULL && strcmp(dsdb_user_at(0)->name, "jane") == 0);
	CHECK(dsdb_user_at(2) != NULL && strcmp(dsdb_user_at(2)->name, "bare") == 0);
	CHECK(dsdb_user_at(3) == NULL);

	/* Packing: privileged callers see the hash, others "*". */
	u = dsdb_user_by_name("jane");
	rv = dsdb_pack_passwd(u, 1, 0, &pwd, buf, sizeof(buf));
	CHECK(rv == 0);
	CHECK(strcmp(pwd.pw_name, "jane") == 0);
	CHECK(strncmp(pwd.pw_passwd, "$6$", 3) == 0);
	CHECK(strcmp(pwd.pw_dir, "/Local/Users/jane") == 0);
	CHECK(strcmp(pwd.pw_shell, "/bin/zsh") == 0);
	CHECK(strcmp(pwd.pw_class, "") == 0);
	CHECK(pwd.pw_uid == 1001 && pwd.pw_gid == 1001);
	rv = dsdb_pack_passwd(u, 0, 1, &pwd, buf, sizeof(buf));
	CHECK(rv == 0 && strcmp(pwd.pw_passwd, "*") == 0);
	CHECK(strcmp(pwd.pw_dir, "/Network/Users/jane") == 0);
	u = dsdb_user_by_name("guest");
	rv = dsdb_pack_passwd(u, 1, 0, &pwd, buf, sizeof(buf));
	CHECK(rv == 0 && strcmp(pwd.pw_passwd, "") == 0);	/* noPassword */
	rv = dsdb_pack_passwd(u, 0, 0, &pwd, buf, sizeof(buf));
	CHECK(rv == 0 && strcmp(pwd.pw_passwd, "*") == 0);

	/* Buffer sizing: too small is ERANGE, the exact size fits. */
	u = dsdb_user_by_name("jane");
	CHECK(dsdb_pack_passwd(u, 0, 0, &pwd, buf, 8) == ERANGE);
	{
		size_t need = strlen("jane") + 1 + strlen("*") + 1 + 1 +
		    strlen(u->real) + 1 + strlen("/Local/Users/jane") + 1 +
		    strlen("/bin/zsh") + 1;
		CHECK(dsdb_pack_passwd(u, 0, 0, &pwd, buf, need) == 0);
		CHECK(dsdb_pack_passwd(u, 0, 0, &pwd, buf, need - 1) == ERANGE);
	}
	dsdb_unlock();
}

static void
test_groups(void)
{
	const struct ds_group *g;
	struct group grp;
	char buf[512];
	size_t need;

	dsdb_lock();
	CHECK(dsdb_refresh_groups() == DSDB_OK);
	CHECK(dsdb_group_count() == 4);
	g = dsdb_group_by_name("admin");
	CHECK(g != NULL && g->gid == 5000 && g->nmembers == 2);
	g = dsdb_group_by_gid(20);
	CHECK(g != NULL && strcmp(g->name, "staff") == 0);	/* name from the key */
	g = dsdb_group_by_name("empty");
	CHECK(g != NULL && g->nmembers == 0);
	CHECK(dsdb_group_by_name("wheel") == NULL);

	g = dsdb_group_by_name("admin");
	CHECK(dsdb_pack_group(g, &grp, buf, sizeof(buf)) == 0);
	CHECK(strcmp(grp.gr_name, "admin") == 0);
	CHECK(strcmp(grp.gr_passwd, "x") == 0);
	CHECK(grp.gr_gid == 5000);
	CHECK(grp.gr_mem != NULL && grp.gr_mem[0] != NULL &&
	    strcmp(grp.gr_mem[0], "jane") == 0 &&
	    strcmp(grp.gr_mem[1], "root") == 0 && grp.gr_mem[2] == NULL);
	CHECK(((uintptr_t)grp.gr_mem % sizeof(char *)) == 0);
	g = dsdb_group_by_name("empty");
	CHECK(dsdb_pack_group(g, &grp, buf, sizeof(buf)) == 0);
	CHECK(grp.gr_mem != NULL && grp.gr_mem[0] == NULL);

	/* ERANGE: pointer array alone, then strings, must each fit. */
	g = dsdb_group_by_name("admin");
	CHECK(dsdb_pack_group(g, &grp, buf, 3 * sizeof(char *)) == ERANGE);
	need = 3 * sizeof(char *) + strlen("admin") + 1 + 2 + strlen("jane") + 1 +
	    strlen("root") + 1;
	/* buf is 16-byte aligned from the compiler, so no alignment slack */
	CHECK(dsdb_pack_group(g, &grp, buf, need) == 0);
	CHECK(dsdb_pack_group(g, &grp, buf, need - 1) == ERANGE);
	dsdb_unlock();
}

static void
test_membership(void)
{
	gid_t gids[16];
	int cnt, i, have;

	dsdb_lock();
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_refresh_groups() == DSDB_OK);

	/* jane: primary 1001, jane(1001) dedup, admin 5000, staff 20, wheel 0,
	 * audio 1201 and video 1202 from /etc/group. */
	CHECK(dsdb_membership("jane", 1001, gids, 16, &cnt, etcgroup) == 0);
	CHECK(cnt == 6);
	CHECK(gids[0] == 1001);
	for (i = 0, have = 0; i < cnt && i < 16; i++)
		if (gids[i] == 5000 || gids[i] == 20 || gids[i] == 0 ||
		    gids[i] == 1201 || gids[i] == 1202)
			have++;
	CHECK(have == 5);
	for (i = 1; i < cnt && i < 16; i++)
		CHECK(gids[i] != 1001);

	/* guest is not an admin: staff, hardware groups, no wheel. */
	CHECK(dsdb_membership("guest", 1002, gids, 16, &cnt, etcgroup) == 0);
	CHECK(cnt == 4);
	for (i = 0; i < cnt; i++)
		CHECK(gids[i] != 0 && gids[i] != 5000);

	/* The caller's primary gid comes first even when it differs. */
	CHECK(dsdb_membership("guest", 77, gids, 16, &cnt, etcgroup) == 0);
	CHECK(cnt == 5 && gids[0] == 77 && gids[1] == 1002);

	/* Sizing call: maxgrp 0 still reports the full count. */
	CHECK(dsdb_membership("jane", 1001, NULL, 0, &cnt, etcgroup) == 0);
	CHECK(cnt == 6);
	/* Partial: two slots filled, count still full. */
	CHECK(dsdb_membership("jane", 1001, gids, 2, &cnt, etcgroup) == 0);
	CHECK(cnt == 6 && gids[0] == 1001);

	/* No /etc/group: only plist groups and wheel. */
	CHECK(dsdb_membership("jane", 1001, gids, 16, &cnt, NULL) == 0);
	CHECK(cnt == 4);

	CHECK(dsdb_membership("stranger", 1, gids, 16, &cnt, etcgroup) == -1);
	dsdb_unlock();
}

static void
bump_mtime(const char *dir, const char *name, int seconds)
{
	char path[600];
	struct stat st;
	struct timeval tv[2];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (stat(path, &st) == -1) {
		perror(path);
		exit(2);
	}
	tv[0].tv_sec = st.st_atime;
	tv[0].tv_usec = 0;
	tv[1].tv_sec = st.st_mtime + seconds;
	tv[1].tv_usec = 0;
	if (utimes(path, tv) == -1) {
		perror("utimes");
		exit(2);
	}
}

static void
test_cache_and_switching(void)
{
	const struct ds_user *u;

	dsdb_lock();
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_user_by_name("newbie") == NULL);

	/* A rewrite in place with a later mtime is picked up. */
	write_file(local, DS_USERS_PLIST,
	    "<plist><dict><key>newbie</key><dict><key>uid</key><integer>2000</integer></dict></dict></plist>");
	bump_mtime(local, DS_USERS_PLIST, 5);
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_user_count() == 1);
	CHECK(dsdb_user_by_name("newbie") != NULL);

	/* dscli's rename(2) replaces the inode: picked up even with the
	 * same second of mtime. */
	replace_file(local, DS_USERS_PLIST,
	    "<plist><dict><key>renamed</key><dict><key>uid</key><integer>2001</integer></dict></dict></plist>");
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_user_by_name("renamed") != NULL);
	CHECK(dsdb_user_by_name("newbie") == NULL);

	/* /Network takes over as soon as its file exists ... */
	write_file(network, DS_USERS_PLIST,
	    "<plist><dict><key>netuser</key><dict><key>uid</key><integer>3000</integer></dict></dict></plist>");
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_users_from_network() == 1);
	CHECK(dsdb_user_by_name("netuser") != NULL);
	CHECK(dsdb_user_by_name("renamed") == NULL);
	/* ... and groups are chosen independently: still local. */
	CHECK(dsdb_refresh_groups() == DSDB_OK);
	CHECK(dsdb_group_by_name("admin") != NULL);

	/* ... and /Local is back when it goes away. */
	remove_file(network, DS_USERS_PLIST);
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_users_from_network() == 0);
	CHECK(dsdb_user_by_name("renamed") != NULL);

	/* Unparsable: unavailable, and the stale records are not served. */
	write_file(local, DS_USERS_PLIST, "<plist><dict><key>x</key>");
	bump_mtime(local, DS_USERS_PLIST, 10);
	CHECK(dsdb_refresh_users() == DSDB_UNAVAIL);
	CHECK(dsdb_user_count() == 0);
	u = dsdb_user_by_name("renamed");
	CHECK(u == NULL);

	/* Missing: unavailable. Restored: fine again. */
	remove_file(local, DS_USERS_PLIST);
	CHECK(dsdb_refresh_users() == DSDB_UNAVAIL);
	write_file(local, DS_USERS_PLIST, users_v1);
	CHECK(dsdb_refresh_users() == DSDB_OK);
	CHECK(dsdb_user_by_name("jane") != NULL);
	dsdb_unlock();
}

int
main(void)
{
	char tmpl[] = "/tmp/dsdb_test.XXXXXX";

	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return (2);
	}
	snprintf(root, sizeof(root), "%s", tmpl);
	snprintf(local, sizeof(local), "%s/Local", root);
	snprintf(network, sizeof(network), "%s/Network", root);
	snprintf(etcgroup, sizeof(etcgroup), "%s/group", root);
	if (mkdir(local, 0755) == -1 || mkdir(network, 0755) == -1) {
		perror("mkdir");
		return (2);
	}
	write_file(local, DS_USERS_PLIST, users_v1);
	write_file(local, DS_GROUPS_PLIST, groups_v1);
	write_file(root, "group", etc_group_text);
	dsdb_set_dirs(local, network);

	test_plist_reader();
	test_users();
	test_groups();
	test_membership();
	test_cache_and_switching();

	remove_file(local, DS_USERS_PLIST);
	remove_file(local, DS_GROUPS_PLIST);
	remove_file(root, "group");
	(void)rmdir(local);
	(void)rmdir(network);
	(void)rmdir(root);

	printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK",
	    checks, failures);
	return (failures ? 1 : 0);
}
