/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The NextBSD Project
 *
 * Host-side tests for the account-tool routing in libds (dsroute.c): which
 * database an account is in, where a new one goes, the system-file reads
 * and the /etc/shells check. Fixtures stand in for /Local, /Network and
 * /etc under a temporary root.
 */

#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"

static int failures, checks;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

static void
write_file(const char *path, const char *text)
{
	FILE *fp = fopen(path, "w");

	if (fp == NULL) {
		perror(path);
		exit(2);
	}
	fputs(text, fp);
	fclose(fp);
}

int
main(void)
{
	char root[] = "/tmp/route_test.XXXXXX";
	char local[300], network[300], etc[300], path[400], name[64];
	uid_t uid;
	gid_t gid;

	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return (2);
	}
	snprintf(local, sizeof(local), "%s/Local", root);
	snprintf(network, sizeof(network), "%s/Network", root);
	snprintf(etc, sizeof(etc), "%s/etc", root);
	mkdir(local, 0755);
	mkdir(network, 0755);
	mkdir(etc, 0755);
	ds_set_dirs(local, network);
	ds_set_system_root(root);

	snprintf(path, sizeof(path), "%s/Users.plist", local);
	write_file(path,
	    "<plist><dict><key>jane</key><dict><key>username</key><string>jane</string>"
	    "<key>uid</key><integer>1001</integer><key>gid</key><integer>1001</integer>"
	    "</dict></dict></plist>\n");
	snprintf(path, sizeof(path), "%s/Groups.plist", local);
	write_file(path,
	    "<plist><dict><key>jane</key><dict><key>gid</key><integer>1001</integer></dict>"
	    "<key>admin</key><dict><key>gid</key><integer>5000</integer>"
	    "<key>members</key><array><string>jane</string></array></dict></dict></plist>\n");
	snprintf(path, sizeof(path), "%s/master.passwd", etc);
	write_file(path,
	    "# comment\n"
	    "root:$6$x$y:0:0::0:0:Charlie &:/root:/bin/sh\n"
	    "_svc:*:123:123::0:0:Service:/nonexistent:/usr/sbin/nologin\n"
	    "nobody:*:65534:65534::0:0:Unprivileged user:/nonexistent:/usr/sbin/nologin\n");
	snprintf(path, sizeof(path), "%s/passwd", etc);
	write_file(path,
	    "root:*:0:0:Charlie &:/root:/bin/sh\n"
	    "_svc:*:123:123:Service:/nonexistent:/usr/sbin/nologin\n");
	snprintf(path, sizeof(path), "%s/group", etc);
	write_file(path, "wheel:*:0:root\n_svc:*:123:\noperator:*:5:root\n");
	snprintf(path, sizeof(path), "%s/shells", etc);
	write_file(path, "# shells\n/bin/sh\n/bin/csh\n  /bin/zsh  \n");

	/* Existing accounts are found where they are. */
	CHECK(ds_route_user("jane") == DS_WHERE_PLIST);
	CHECK(ds_route_user("root") == DS_WHERE_SYSTEM);
	CHECK(ds_route_user("_svc") == DS_WHERE_SYSTEM);
	CHECK(ds_route_user("nobody") == DS_WHERE_SYSTEM);
	CHECK(ds_route_user("stranger") == DS_WHERE_NONE);
	CHECK(ds_route_uid(1001, name, sizeof(name)) == DS_WHERE_PLIST && strcmp(name, "jane") == 0);
	CHECK(ds_route_uid(0, name, sizeof(name)) == DS_WHERE_SYSTEM && strcmp(name, "root") == 0);
	CHECK(ds_route_uid(4242, name, sizeof(name)) == DS_WHERE_NONE);
	CHECK(ds_route_group("admin") == DS_WHERE_PLIST);
	CHECK(ds_route_group("wheel") == DS_WHERE_SYSTEM);
	CHECK(ds_route_group("nogroup") == DS_WHERE_NONE);
	CHECK(ds_route_gid(5000, name, sizeof(name)) == DS_WHERE_PLIST && strcmp(name, "admin") == 0);
	CHECK(ds_route_gid(5, name, sizeof(name)) == DS_WHERE_SYSTEM && strcmp(name, "operator") == 0);

	/* The system files, read directly. */
	CHECK(ds_system_user("_svc", 0, name, sizeof(name), &uid) == 0 && uid == 123);
	CHECK(ds_system_user(NULL, 65534, name, sizeof(name), &uid) == 0 && strcmp(name, "nobody") == 0);
	CHECK(ds_system_user("jane", 0, NULL, 0, NULL) == -1);
	CHECK(ds_system_group("wheel", 0, NULL, 0, &gid) == 0 && gid == 0);
	CHECK(ds_system_group(NULL, 123, name, sizeof(name), &gid) == 0 && strcmp(name, "_svc") == 0);

	/* Without master.passwd (unprivileged), passwd answers. */
	snprintf(path, sizeof(path), "%s/master.passwd", etc);
	chmod(path, 0);
	if (geteuid() != 0) {
		CHECK(ds_system_user("root", 0, NULL, 0, &uid) == 0 && uid == 0);
		CHECK(ds_route_user("_svc") == DS_WHERE_SYSTEM);
	}
	chmod(path, 0600);

	/* New accounts: the routing rule. */
	CHECK(ds_route_new_user(true, 499, NULL, NULL) == DS_WHERE_SYSTEM);
	CHECK(ds_route_new_user(true, 500, NULL, NULL) == DS_WHERE_PLIST);
	CHECK(ds_route_new_user(false, 0, "/usr/sbin/nologin", NULL) == DS_WHERE_SYSTEM);
	CHECK(ds_route_new_user(false, 0, "/usr/sbin/nologin", "/nonexistent") == DS_WHERE_SYSTEM);
	CHECK(ds_route_new_user(false, 0, "/usr/sbin/nologin", "/Local/Users/x") == DS_WHERE_PLIST);
	CHECK(ds_route_new_user(false, 0, "/bin/zsh", NULL) == DS_WHERE_PLIST);
	CHECK(ds_route_new_user(false, 0, NULL, NULL) == DS_WHERE_PLIST);
	CHECK(ds_route_new_user(true, 1001, "/usr/sbin/nologin", NULL) == DS_WHERE_SYSTEM);
	CHECK(ds_route_new_group(true, 80, "www") == DS_WHERE_SYSTEM);
	CHECK(ds_route_new_group(true, 5001, "staff") == DS_WHERE_PLIST);
	CHECK(ds_route_new_group(false, 0, "_svc2") == DS_WHERE_SYSTEM);
	CHECK(ds_route_new_group(false, 0, "staff") == DS_WHERE_PLIST);

	/* /Network takes precedence for the plist side, as for the NSS module. */
	snprintf(path, sizeof(path), "%s/Users.plist", network);
	write_file(path,
	    "<plist><dict><key>netjane</key><dict><key>uid</key><integer>2001</integer></dict></dict></plist>\n");
	CHECK(ds_route_user("netjane") == DS_WHERE_PLIST);
	CHECK(ds_route_user("jane") == DS_WHERE_NONE);
	unlink(path);
	CHECK(ds_route_user("jane") == DS_WHERE_PLIST);

	/* Shells. */
	CHECK(ds_shell_listed("/bin/sh"));
	CHECK(ds_shell_listed("/bin/zsh"));
	CHECK(!ds_shell_listed("/bin/fish"));
	CHECK(!ds_shell_listed("shells"));

	/* Tool resolution: nothing under the test root, and never ourselves. */
	CHECK(ds_bsd_tool("pw", "/usr/sbin/pw", path, sizeof(path)) == -1);
	snprintf(path, sizeof(path), "%s/usr/libexec/bsd", root);
	CHECK(ds_mkdirs(path, 0755) == 0);
	snprintf(path, sizeof(path), "%s/usr/libexec/bsd/pw", root);
	write_file(path, "#!/bin/sh\n");
	chmod(path, 0555);
	CHECK(ds_bsd_tool("pw", "/usr/sbin/pw", name, sizeof(name)) == 0 || errno == ENAMETOOLONG);
	{
		char got[400];

		CHECK(ds_bsd_tool("pw", "/usr/sbin/pw", got, sizeof(got)) == 0 &&
		    strcmp(got, path) == 0);
	}

	printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK",
	    checks, failures);
	return (failures ? 1 : 0);
}
