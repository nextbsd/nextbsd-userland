# sudo on NextBSD

**Source:** Darwin's sudo, `apple-oss-distributions/sudo` at tag `sudo-114.100.11`
(upstream 1.9.17p2 plus Darwin's patches). Darwin's patches are all under
`__APPLE__*` guards, so they compile out here. The licence is ISC-style; see
`dist/LICENSE.md`.

**Only what `sudo` and `visudo` need is vendored**, in `dist/`: 369 of Darwin's
1,399 files. `nextbsd-trim.sh` produces it from a Darwin checkout. It copies:
- `configure`, `config.h.in`, `pathnames.h.in` and the libtool helpers in
  `scripts/`;
- `include/`;
- `lib/util`, `lib/eventlog`, `lib/iolog`, `lib/protobuf-c`, `plugins/sudoers`
  and `src`, without their `regress/` tests or `po/` translations;
- from `docs/`, only the `sudo` and `visudo` man page sources;
- `LICENSE.md`.

Everything else is left out: the log server, the other plugins, the Python
plugin, examples, translations, tests, and Darwin's Xcode project and
pre-generated macOS `config.h`.

The one edit to Darwin's files is in `dist/configure`. `nextbsd-trim.sh`
removes the `ac_config_files` registrations for the directories that were left
out, so `configure` doesn't look for their `Makefile.in`. Darwin doesn't enable
NLS either (`HAVE_LIBINTL_H` is undefined in its `config.h`).

**Build:** `build-userland.sh` runs `dist/configure` in cross mode
(`--host=<arch>-unknown-freebsd`, the cross clang with `--sysroot`), out of
tree. Four `configure` checks try to run a test program, which can't be done
when cross compiling, so their answers are supplied as cache variables. The
values are FreeBSD facts: `sudo_cv_func_fnmatch`, `sudo_cv_working_pie`,
`ac_cv_have_working_snprintf` and `ac_cv_have_working_vsnprintf` are all `yes`.
`sudo_cv_var_mantype=mdoc` pins the man page format to what FreeBSD's mandoc
reads.

It then builds only `lib/util`, `lib/eventlog`, `lib/iolog`, `lib/protobuf-c`,
the `sudoers` policy (linked into `sudo`: `--enable-static-sudoers`), `visudo`,
`sudo` and their two man pages.

**Shipped, as on Darwin:**

| Path | Mode |
|---|---|
| `/usr/bin/sudo` | 4511, setuid root (re-applied in CI after staging and after the image `chown`) |
| `/usr/sbin/visudo` | 0111 |
| `/usr/share/man/man8/sudo.8`, `visudo.8` | 0444 |

`libsudo_util` is static (`--disable-shared-libutil`), so there is no
`/usr/libexec/sudo`. The following are also off:
- noexec and intercept;
- the log server and client, and with them OpenSSL;
- NLS;
- sendmail.

PAM goes through `/etc/pam.d/sudo`. Compression uses the base `libz`.
`/etc/sudoers` (Darwin's `files/sudoers`, without `lecture_file`) comes from
nextbsd/nextbsd-overlays; the package ships no `/etc`.

**Updating:**
1. Check out a newer `apple-oss-distributions/sudo` tag.
2. Run `./nextbsd-trim.sh <checkout> dist`.
3. Update the tag above and rebuild. If `configure` fails on a missing
   `Makefile.in`, extend the `keep` list in `nextbsd-trim.sh`.

Tracking issue: nextbsd/nextbsd-userland#247.
