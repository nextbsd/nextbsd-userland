# Vendored: Apple curl (curl + libcurl)

**Source:** [apple-oss-distributions/curl](https://github.com/apple-oss-distributions/curl),
tag **`curl-166.0.0.0.1`** (commit `5bd44caa14c16b8fcc2274537b0f51884b23af36`).
Apple's `curl.plist` gives the upstream version: **curl 8.7.1** (2024-03-27), imported by Apple
from `curl-8.7.1.tar.xz`, SHA-256
`6fea2aac6a4610fbd0400afb0bcddbe7258a64c63f1f68e5855ebc0c659710cd`.

**Pin:** `nextbsd-trim.sh` takes the tag archive
`https://github.com/apple-oss-distributions/curl/archive/refs/tags/curl-166.0.0.0.1.tar.gz`
and refuses it unless its SHA-256 is
`73d054ab722ddf9c0b9d63b5636a8a2ad99bc659370d553aaa64d323746f2108`.

**License:** the curl license (MIT-style), `dist/COPYING`. Apple's `curl.plist` calls it MIT.

**Plan:** E24 decision D4
([wiki](https://github.com/nextbsd/nextbsd-userland/wiki/E24-GNUstep-Gershwin-Requirements-Direction)),
nextbsd/nextbsd-userland#444. libcurl is used by gnustep-base (`NSURLSession`), Gershwin
components and git (`NextBSD-toolchain`).

## Layout

| Path | What |
|---|---|
| `dist/` | Apple's `curl/` subtree, **unmodified**, trimmed by `nextbsd-trim.sh` to what the build and install use: `CMakeLists.txt`, `CMake/`, `include/`, `lib/`, `src/`, `curl-config.in`, `libcurl.pc.in`, `libcurl.def`, `COPYING`, `README`, `RELEASE-NOTES`, and the pre-built man pages (`docs/curl.1`, `docs/curl-config.1`, `docs/libcurl/**/*.3`). Left out: the test suite, autotools, Windows/OS/2/Plan 9/VMS projects, packaging, examples and the Markdown doc sources |
| `apple/` | Apple's build metadata, for reference only: `curl.plist` (version and Apple's list of modifications), `README.APPLE`, `config_mac/curl_config.h`, `xcconfigs/common.xcconfig` |
| `patches/` | NextBSD's patch series (`series` gives the order), applied by `build-userland.sh` to a copy of `dist/` at build time. Every patch is a security fix; each header names the CVE, the upstream commit and where the backport came from |
| `nextbsd-trim.sh` | Re-creates `dist/` and `apple/` from the pinned archive |

### What Apple changed in curl 8.7.1

`diff -r` against the 8.7.1 tarball shows 12 changed files in `include/` and `lib/` (Apple's
`curl.plist` lists them):
- **Secure Transport and the Darwin TLS choice** (`vtls/sectransp.c`, `vtls/vtls.c`,
  `vtls/openssl.c`): all under `__APPLE__` / `TARGET_OS_OSX`, so they compile out on NextBSD.
- `include/curl/curl.h`: `CURLE_SSL_CACERT` is an enum value again rather than a macro (same
  value, 60).
- `lib/curl_gssapi.c`: alignment of the `gss_OID_desc` structs.
- **Security backports:** CVE-2024-7264 (`vtls/x509asn1.c`), CVE-2024-9681 (`hsts.c`),
  CVE-2025-9086 (`cookie.c`), CVE-2025-14524 (`curl_sasl.c`), CVE-2025-14017 (`ldap.c`),
  CVE-2025-14819 (`vtls/openssl.c`), and a `qsort()` fix in `http_aws_sigv4.c`.

## Build (`build-userland.sh`, "curl + libcurl")

CMake, cross-built on the runner with `cmake/cross-<arch>.cmake` against the base sysroot,
like the other CMake components. Nothing links nextbsd-userland libraries.

| | Apple (macOS) | NextBSD |
|---|---|---|
| TLS | LibreSSL + Secure Transport, chosen at run time | **base OpenSSL 3.5.6** only (`libssl.so.35`, `libcrypto.so.35`). Secure Transport does not exist on NextBSD |
| zlib | system | base zlib 1.3.2 |
| GSS-API / Kerberos | GSS.framework | base MIT Kerberos 1.22 (`libgssapi_krb5`): SPNEGO/Negotiate, Kerberos FTP, SASL GSSAPI |
| HTTP/2 | nghttp2 (private copy) | **off**: no nghttp2 in base, and no new third-party dependency |
| libpsl, libidn2, libssh2, brotli, zstd | off | off (not in base) |
| LDAP | system OpenLDAP | **off**: base has no public OpenLDAP |
| Resolver | threaded | threaded |
| WebSockets | off (experimental in 8.7.1) | off |
| CA bundle | `/etc/ssl/cert.pem` | `/etc/ssl/cert.pem` (below) |
| Built-in manual (`curl -M`) | yes | no: it is generated with Perl at build time |

Protocols: DICT, FILE, FTP(S), GOPHER(S), HTTP(S), IMAP(S), IPFS/IPNS, MQTT, POP3(S), RTSP,
SMB(S), SMTP(S), TELNET, TFTP. Features: SSL, IPv6, UnixSockets, libz, AsynchDNS, GSS-API,
SPNEGO, Kerberos, NTLM, TLS-SRP, HTTPS-proxy, alt-svc, HSTS.

### Installed files

| Path | |
|---|---|
| `/usr/bin/curl` | the tool |
| `/usr/lib/libcurl.so.4` (`libcurl.so.4.8.0`, `libcurl.so`) | SONAME `libcurl.so.4`, as every other libcurl 8 build |
| `/usr/include/curl/*.h` | public headers |
| `/usr/libdata/pkgconfig/libcurl.pc` | where base keeps its `.pc` files (`openssl.pc`, `zlib.pc`, `mit-krb5-gssapi.pc`) |
| `/usr/bin/curl-config`, `/usr/lib/cmake/CURL/` | for builds that use them instead of pkg-config |
| `/usr/share/man/man1/curl.1.gz`, `curl-config.1.gz`, `/usr/share/man/man3/*.3.gz` | Apple's pre-built pages |

No static library. No daemon, no launchd job, nothing printed at boot.

## CA certificates

curl reads **`/etc/ssl/cert.pem`** (`CURL_CA_BUNDLE`), with no CA directory. That is the path
Apple's curl uses, OpenSSL's default certificate file (base OpenSSL's `OPENSSLDIR` is
`/etc/ssl`), and the bundle base `certctl rehash` writes. On NextBSD `/etc` is the
`/private/etc` symlink, so the file is `/private/etc/ssl/cert.pem`.

Who writes it: the **image assembly**, from base's trust store in `/usr/share/certs/trusted`.
nextbsd-ci's `assemble-image.sh` concatenates the trusted certificates into
`/private/etc/ssl/cert.pem` (what certctl's bundle step does), and nextbsd's `build.sh` runs
`certctl rehash` on the new root. On a running system `certctl rehash` regenerates it.
nextbsd-overlays seeds no certificates and needs none: a bundle copied from the overlay
would go stale when base's trust store changes. If the file is missing, HTTPS transfers fail
with `error setting certificate file: /etc/ssl/cert.pem` (curl error 77) instead of
connecting unverified; `--cacert`/`CURLOPT_CAINFO` and `CURL_CA_BUNDLE` in the environment
still override it.

## Security fixes since curl 8.7.1 (CVE check)

Checked on 2026-10-24 against curl's advisory data (`https://curl.se/docs/vuln.json`, latest
release curl 8.23.0 of 2026-10-14). **53 advisories** cover curl 8.7.1. Of these, **28 are
fixed by the patches here**, **5 are already fixed in Apple's tree**, and **20 do not apply
to this build**. Two more advisories (CVE-2026-8926, CVE-2026-87862) were introduced by fixes
for older CVEs that this tree carries, so they are patched too: 30 CVEs in 35 patches.

Most backports come from Ubuntu's curl 8.5.0 security updates for 24.04 LTS
(`8.5.0-2ubuntu10.16`, `debian/patches/`), whose code is close to 8.7.1. Where Ubuntu's
version was wrong or incomplete it was corrected here, as each patch header says (TFTP,
CVE-2026-8286, CVE-2026-8927). Four are NextBSD's own, because the upstream fix depends on
refactoring 8.7.1 does not have: CVE-2026-8458, CVE-2026-19931, CVE-2026-8926 (and
CVE-2026-92386 is upstream's one-line change).

| CVE | Severity | Issue | Result |
|---|---|---|---|
| CVE-2024-6197 | Medium | freeing stack buffer in utf8asn1str | N/A: `vtls/x509asn1.c` is compiled only for GnuTLS, wolfSSL, Schannel and Secure Transport, not OpenSSL |
| CVE-2024-7264 | Low | ASN.1 date parser overread | Apple (and compiled out, as above) |
| CVE-2024-8096 | Medium | OCSP stapling bypass with GnuTLS | N/A: GnuTLS only |
| CVE-2024-9681 | Low | HSTS subdomain overwrites parent cache entry | Apple |
| CVE-2024-11053 | Low | netrc and redirect credential leak | patches 0001, 0002 |
| CVE-2025-0167 | Low | netrc and default credential leak | patch 0007 |
| CVE-2025-0725 | Low | gzip integer overflow | N/A: only with zlib older than 1.2.0.4; base has 1.3.2 |
| CVE-2025-5025 | Medium | no QUIC certificate pinning with wolfSSL | N/A: wolfSSL/QUIC only |
| CVE-2025-10966 | Low | missing SFTP host verification with wolfSSH | N/A: no SSH backend |
| CVE-2025-14017 | Medium | broken TLS options for threaded LDAPS | Apple (and LDAP is off) |
| CVE-2025-14524 | Low | bearer token leak on cross-protocol redirect | Apple |
| CVE-2025-14819 | Low | OpenSSL partial chain store policy bypass | Apple |
| CVE-2025-15079 | Low | libssh global known_hosts override | N/A: no SSH backend |
| CVE-2025-15224 | Low | libssh key passphrase bypass | N/A: no SSH backend |
| CVE-2026-1965 | Medium | bad reuse of HTTP Negotiate connection | patches 0003, 0004 |
| CVE-2026-3783 | Medium | token leak with redirect and netrc | patch 0005 |
| CVE-2026-3784 | Low | wrong proxy connection reuse with credentials | patch 0006 |
| CVE-2026-4873 | Low | connection reuse ignores TLS requirement | patch 0008 |
| CVE-2026-5545 | Medium | wrong reuse of HTTP Negotiate connection | patch 0009 |
| CVE-2026-5773 | Low | wrong reuse of SMB connection | patch 0010 |
| CVE-2026-6253 | Medium | proxy credentials leak over redirect-to proxy | patch 0011 |
| CVE-2026-6276 | Low | stale custom cookie host causes cookie leak | patch 0012 |
| CVE-2026-6429 | Medium | netrc credential leak with reused proxy connection | patches 0013 to 0015 |
| CVE-2026-7168 | Medium | cross-proxy Digest auth state leak | patch 0016 |
| CVE-2026-8286 | Low | wrong STARTTLS connection reuse | patch 0017 |
| CVE-2026-8458 | Low | wrong reuse for different services | patch 0031 (NextBSD) |
| CVE-2026-8924 | Low | trailing dot domain super cookie | N/A: the flaw is in the libpsl check, and libpsl is not built (as on macOS) |
| CVE-2026-8927 | Medium | env-set cross-proxy Digest auth state leak | patch 0018 |
| CVE-2026-8932 | Low | incomplete mTLS config matching in conn reuse | patch 0020 |
| CVE-2026-9547 | Low | SSH improper host validation | N/A: no SSH backend |
| CVE-2026-10536 | Low | HTTP/2 stream-dependency tree UAF | N/A: the dependency tree exists only with nghttp2; without it `CURLOPT_STREAM_DEPENDS` returns `CURLE_NOT_BUILT_IN` |
| CVE-2026-11856 | Medium | cross-origin Digest auth state leak | patch 0019 |
| CVE-2026-12064 | Low | proto-default skips SSH verification | N/A: no SCP/SFTP |
| CVE-2026-13608 | Low | OpenLDAP SASL authentication bypass | N/A: LDAP is off |
| CVE-2026-18924 | Low | HTTP/2 server push UAF | N/A: no HTTP/2 |
| CVE-2026-19931 | Medium | Negotiate ambient user conn reuse | patch 0032 (NextBSD) |
| CVE-2026-80230 | Low | OpenSSL pinning bypass | patch 0021 |
| CVE-2026-80231 | Low | native CA store conn reuse | N/A: with OpenSSL, `CURLSSLOPT_NATIVE_CA` does anything only on Windows in 8.7.1 |
| CVE-2026-82209 | Low | domain-scoped PSL domain cookie | N/A: libpsl only |
| CVE-2026-86409 | Low | stale SigV4 headers sent unintended | patch 0022 |
| CVE-2026-86410 | Low | redirect to SFTP or SCP bypasses host verification | N/A: no SCP/SFTP |
| CVE-2026-86414 | Low | leaking host credentials to HTTP proxy | patch 0023 |
| CVE-2026-87863 | Low | HSTS response erases persisted entries | patch 0024 |
| CVE-2026-87864 | Low | IMAP APPEND plus CRLF command injection | patch 0025 |
| CVE-2026-87865 | Low | knownhost callback mismatches key as missing | N/A: libssh2 only |
| CVE-2026-87866 | Low | wolfSSL public-key pinning bypass | N/A: wolfSSL only (`x509asn1.c`) |
| CVE-2026-92386 | Low | server-set cookie aborts cookie jar load | patch 0033 |
| CVE-2026-92388 | Low | FTP wildcard write callback mixup | patches 0026, 0027 |
| CVE-2026-92389 | Low | TFTP direction confusion | patch 0028 |
| CVE-2026-92390 | Low | `-J` no-clobber bypassed with 103 early hints | N/A in 8.7.1: `-J` cannot be combined with `-i` (`PARAM_CONTDISP_SHOW_HEADER`), and neither a 103 response nor `--etag-save` opens the output file before `Content-Disposition` is seen, so the header buffering the upstream fix changes does not exist |
| CVE-2026-92391 | Low | `curl_mime_free` stack overflow by recursion | patch 0029 |
| CVE-2026-94266 | Low | password leak in `CURLINFO_EFFECTIVE_URL` | patch 0030 |
| CVE-2026-107928 | Low | info leak uploading over WebSockets with GnuTLS | N/A: GnuTLS only |
| CVE-2026-8926 | Low | password leak with netrc and user in URL | patch 0034 (NextBSD); introduced by the CVE-2024-11053 fix (0002) |
| CVE-2026-87862 | Low | HSTS cache import drops longer-lived child | patch 0035; introduced by the CVE-2024-9681 fix Apple carries |

Advisories outside 8.7.1's range that touch code this tree has, checked and not applicable:
CVE-2025-9086 (8.13.0+; Apple carries the fix anyway), CVE-2026-8925 (GNU SASL, not built),
CVE-2025-10148 (WebSockets, off).

**How the patches were checked:** the patched tree was built natively (macOS, OpenSSL 3.5,
GSS-API, debug build) and curl 8.7.1's own test suite was run: 1491 tests, with the same
failures as unpatched Apple 8.7.1 (symbol/doc consistency tests such as 1119 and 1477) plus
unit test 1304, which tests the old netrc semantics the CVE-2024-11053 fix changes. The
build was also cross-built for amd64 and arm64 against the NextBSD base sysroot. Neither is
part of CI.

## Updating

1. Download the new Apple tag archive, put its SHA-256 and tag in `nextbsd-trim.sh`, run
   `./nextbsd-trim.sh <archive> .` here.
2. Read the new `apple/curl.plist`: drop every patch whose fix Apple now carries.
3. Check the remaining patches still apply (`build-userland.sh` stops on the first that
   does not) and redo the CVE check above against `https://curl.se/docs/vuln.json`.
