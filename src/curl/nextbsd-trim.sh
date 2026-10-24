#!/bin/sh
# nextbsd-trim.sh <apple-curl-tag-archive.tar.gz> <dest>   (dest: src/curl)
#
# Vendor Apple's curl (apple-oss-distributions/curl) into src/curl: verify the
# pinned archive, then copy only what the NextBSD build and install use into
# <dest>/dist, and Apple's build metadata (for reference) into <dest>/apple.
# The copied files are byte-for-byte Apple's; local changes live in
# <dest>/patches and are applied at build time (build-userland.sh).
#
# Re-run on each Apple update after changing TAG and SHA256 below, and
# re-check every patch in patches/series (see VENDORED.md, "Updating").
set -eu

TAG=curl-166.0.0.0.1
# SHA-256 of https://github.com/apple-oss-distributions/curl/archive/refs/tags/$TAG.tar.gz
SHA256=73d054ab722ddf9c0b9d63b5636a8a2ad99bc659370d553aaa64d323746f2108

ARCHIVE=${1:?usage: nextbsd-trim.sh <$TAG.tar.gz> <dest>}
DEST=${2:?usage: nextbsd-trim.sh <$TAG.tar.gz> <dest>}

if command -v sha256sum >/dev/null 2>&1; then
    got=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
else
    got=$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)
fi
[ "$got" = "$SHA256" ] || { echo "SHA-256 mismatch for $ARCHIVE: $got" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
tar -C "$TMP" -xzf "$ARCHIVE"
TOP="$TMP/curl-$TAG"
[ -d "$TOP/curl" ] || { echo "unexpected archive layout" >&2; exit 1; }

mkdir -p "$DEST"
DEST=$(cd "$DEST" && pwd)
rm -rf "$DEST/dist" "$DEST/apple"
mkdir -p "$DEST/dist" "$DEST/apple"

# Apple's own files: the version/modification record and the macOS build
# configuration NextBSD's CMake options are derived from.
(cd "$TOP" && cp -p curl.plist README.APPLE "$DEST/apple/")
mkdir -p "$DEST/apple/config_mac" "$DEST/apple/xcconfigs"
cp -p "$TOP/config_mac/curl_config.h" "$DEST/apple/config_mac/"
cp -p "$TOP/xcconfigs/common.xcconfig" "$DEST/apple/xcconfigs/"

# The curl 8.7.1 tree: CMake build files, headers, libcurl, the curl tool,
# the pkg-config/curl-config templates and the pre-built man pages.
# Left out: the test suite, autotools, Windows/OS2/Plan 9/VMS project files,
# packaging, examples and the Markdown documentation sources.
cd "$TOP/curl"
{
    printf '%s\n' COPYING README RELEASE-NOTES CMakeLists.txt \
        curl-config.in libcurl.pc.in libcurl.def \
        docs/curl.1 docs/curl-config.1
    find CMake include lib src -type f
    find docs/libcurl -type f -name '*.3'
} | sort | while IFS= read -r f; do
    mkdir -p "$DEST/dist/$(dirname "$f")"
    cp -p "$f" "$DEST/dist/$f"
done
echo "vendored $TAG into $DEST/dist ($(find "$DEST/dist" -type f | wc -l | tr -d ' ') files)"
