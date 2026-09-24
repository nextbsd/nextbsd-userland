/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
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
 * Static Bonjour service registrations held by the resident daemon
 * (nextbsd/nextbsd-userland#250, E18 U4).
 *
 * A DNS-SD registration lives only as long as the process that made it,
 * so a one-shot command such as `dscli promote` cannot announce a service
 * itself. Instead it drops a file into
 *
 *     /Local/Library/Preferences/mDNSResponder/Services/<name>.plist
 *
 * and mDNSResponder registers every file in that directory at start and
 * keeps the set in step with the directory afterwards: a new file
 * registers, a removed file deregisters, a changed file re-registers.
 * Avahi's /etc/avahi/services works the same way.
 *
 * Each file is an XML property list with a dictionary at the top:
 *
 *     Name    string   instance name (required)
 *     Type    string   service type, e.g. _nextbsd-ds._tcp (required)
 *     Port    integer  1..65535 (required)
 *     Domain  string   default "local."
 *     Host    string   target host, default this machine
 *     TXT     dict     string keys and values, published as key=value
 *
 * A malformed file is logged and skipped; it is never fatal. The
 * directory is watched with kqueue(2) (EVFILT_VNODE on the directory), a
 * file descriptor the daemon's own select(2) loop already knows how to
 * poll through mDNSPosixAddFDToEventLoop(). If the directory cannot be
 * opened at start, the daemon retries every 30 seconds from the main
 * loop. Registration goes through the engine's internal API
 * (mDNS_RegisterService), as Responder.c does, not the client library.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mDNSEmbeddedAPI.h"
#include "mDNSPosix.h"
#include "nbsd_plist.h"

#ifndef STATIC_SERVICES_DIR
#define STATIC_SERVICES_DIR "/Local/Library/Preferences/mDNSResponder/Services"
#endif

#define STATIC_MAX_FILE   (64 * 1024)
#define STATIC_RETRY_SECS 30

typedef struct StaticService StaticService;
struct StaticService
{
    StaticService   *next;
    char            *file;        // basename within the directory
    ino_t            ino;         // identity of the file that was registered
    struct timespec  mtim;
    off_t            size;
    mDNSBool         registered;  // an mDNS registration is outstanding
    mDNSBool         stale;       // unlinked from the list; free on MemFree
    char             name[MAX_ESCAPED_DOMAIN_LABEL];
    char             type[MAX_ESCAPED_DOMAIN_NAME];
    int              port;
    ServiceRecordSet srs;
};

// A file that could not be registered, with the identity it had then. It
// is retried only when it changes (a writer that creates the file and then
// fills it produces one directory event, at creation, when it is still
// empty), and logged once per identity, not on every rescan.
typedef struct SkippedFile SkippedFile;
struct SkippedFile
{
    SkippedFile     *next;
    char            *file;
    ino_t            ino;
    struct timespec  mtim;
    off_t            size;
};

static StaticService *gServices;
static SkippedFile   *gSkipped;
static mDNS          *gM;
static int            gDirFD = -1;
static int            gKQ    = -1;
static time_t         gNextRetry;
static time_t         gRescanAt;      // a rescan is due (0 = none)

#define STATIC_RESCAN_SECS 2

// ---- registration -----------------------------------------------------------

mDNSlocal void StaticRegistrationCallback(mDNS *const m, ServiceRecordSet *const srs, mStatus status)
{
    StaticService *s = (StaticService *)srs->ServiceContext;

    switch (status)
    {
    case mStatus_NoError:
        LogMsg("MDNS-STATIC: registered %s: \"%s\" %s port %d", s->file, s->name, s->type, s->port);
        break;

    case mStatus_NameConflict:
        // Pick the next unique name, as Responder.c and the client path do.
        LogMsg("MDNS-STATIC: name conflict for %s (\"%s\"); renaming", s->file, s->name);
        if (mDNS_RenameAndReregisterService(m, srs, mDNSNULL) != mStatus_NoError)
            LogMsg("MDNS-STATIC: rename failed for %s", s->file);
        break;

    case mStatus_MemFree:
        s->registered = mDNSfalse;
        if (s->stale)
        {
            LogMsg("MDNS-STATIC: withdrawn %s", s->file);
            free(s->file);
            free(s);
        }
        break;

    default:
        LogMsg("MDNS-STATIC: %s: registration status %d", s->file, (int)status);
        break;
    }
}

mDNSlocal int ReadWholeFile(int dirfd, const char *file, char **out, size_t *outlen)
{
    struct stat st;
    char *buf;
    ssize_t got;
    size_t have = 0;
    int fd;

    fd = openat(dirfd, file, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return -1;
    if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) || st.st_size > STATIC_MAX_FILE)
    {
        close(fd);
        return -1;
    }
    buf = malloc((size_t)st.st_size + 1);
    if (buf == NULL) { close(fd); return -1; }
    while (have < (size_t)st.st_size)
    {
        got = read(fd, buf + have, (size_t)st.st_size - have);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        have += (size_t)got;
    }
    close(fd);
    buf[have] = '\0';
    *out = buf;
    *outlen = have;
    return 0;
}

// Append "key=value" as one TXT string; -1 when it does not fit.
mDNSlocal int AppendTXT(mDNSu8 *txt, mDNSu16 *txtlen, mDNSu16 txtmax, const char *key, const char *value)
{
    size_t kl = strlen(key), vl = strlen(value), n = kl + 1 + vl;

    if (kl == 0 || n > 255 || *txtlen + 1 + n > txtmax) return -1;
    txt[*txtlen] = (mDNSu8)n;
    memcpy(txt + *txtlen + 1, key, kl);
    txt[*txtlen + 1 + kl] = '=';
    memcpy(txt + *txtlen + 1 + kl + 1, value, vl);
    *txtlen = (mDNSu16)(*txtlen + 1 + n);
    return 0;
}

// Parse the file and register it; returns the new entry or NULL.
mDNSlocal StaticService *RegisterFile(const char *file, const struct stat *st)
{
    StaticService *s = NULL;
    struct pl_node *root = NULL;
    const struct pl_node *txtdict;
    const char *name, *type, *domain, *host;
    char typebuf[MAX_ESCAPED_DOMAIN_NAME], dombuf[MAX_ESCAPED_DOMAIN_NAME], hostbuf[MAX_ESCAPED_DOMAIN_NAME];
    char *buf = NULL;
    size_t len, i, tl;
    long long port;
    domainlabel n;
    domainname t, d, h;
    mDNSu8 txt[StandardAuthRDSize];
    mDNSu16 txtlen = 0;
    mStatus err;

    if (ReadWholeFile(gDirFD, file, &buf, &len) == -1)
    {
        LogMsg("MDNS-STATIC: %s: cannot read: %s", file, strerror(errno));
        return NULL;
    }
    root = pl_parse(buf, len);
    free(buf);
    if (root == NULL || root->type != PL_DICT)
    {
        LogMsg("MDNS-STATIC: %s: not a plist dictionary; skipped", file);
        goto fail;
    }
    name = pl_dict_string(root, "Name");
    type = pl_dict_string(root, "Type");
    if (name == NULL || name[0] == '\0' || strlen(name) >= MAX_DOMAIN_LABEL)
    {
        LogMsg("MDNS-STATIC: %s: Name missing or too long; skipped", file);
        goto fail;
    }
    if (type == NULL || type[0] == '\0')
    {
        LogMsg("MDNS-STATIC: %s: Type missing; skipped", file);
        goto fail;
    }
    if (pl_dict_integer(root, "Port", &port) == -1 || port < 1 || port > 65535)
    {
        LogMsg("MDNS-STATIC: %s: Port missing or out of range; skipped", file);
        goto fail;
    }
    // A type is "_svc._tcp" or "_svc._udp"; a trailing dot is optional.
    tl = strlen(type);
    if (tl + 2 >= sizeof(typebuf) ||
        !((tl >= 5 && (strcmp(type + tl - 5, "._tcp") == 0 || strcmp(type + tl - 5, "._udp") == 0)) ||
          (tl >= 6 && (strcmp(type + tl - 6, "._tcp.") == 0 || strcmp(type + tl - 6, "._udp.") == 0))))
    {
        LogMsg("MDNS-STATIC: %s: Type \"%s\" is not _name._tcp or _name._udp; skipped", file, type);
        goto fail;
    }
    snprintf(typebuf, sizeof(typebuf), "%s%s", type, type[tl - 1] == '.' ? "" : ".");
    domain = pl_dict_string(root, "Domain");
    if (domain == NULL || domain[0] == '\0') domain = "local.";
    if (strlen(domain) + 2 >= sizeof(dombuf)) { LogMsg("MDNS-STATIC: %s: Domain too long; skipped", file); goto fail; }
    snprintf(dombuf, sizeof(dombuf), "%s%s", domain, domain[strlen(domain) - 1] == '.' ? "" : ".");
    host = pl_dict_string(root, "Host");
    if (host != NULL && host[0] != '\0')
    {
        if (strlen(host) + 2 >= sizeof(hostbuf)) { LogMsg("MDNS-STATIC: %s: Host too long; skipped", file); goto fail; }
        snprintf(hostbuf, sizeof(hostbuf), "%s%s", host, host[strlen(host) - 1] == '.' ? "" : ".");
    }
    else
        hostbuf[0] = '\0';

    txtdict = pl_dict_get(root, "TXT");
    if (txtdict != NULL)
    {
        if (txtdict->type != PL_DICT)
        {
            LogMsg("MDNS-STATIC: %s: TXT is not a dictionary; skipped", file);
            goto fail;
        }
        for (i = 0; i < txtdict->nchildren; i++)
        {
            const struct pl_node *kv = txtdict->children[i];
            if (kv->type != PL_STRING)
            {
                LogMsg("MDNS-STATIC: %s: TXT value for \"%s\" is not a string; skipped", file, kv->key);
                goto fail;
            }
            if (AppendTXT(txt, &txtlen, sizeof(txt), kv->key, kv->str) == -1)
            {
                LogMsg("MDNS-STATIC: %s: TXT record too long (limit %u bytes); skipped", file, (unsigned)sizeof(txt));
                goto fail;
            }
        }
    }

    if (!MakeDomainLabelFromLiteralString(&n, name) ||
        !MakeDomainNameFromDNSNameString(&t, typebuf) ||
        !MakeDomainNameFromDNSNameString(&d, dombuf) ||
        (hostbuf[0] != '\0' && !MakeDomainNameFromDNSNameString(&h, hostbuf)))
    {
        LogMsg("MDNS-STATIC: %s: bad Name, Type, Domain or Host; skipped", file);
        goto fail;
    }

    s = calloc(1, sizeof(*s));
    if (s == NULL) goto fail;
    s->file = strdup(file);
    if (s->file == NULL) goto fail;
    s->ino = st->st_ino;
    s->mtim = st->st_mtim;
    s->size = st->st_size;
    strlcpy(s->name, name, sizeof(s->name));
    strlcpy(s->type, typebuf, sizeof(s->type));
    s->port = (int)port;

    err = mDNS_RegisterService(gM, &s->srs, &n, &t, &d,
                               hostbuf[0] != '\0' ? &h : mDNSNULL, mDNSOpaque16fromIntVal((mDNSu16)port),
                               mDNSNULL, txtlen ? txt : mDNSNULL, txtlen,
                               mDNSNULL, 0, mDNSInterface_Any,
                               StaticRegistrationCallback, s, 0);
    if (err != mStatus_NoError)
    {
        LogMsg("MDNS-STATIC: %s: mDNS_RegisterService failed: %d", file, (int)err);
        goto fail;
    }
    s->registered = mDNStrue;
    pl_free(root);
    return s;

fail:
    if (s != NULL) { free(s->file); free(s); }
    pl_free(root);
    return NULL;
}

mDNSlocal void Withdraw(StaticService *s)
{
    StaticService **pp;

    for (pp = &gServices; *pp != NULL; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    s->next = NULL;
    if (s->registered)
    {
        s->stale = mDNStrue;                 // freed in the MemFree callback
        mDNS_DeregisterService(gM, &s->srs);
    }
    else
    {
        LogMsg("MDNS-STATIC: withdrawn %s", s->file);
        free(s->file);
        free(s);
    }
}

// ---- directory scan -----------------------------------------------------------

mDNSlocal mDNSBool IsServiceFile(const char *name)
{
    size_t n = strlen(name);
    return name[0] != '.' && n > 6 && strcmp(name + n - 6, ".plist") == 0;
}

mDNSlocal mDNSBool SameIdentity(ino_t ino, const struct timespec *mtim, off_t size, const struct stat *st)
{
    return ino == st->st_ino && size == st->st_size &&
           mtim->tv_sec == st->st_mtim.tv_sec && mtim->tv_nsec == st->st_mtim.tv_nsec;
}

mDNSlocal SkippedFile *FindSkipped(const char *file)
{
    SkippedFile *k;
    for (k = gSkipped; k != NULL; k = k->next)
        if (strcmp(k->file, file) == 0) return k;
    return NULL;
}

mDNSlocal void RememberSkipped(const char *file, const struct stat *st)
{
    SkippedFile *k = FindSkipped(file);
    if (k == NULL)
    {
        k = calloc(1, sizeof(*k));
        if (k == NULL) return;
        k->file = strdup(file);
        if (k->file == NULL) { free(k); return; }
        k->next = gSkipped;
        gSkipped = k;
    }
    k->ino = st->st_ino;
    k->mtim = st->st_mtim;
    k->size = st->st_size;
}

mDNSlocal void ForgetSkipped(const char *file)
{
    SkippedFile **pp, *k;
    for (pp = &gSkipped; (k = *pp) != NULL; pp = &k->next)
        if (strcmp(k->file, file) == 0)
        {
            *pp = k->next;
            free(k->file);
            free(k);
            return;
        }
}

// Drop skipped entries whose file is gone.
mDNSlocal void PruneSkipped(void)
{
    SkippedFile **pp, *k;
    struct stat st;
    pp = &gSkipped;
    while ((k = *pp) != NULL)
    {
        if (fstatat(gDirFD, k->file, &st, 0) == -1)
        {
            *pp = k->next;
            free(k->file);
            free(k);
        }
        else
            pp = &k->next;
    }
}

mDNSlocal void Rescan(void)
{
    DIR *dir;
    struct dirent *de;
    struct stat st;
    StaticService *s, *next, *seen = NULL, **tail;
    int fd;

    if (gDirFD == -1) return;
    fd = openat(gDirFD, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd == -1 || (dir = fdopendir(fd)) == NULL)
    {
        if (fd != -1) close(fd);
        LogMsg("MDNS-STATIC: cannot read %s: %s", STATIC_SERVICES_DIR, strerror(errno));
        return;
    }

    // Move every entry that is still present (and unchanged) to `seen`;
    // whatever is left in gServices afterwards has gone away.
    tail = &seen;
    gRescanAt = 0;
    while ((de = readdir(dir)) != NULL)
    {
        SkippedFile *k;

        if (!IsServiceFile(de->d_name)) continue;
        if (fstatat(gDirFD, de->d_name, &st, 0) == -1 || !S_ISREG(st.st_mode)) continue;
        for (s = gServices; s != NULL; s = s->next)
            if (strcmp(s->file, de->d_name) == 0) break;
        if (s != NULL)
        {
            if (SameIdentity(s->ino, &s->mtim, s->size, &st))
            {
                // unchanged: keep the registration
                StaticService **pp;
                for (pp = &gServices; *pp != s; pp = &(*pp)->next) ;
                *pp = s->next;
                s->next = NULL;
                *tail = s; tail = &s->next;
                continue;
            }
            LogMsg("MDNS-STATIC: %s changed; re-registering", s->file);
            Withdraw(s);
        }
        k = FindSkipped(de->d_name);
        if (k != NULL && SameIdentity(k->ino, &k->mtim, k->size, &st))
            continue;                         // already reported; unchanged
        s = RegisterFile(de->d_name, &st);
        if (s != NULL)
        {
            ForgetSkipped(de->d_name);
            *tail = s; tail = &s->next;
        }
        else
        {
            // Perhaps still being written: look again shortly. A file that
            // has not changed by then is left alone until it does.
            RememberSkipped(de->d_name, &st);
            gRescanAt = time(NULL) + STATIC_RESCAN_SECS;
        }
    }
    closedir(dir);

    for (s = gServices; s != NULL; s = next)
    {
        next = s->next;
        Withdraw(s);
    }
    gServices = seen;
    PruneSkipped();
}

// ---- watch --------------------------------------------------------------------

mDNSlocal void DisarmWatch(void)
{
    if (gKQ != -1)
    {
        mDNSPosixRemoveFDFromEventLoop(gKQ);
        close(gKQ);
        gKQ = -1;
    }
    if (gDirFD != -1)
    {
        close(gDirFD);
        gDirFD = -1;
    }
}

mDNSlocal void WatchCallback(int fd, void *context)
{
    struct kevent evs[16];
    struct timespec zero = { 0, 0 };
    int n, i, gone = 0;

    (void)context;
    n = kevent(fd, NULL, 0, evs, 16, &zero);
    for (i = 0; i < n; i++)
        if (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) gone = 1;
    if (gone)
    {
        // The directory itself went away: withdraw everything and retry later.
        StaticService *s, *next;
        LogMsg("MDNS-STATIC: %s removed; withdrawing all static services", STATIC_SERVICES_DIR);
        for (s = gServices; s != NULL; s = next) { next = s->next; Withdraw(s); }
        gServices = NULL;
        DisarmWatch();
        gNextRetry = time(NULL) + STATIC_RETRY_SECS;
        return;
    }
    if (n > 0) Rescan();
}

mDNSlocal int ArmWatch(void)
{
    struct kevent kev;

    gDirFD = open(STATIC_SERVICES_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (gDirFD == -1) return -1;
    gKQ = kqueue();
    if (gKQ == -1) { DisarmWatch(); return -1; }
    (void)fcntl(gKQ, F_SETFD, FD_CLOEXEC);
    EV_SET(&kev, gDirFD, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE, 0, NULL);
    if (kevent(gKQ, &kev, 1, NULL, 0, NULL) == -1) { DisarmWatch(); return -1; }
    if (mDNSPosixAddFDToEventLoop(gKQ, WatchCallback, NULL) != mStatus_NoError) { DisarmWatch(); return -1; }
    return 0;
}

// Create the directory chain at start (we are still root then), mode 0755,
// so dscli can drop files in and the daemon, later running as nobody, can
// read them.
mDNSlocal void EnsureDirectory(void)
{
    char path[sizeof(STATIC_SERVICES_DIR)];
    char *p;

    strlcpy(path, STATIC_SERVICES_DIR, sizeof(path));
    for (p = path + 1; *p != '\0'; p++)
    {
        if (*p != '/') continue;
        *p = '\0';
        (void)mkdir(path, 0755);
        *p = '/';
    }
    (void)mkdir(path, 0755);
}

mDNSexport void StaticServicesInit(mDNS *m)
{
    gM = m;
    EnsureDirectory();
    if (ArmWatch() == -1)
    {
        LogMsg("MDNS-STATIC: cannot watch %s (%s); retrying every %d s",
               STATIC_SERVICES_DIR, strerror(errno), STATIC_RETRY_SECS);
        gNextRetry = time(NULL) + STATIC_RETRY_SECS;
        return;
    }
    LogMsg("MDNS-STATIC-WATCH: watching %s", STATIC_SERVICES_DIR);
    Rescan();
}

// Called from the main loop. Re-arms the watch if it is down, and re-reads
// the directory when a file that failed to parse may have been completed.
mDNSexport void StaticServicesIdle(mDNS *m)
{
    (void)m;
    if (gM == NULL) return;
    if (gKQ != -1)
    {
        if (gRescanAt != 0 && time(NULL) >= gRescanAt) Rescan();
        return;
    }
    if (time(NULL) < gNextRetry) return;
    if (ArmWatch() == -1)
    {
        gNextRetry = time(NULL) + STATIC_RETRY_SECS;
        return;
    }
    LogMsg("MDNS-STATIC-WATCH: watching %s", STATIC_SERVICES_DIR);
    Rescan();
}
