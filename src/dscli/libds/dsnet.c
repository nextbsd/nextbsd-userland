/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
 * Ported from Gershwin's dscli by Joe Maloney
 * (gershwin-desktop/gershwin-components, DirectoryServices/dscli, BSD-2-Clause).
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
 * The files that promote, join and leave own: the binding plist, the
 * exports, the Bonjour service file, and ntp.conf's managed block. Their
 * exact contents are the contracts shared with the LaunchDaemons (E18
 * U5, U6) and mDNSResponder's service files (U4).
 */

#include <sys/stat.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"
#include "plist.h"

#define EXPORTS_HEADER	"# Written by dscli promote; dscli demote removes this file.\n"
#define NTP_BEGIN	"# BEGIN dscli directory server (managed by dscli join and leave; do not edit)"
#define NTP_END		"# END dscli directory server"

/* ---- binding ------------------------------------------------------------- */

static int
binding_path(char *buf, size_t len)
{
	return (ds_path(buf, len, ds_local_dir(), DS_BINDING_PLIST));
}

int
ds_binding_read(char *server, size_t len)
{
	char path[PATH_MAX];
	char *buf;
	size_t blen;
	struct pl_node *root;
	const char *s;

	if (binding_path(path, sizeof(path)) == -1)
		return (-1);
	buf = ds_read_file(path, &blen);
	if (buf == NULL)
		return (-1);
	root = pl_parse(buf, blen);
	free(buf);
	if (root == NULL) {
		errno = EINVAL;
		return (-1);
	}
	s = pl_dict_string(root, "server");
	if (s == NULL || s[0] == '\0' || strlen(s) >= len) {
		pl_free(root);
		errno = EINVAL;
		return (-1);
	}
	strcpy(server, s);
	pl_free(root);
	return (0);
}

int
ds_binding_write(const char *server)
{
	char path[PATH_MAX], *esc, *doc;
	int rv;

	if (ds_mkdirs(ds_local_dir(), 0755) == -1 ||
	    binding_path(path, sizeof(path)) == -1)
		return (-1);
	esc = ds_xml_escape(server);
	if (esc == NULL)
		return (-1);
	rv = asprintf(&doc,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n<dict>\n"
	    "\t<key>server</key>\n\t<string>%s</string>\n"
	    "\t<key>version</key>\n\t<integer>1</integer>\n"
	    "</dict>\n</plist>\n", esc);
	free(esc);
	if (rv == -1)
		return (-1);
	rv = ds_write_atomic(path, doc, strlen(doc), 0644);
	free(doc);
	return (rv);
}

int
ds_binding_remove(void)
{
	char path[PATH_MAX];

	if (binding_path(path, sizeof(path)) == -1)
		return (-1);
	if (unlink(path) == -1 && errno != ENOENT)
		return (-1);
	return (0);
}

/* ---- exports ------------------------------------------------------------- */

/*
 * One line per filesystem: the kernel takes a single default export per
 * filesystem (a second line for the same one fails with EPERM, whatever
 * its options), and both directories normally sit on /. So the exported
 * directories are grouped by st_dev and each group is one line listing
 * all of them. No -ro: root maps to nobody, and the plists are root-owned,
 * so clients cannot alter them. A directory that does not exist yet is
 * grouped with the root filesystem.
 */
int
ds_exports_write(void)
{
	static const char *const dirs[] = { DS_LOCAL_USERS, DS_NETWORK_DIR };
	char path[PATH_MAX], sys[PATH_MAX], text[1024];
	struct stat st;
	dev_t dev[2];
	size_t i, j, n;
	int done[2] = { 0, 0 };

	for (i = 0; i < 2; i++) {
		ds_syspath(sys, sizeof(sys), dirs[i]);
		if (stat(sys, &st) == -1) {
			ds_syspath(sys, sizeof(sys), "/");
			if (stat(sys, &st) == -1)
				return (-1);
		}
		dev[i] = st.st_dev;
	}
	n = strlcpy(text, EXPORTS_HEADER, sizeof(text));
	for (i = 0; i < 2; i++) {
		if (done[i])
			continue;
		n = strlcat(text, dirs[i], sizeof(text));
		done[i] = 1;
		for (j = i + 1; j < 2; j++) {
			if (done[j] || dev[j] != dev[i])
				continue;
			(void)strlcat(text, " ", sizeof(text));
			n = strlcat(text, dirs[j], sizeof(text));
			done[j] = 1;
		}
		n = strlcat(text, "\n", sizeof(text));
	}
	if (n >= sizeof(text)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	ds_syspath(path, sizeof(path), DS_EXPORTS);
	return (ds_write_atomic(path, text, n, 0644));
}

bool
ds_exports_ours(void)
{
	char path[PATH_MAX], *buf;
	bool ours;

	ds_syspath(path, sizeof(path), DS_EXPORTS);
	buf = ds_read_file(path, NULL);
	if (buf == NULL)
		return (false);
	ours = strncmp(buf, EXPORTS_HEADER, sizeof(EXPORTS_HEADER) - 1) == 0;
	free(buf);
	return (ours);
}

int
ds_exports_remove(void)
{
	char path[PATH_MAX];

	ds_syspath(path, sizeof(path), DS_EXPORTS);
	if (unlink(path) == -1 && errno != ENOENT)
		return (-1);
	return (0);
}

/* ---- Bonjour service file ------------------------------------------------ */

int
ds_service_file_write(const char *display_name)
{
	char dir[PATH_MAX], path[PATH_MAX], *esc, *doc;
	int rv;

	ds_syspath(dir, sizeof(dir), DS_SERVICE_DIR);
	if (ds_mkdirs(dir, 0755) == -1 ||
	    ds_path(path, sizeof(path), dir, DS_SERVICE_FILE) == -1)
		return (-1);
	esc = ds_xml_escape(display_name);
	if (esc == NULL)
		return (-1);
	rv = asprintf(&doc,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n<dict>\n"
	    "\t<key>Name</key>\n\t<string>%s</string>\n"
	    "\t<key>Type</key>\n\t<string>%s</string>\n"
	    "\t<key>Port</key>\n\t<integer>%d</integer>\n"
	    "\t<key>TXT</key>\n\t<dict>\n"
	    "\t\t<key>path</key>\n\t\t<string>%s</string>\n"
	    "\t\t<key>v</key>\n\t\t<string>1</string>\n"
	    "\t\t<key>name</key>\n\t\t<string>%s</string>\n"
	    "\t</dict>\n"
	    "</dict>\n</plist>\n",
	    esc, DS_SERVICE_TYPE, DS_SERVICE_PORT, DS_NETWORK_DIR, esc);
	free(esc);
	if (rv == -1)
		return (-1);
	rv = ds_write_atomic(path, doc, strlen(doc), 0644);
	free(doc);
	return (rv);
}

int
ds_service_file_remove(void)
{
	char dir[PATH_MAX], path[PATH_MAX];

	ds_syspath(dir, sizeof(dir), DS_SERVICE_DIR);
	if (ds_path(path, sizeof(path), dir, DS_SERVICE_FILE) == -1)
		return (-1);
	if (unlink(path) == -1 && errno != ENOENT)
		return (-1);
	return (0);
}

/* ---- ntp.conf ------------------------------------------------------------ */

/*
 * Replace whatever sits between the BEGIN and END markers with one server
 * line (or nothing). A file without the markers gets them appended, so
 * join works on any ntp.conf; a missing file is created with just the
 * block.
 */
int
ds_ntp_set_server(const char *host)
{
	char path[PATH_MAX], *buf, *begin, *end, *out, *block;
	size_t len, headlen, taillen, blen;
	int rv;

	ds_syspath(path, sizeof(path), DS_NTP_CONF);
	buf = ds_read_file(path, &len);
	if (buf == NULL) {
		if (errno != ENOENT)
			return (-1);
		buf = strdup("");
		len = 0;
		if (buf == NULL)
			return (-1);
	}
	if (host != NULL)
		rv = asprintf(&block, "%s\nserver %s prefer iburst\n%s\n",
		    NTP_BEGIN, host, NTP_END);
	else
		rv = asprintf(&block, "%s\n%s\n", NTP_BEGIN, NTP_END);
	if (rv == -1) {
		free(buf);
		return (-1);
	}
	blen = strlen(block);
	begin = strstr(buf, NTP_BEGIN);
	end = begin != NULL ? strstr(begin, NTP_END) : NULL;
	if (begin != NULL && end != NULL) {
		end += strlen(NTP_END);
		if (*end == '\n')
			end++;
		headlen = (size_t)(begin - buf);
		taillen = len - (size_t)(end - buf);
		out = malloc(headlen + blen + taillen + 1);
		if (out == NULL)
			goto fail;
		memcpy(out, buf, headlen);
		memcpy(out + headlen, block, blen);
		memcpy(out + headlen + blen, end, taillen);
		out[headlen + blen + taillen] = '\0';
	} else {
		int nl = (len > 0 && buf[len - 1] != '\n');
		out = malloc(len + nl + 1 + blen + 1);
		if (out == NULL)
			goto fail;
		memcpy(out, buf, len);
		if (nl)
			out[len] = '\n';
		out[len + nl] = '\n';
		memcpy(out + len + nl + 1, block, blen + 1);
	}
	free(buf);
	free(block);
	rv = ds_write_atomic(path, out, strlen(out), 0644);
	free(out);
	return (rv);
fail:
	free(buf);
	free(block);
	return (-1);
}

/* ---- role ---------------------------------------------------------------- */

enum ds_role
ds_role(char *server, size_t len)
{
	char local[256];

	if (server == NULL || len == 0) {
		server = local;
		len = sizeof(local);
	}
	server[0] = '\0';
	if (ds_binding_read(server, len) == 0)
		return (DS_JOINED);
	if (ds_exports_ours())
		return (DS_SERVER);
	return (DS_STANDALONE);
}
