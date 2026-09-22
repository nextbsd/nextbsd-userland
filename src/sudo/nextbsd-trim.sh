#!/bin/sh
# nextbsd-trim.sh <darwin-sudo-checkout> <dest>   (dest: src/sudo/dist)
# Copy only what building sudo and visudo needs out of Darwin's sudo
# (apple-oss-distributions/sudo), and drop the configure registrations for the
# directories left out. Re-run on each Darwin update.
set -eu
SRC=${1:?usage: nextbsd-trim.sh <darwin-sudo-checkout> <dest>}/sudo
DEST=${2:?usage: nextbsd-trim.sh <darwin-sudo-checkout> <dest>}
mkdir -p "$DEST"; DEST=$(cd "$DEST" && pwd)
find "$DEST" -mindepth 1 -delete
cd "$SRC"
{
    # configure and its inputs
    printf '%s\n' LICENSE.md configure config.h.in pathnames.h.in \
        scripts/config.guess scripts/config.sub scripts/install-sh \
        scripts/ltmain.sh scripts/mkinstalldirs \
        docs/Makefile.in docs/sudo.mdoc.in docs/visudo.mdoc.in
    # headers, and the directories sudo and visudo compile (no tests, no po)
    find include lib/util lib/eventlog lib/iolog lib/protobuf-c plugins/sudoers src \
        -type f ! -path '*/regress/*' ! -path '*/po/*'
} | while IFS= read -r f; do
    mkdir -p "$DEST/$(dirname "$f")"
    cp -p "$f" "$DEST/$f"
done
# configure: register only the files for the directories kept above.
keep='docs/Makefile|include/Makefile|lib/eventlog/Makefile|lib/iolog/Makefile|lib/protobuf-c/Makefile|lib/util/Makefile|lib/util/util.exp|plugins/sudoers/Makefile|src/Makefile|src/intercept.exp|src/sudo_usage.h'
awk -v keep="^($keep)\$" '
    /^[ \t]*ac_config_files="\$ac_config_files [^"]*"[ \t]*$/ {
        s = $0; sub(/.*\$ac_config_files /, "", s); sub(/".*/, "", s)
        n = split(s, f, " "); out = ""
        for (i = 1; i <= n; i++) if (f[i] ~ keep) out = out " " f[i]
        if (out == "") next
        match($0, /^[ \t]*/); print substr($0, 1, RLENGTH) "ac_config_files=\"$ac_config_files" out "\""
        next
    }
    { print }' configure > "$DEST/configure"
chmod 0755 "$DEST/configure"
