# Vendored: Apple libxml2

**Upstream:** https://github.com/apple-oss-distributions/libxml2
**Tag:** `libxml2-39.10` (commit `4a38cab210b7f915ffde8db164d2812263564342`)
**Tag archive SHA-256:** `e80fc769240edfeb7d79fd15b89bf41820f4d77c9c06cfb2f47dd966c98d8ec2`
(`https://github.com/apple-oss-distributions/libxml2/archive/refs/tags/libxml2-39.10.tar.gz`)
**Carries:** libxml2 **2.9.13** plus Apple's fixes (`libxml2.plist`: `OpenSourceVersion` 2.9.13)
**Imported:** 2026-10-24 ([nextbsd-userland#442](https://github.com/nextbsd/nextbsd-userland/issues/442), E24 decision D4)
**License:** MIT (`Copyright`)

## Why Apple's

macOS ships libxml2 as a system library (`/usr/lib/libxml2.2.dylib`), and E24
decision D4 takes Apple's source wherever Apple ships an equivalent under a
licence that allows it. GNUstep and Gershwin build against it.

## What is here

`src/libxml2` is the `libxml2/` directory of Apple's tag, unmodified, with
these removed because nothing builds or ships them:

- `doc/` except the `xmllint.1` and `xmlcatalog.1` pages; `result/`, `test/`,
  `xstc/`, `fuzz/`, `fuzz-old/`, `example/`;
- `python/`, `os400/`, `win32/`, `vms/`, `VxWorks/`, `macos/`, `bakefile/`,
  `.gitlab-ci.yml`.

Apple's top-level Xcode build (`libxml2.xcodeproj`, `Configurations/`,
`Pregenerated Files/`, `Libxml2XCTests/`, `Tools/`, `Makefile*`) is not
imported; `libxml2.plist` is kept for its provenance record.

`patches/` holds NextBSD's changes as a series. `build-userland.sh` copies
this tree to `.build/libxml2-src-<arch>` and applies the series there with
`patch -p1`, so the tree in git stays byte-for-byte Apple's.

To re-vendor: extract the tag archive above, check its SHA-256, copy its
`libxml2/` over this directory, delete the paths listed above, keep
`VENDORED.md` and `patches/`, and check each patch still applies.

## Build

Apple's tree has no generated `configure` (Apple runs `autoreconf` inside
its own build), but it keeps the `CMakeLists.txt` upstream added in 2.9.11.
`build-userland.sh` builds that with host cmake/ninja and
`cmake/cross-<arch>.cmake`, as it does ICU and libdispatch. No autotools.

The features follow Apple's own build (its `Pregenerated Files` config):
threads, zlib, HTTP/FTP, legacy and DocBook APIs, XPointer locations
compiled in (and disabled at run time, see patch 0001), no lzma. One
difference: the encoding converter is **libc `iconv(3)`**, not ICU. Apple
links libicucore, but NextBSD's ICU is swift-foundation-icu, whose headers
are `_foundation_unicode/*.h`, not the `<unicode/*.h>` libxml2 includes.

Installed:

| Path | What |
|---|---|
| `/usr/lib/libxml2.so.2` (→ `.so.2.9.13`), `libxml2.so` | library, upstream soname |
| `/usr/include/libxml2/libxml/*.h` | headers |
| `/usr/libdata/pkgconfig/libxml-2.0.pc` | pkg-config (`prefix=/usr`) |
| `/usr/lib/cmake/libxml2-2.9.13/` | CMake package config |
| `/usr/bin/xml2-config` | upstream config script |
| `/usr/bin/xmllint`, `/usr/bin/xmlcatalog` | the commands macOS ships |
| `xmllint(1)`, `xmlcatalog(1)`, `libxml(3)` | man pages |

Not built: Python bindings, tests, ICU, lzma. The HTML documentation is not
shipped.

## Patches

| Patch | Why |
|---|---|
| `0001-nextbsd-apple-linked-on-or-after-behaviour.patch` | Apple's stricter behaviour (XPath operation and recursion limits, XPointer locations disabled at run time) is gated on dyld's linked-on-or-after checks, which only exist on Darwin, so off Darwin `linkedOnOrAfter*()` returned false and NextBSD got legacy 2.9.13 behaviour. They now return true, and `LIBXML_HAS_XPOINTER_LOCATIONS_DISABLED_AT_RUNTIME` is defined. Also includes `xmlversionInternal.h` unconditionally, which off Darwin was an implicit declaration (an error with clang 19). |
| `0002-cmake-soversion.patch` | 2.9.13's CMake build has no `SOVERSION`, so its soname was `libxml2.so.2.9.13`; upstream's is `libxml2.so.2`. |
| `0003-uri-include-stdint.patch` | Apple's overflow check in `xmlBuildRelativeURI()` uses `INT32_MAX`, which only Darwin's headers declare transitively. |
| `0010`–`0027` | Security fixes from upstream that Apple's tree lacks on NextBSD; see the CVE check below. |

The memory-allocator change Apple gates on `linkedOnOrAfter2024EReleases()`
needs `LIBXML_HAS_DEPRECATED_MEMORY_ALLOCATION_FUNCTIONS`, which the public
headers set only for Apple SDKs, so `xmlMalloc` and friends are unchanged.

Both libxml2 and Apple's libxslt export a `linkedOnOrAfterFall2022OSVersions`
symbol. Off Darwin both now return true, so it does not matter which one the
dynamic linker binds.

## Security fixes since 2.9.13 (CVE check)

**Result:** Apple back-ported most fixes up to 2.14.x. Eighteen fixes it lacks (or has only on Darwin) are carried as `patches/0010`–`0027`, all from upstream commits; each patch starts with its CVE, the upstream commit and what was adapted. Not carried: the disputed CVE-2023-39615 (API misuse only), the 2.11 entity-amplification rework (large refactor), malloc-failure-only and leak-only fixes, and the >2 GiB `xmlBuildRelativeURI` counters. Also not carried, because upstream has no released fix yet: GitLab #1011 (CVE-2025-67812), #1104, #1082, #1033–1035, #1049 (closed without a public fix) and the open #1037, #1038, #1063, #1087, #1121, #1139. One Apple-only bug is noted: `xmlPushInput` double-frees the input when `inputPush` fails, which needs an allocation failure. Patches 0021 (`xmlCopyEntity` keeps children) and 0025 (`xmlXIncludeProcess` honours the document's parse flags) change behaviour as upstream did.

Checked on 2026-10-24 against upstream libxml2 up to v2.15.4 and master, libxslt up to v1.1.45 and master, the Debian security tracker, and the libxml2 GitLab issues labelled "1. Security". "In Apple tree?" is judged on the code that compiles on NextBSD (no `__APPLE__`); several of Apple's own fixes sit behind `__APPLE__` or OS-version checks and do not count. Line numbers refer to the unpatched Apple tree.

| component | CVE / issue | upstream fixed-in + commit | in Apple tree? (evidence) | action |
|---|---|---|---|---|
| libxml2 | CVE-2022-29824 | 2.9.14, 2554a240 (master 6c283d83) | Yes: buf.c:xmlBufGrowInternal:454 (SIZE_MAX check); tree.c:xmlBufferGrow:7450, xmlBufferAdd:7714 (INT_MAX checks, 2.10 form) | already fixed |
| libxml2 | CVE-2022-2309 | 2.10.0, 5930fe01 + a82ea25f | Yes: parser.c:xmlCtxtReset:15030 and HTMLparser.c:htmlCtxtReset:6861 set `nsNr = 0` | already fixed |
| libxml2 | CVE-2022-40303 | 2.10.3, c8469863 | Yes: all 101 added lines present in parser.c (XML_PARSE_HUGE length counters) | already fixed |
| libxml2 | CVE-2022-40304 | 2.10.3, 1b41ec4e | Yes: entities.c, all added lines present (entity-cycle dict corruption) | already fixed |
| libxml2 | CVE-2022-49043 | 2.11.0, 5a19e216 | Yes: xinclude.c:xmlXIncludeAddNode:654-664 frees URI after the error path | already fixed |
| libxml2 | CVE-2023-28484 | 2.10.4, 647e072e (+4c6922f7) | Yes: xmlschemas.c:xmlSchemaFixupComplexType:18645 | already fixed |
| libxml2 | CVE-2023-29469 | 2.10.4, 09a2dd45 | Yes: dict.c:xmlDictComputeFastKey:512 (`namelen <= 0`) | already fixed |
| libxml2 | CVE-2023-39615 (disputed) | 2.12.0, d0c3f01e + 235b15a5 | No: parser.c:xmlCtxtUseOptionsInternal:15269 still installs xmlSAX2StartElement for XML_PARSE_SAX1 | not carried. Low: needs a custom SAX handler combined with XML_PARSE_SAX1 (API misuse per upstream), and the fix changes how SAX1 and SAX2 are detected |
| libxml2 | CVE-2023-45322 | 2.12.2 30d7660b / 2.13.0 8707838e | Yes: tree.c:xmlStaticCopyNodeList:4494-4529 (`newSubset`) | already fixed |
| libxml2 | CVE-2024-25062 | 2.11.7 2b0aac14 / 2.12.5 92721970 | Yes: xmlreader.c:xmlTextReaderRead:2047 (`state != BACKTRACK`) | already fixed |
| libxml2 | CVE-2024-34459 | 2.11.8 8ddc7f13 / 2.12.7 2876ac53 | Yes: xmllint.c:xmlHTMLPrintFileContext:602,620 (`cur > base`) | already fixed |
| libxml2 | CVE-2024-40896 | 2.11.9 / 2.12.9 / 2.13.3, de28e6ed | n/a: the bug was introduced in 2.11.0 (481d79d4) | N/A |
| libxml2 | CVE-2024-56171 | 2.12.10 245b70d7 / 2.13.6 a6bf2cb7 | Yes: xmlschemas.c:23704, 23922 (reload `dupls->items`) | already fixed |
| libxml2 | CVE-2025-24928 | 2.12.10 858ca26c / 2.13.6 055b3825 | Yes, equivalent: Apple rewrote valid.c:xmlSnprintfElementContent:1335 and xmlSnprintfElements:5279 with tracked lengths and terminateBufferTooSmall() | already fixed |
| libxml2 | CVE-2025-27113 | 2.12.10 503f788e / 2.13.6 6c716d49 | Yes: pattern.c:1190, 1193 (`XML_OP_ELEM`) | already fixed |
| libxml2 | CVE-2025-32414 | 2.13.8 443a9c89 / 2.14.2 | Python bindings only | not applicable (no Python bindings built) |
| libxml2 | CVE-2025-32415 | 2.13.8 384cc7c1 / 2.14.2 8ac33b1c | Yes: xmlschemas.c:xmlSchemaIDCFillNodeTables:23664, 23715 (`bind->nbNodes`) | already fixed |
| libxml2 | CVE-2025-6021 | 2.13.9 17d950ae / 2.14.4 acbbeef9 | Yes: tree.c:xmlBuildQName:234-244 (size_t lengths, SIZE_MAX check) | already fixed |
| libxml2 | CVE-2025-6170 | 2.13.9 5e9ec5c1 / 2.14.5 c340e419 | Yes, equivalent: debugXML.c:xmlShell:2873, 2889 (loops bounded by `sizeof(command)` / `sizeof(arg)`) | already fixed |
| libxml2 | CVE-2025-49794, CVE-2025-49796 | 2.13.9 81cef8c5 / 2.14.5 / master 71e1e8af | No: schematron.c:xmlSchematronGetNode:1246 frees the XPath result, then the node is used | **patch 0010** |
| libxml2 | CVE-2025-49795 | 2.13.9 62048278 / master 499bcb78 + 24d7e159 | n/a: sch:value-of was introduced in 2.10.0 (2cc93f77) and is absent | N/A |
| libxml2 | regexp int overflow / OOB, #950 (no CVE; NEWS "Security") | 2.13.9 / 2.14.6, 89b8b9de | Partly: fixed only in the `#ifdef __APPLE__` xmlRegCalloc2 macro (xmlregexp.c:442-447); the generic function compiled on NextBSD has no limit | **patch 0011** |
| libxml2 / libxslt | CVE-2025-7425 (libxml2-side mitigation) | 2.13.9 a489aca8 / 2.14.6 9de92ed7 / master f1e1f13b | Partly: masking XML_ATTR_GET_ATYPE (include/libxml/tree.h:449-460) is enabled only for macOS 14+ / iOS 17+ targets. On NextBSD, libxslt's `atype \|= flags<<27` (xsltutils.c:1937) makes xmlFreeProp (tree.c:2152) skip xmlRemoveID, leaving a use-after-free | **patch 0012** (libxml2; no libxslt patch needed) |
| libxml2 | CVE-2025-9714 | 2.10.0, 677a4264 | No: xpath.c:xmlXPathRunEval:13831, xmlXPathCtxtCompile:14186/14212, xmlXPathEvalExpr:14386/14400 reset `depth = 0` | **patch 0013** |
| libxml2 / libxslt | CVE-2025-10911 (libxml2-side) | 2.15.2, d583ca9b (libxslt MR !77 unmerged) | No: xpath.c:xmlXPathNextFollowingSibling, NextPrecedingSibling, NextFollowing, NextPrecedingInternal follow xmlDoc prev/next | **patch 0014**; libxslt-side MR !77 not carried (unmerged upstream, about 700 lines, ABI change) |
| libxml2 | CVE-2025-8732 (+ #1064) | 2.15.2, 3425dece + 4982ee83 | No: catalog.c:xmlParseSGMLCatalog:2371 and xmlExpandCatalog:2782 have no depth limit or dedup | **patch 0016** |
| libxml2 | CVE-2025-26434 | 2.14.0, 5e787401 | n/a: bug introduced in 2.14.0 (2adcde39) | N/A |
| libxml2 | CVE-2026-0989 | 2.15.2, 66c52b3a | No: relaxng.c:xmlRelaxNGIncludePush has no depth limit | **patch 0015** (limit fixed at 1000; no new API or env var) |
| libxml2 | CVE-2026-0990 | 2.15.2 ac6f0fde / master 1961208e | No: catalog.c:xmlCatalogListXMLResolveURI:2132 has no depth guard | **patch 0017** |
| libxml2 | CVE-2026-0992 | 2.15.2, 4af23b52 + 096402c9 | No: catalog.c:xmlParseXMLCatalogNode:1314 | **patch 0018** |
| libxml2 | CVE-2026-76781 | 2.15.4 a34c084d / master c6324894 | n/a on its own: a regression of the 0992 fix | folded into **patch 0018** |
| libxml2 | CVE-2026-1757 | 2.15.2 5446460a / 160c8a43 | Yes, equivalent: debugXML.c:xmlShell:2878-2880 calls `free(cmdline)` before `continue` | already fixed |
| libxml2 | CVE-2026-6653 | 2.11.0, 463bbeec | Yes, equivalent: parser.c:xmlPushInput:2417 checks `instate == XML_PARSER_EOF` before inputPush, so a pushed input is never freed by the caller | already fixed |
| libxml2 | CVE-2026-6732 | master 226b5608 + 7cea3fd1 | n/a: bug introduced in 2.13.0 (e1153832) | N/A |
| libxml2 | CVE-2026-74860 | 2.15.3, f41e1865 | Python bindings only | not applicable (no Python bindings built) |
| libxml2 | CVE-2026-86137 | 2.15.4, 76fe08d9 | n/a: bug introduced in 2.15.3 | N/A |
| libxml2 | CVE-2026-86138 | 2.15.4 a4cba4b5 / master e41032ae | No: dict.c:xmlDictAddQString:393-396 (unsigned overflow of `4*(namelen+plen+1)`) | **patch 0022** |
| libxml2 | CVE-2026-86139 | 2.15.4, 8edbbdb0 | Yes, equivalent: uri.c:xmlURIEscapeStr (`if (!(len > 0)) return NULL`); bug introduced in 2.13 upstream | already fixed |
| libxml2 | CVE-2026-86140 | 2.15.4 d1686f91 / master 61e5f3ea | Yes, equivalent: Apple's bounded valid.c:xmlSnprintfElements:5288-5385 (reserves room for `(` and `)`) | already fixed |
| libxml2 | CVE-2026-86141 | 2.15.4, e89a8aae | n/a: introduced by the 86137 fix | N/A |
| libxml2 | CVE-2026-86142 | 2.15.4 6b3a736c / master 595d7d8f | No: xpointer.c:xmlXPtrEvalXPtrPart:1060 (xmlStrlen saturates to 0, giving a 1-byte buffer) | **patch 0023** |
| libxml2 | CVE-2026-86143 | 2.15.4 90f293ba / master b06e0364 | No: xmlIO.c:xmlOutputBufferWrite/WriteEscape/Flush:3406-3731 truncate xmlBufUse() to int | **patch 0024** |
| libxml2 | CVE-2026-86144 | 2.15.4, b63cd517 | No: xinclude.c:xmlXIncludeProcess:2649 and xmlXIncludeProcessTree:2694 pass flags 0 | **patch 0025** (entry points only; the XIncludeLoadTxt part needs the 2.13+ loader API) |
| libxml2 (xmlcatalog) | CVE-2026-11979 | 2.15.4 cd48d441 / master c2e233fc | No: xmlcatalog.c:usershell:115-150 | **patch 0026** (needed only if xmlcatalog is installed) |
| libxml2 | c14n type confusion in xmlC14NProcessAttrsAxis, #1096 (NEWS 2.15.3 Security) | 2.15.3 495c9451 / master 0fd00ec5 | No: c14n.c:1192 | **patch 0019** |
| libxml2 | c14n type confusion in xmlC14NProcessNamespacesAxis, #1016 | 2.15.2, f879a017 | Yes: c14n.c:624 | already fixed |
| libxml2 | c14n double free, #1073 (NEWS 2.15.3 Security) | 2.15.3, a97e0d5e | n/a: needs the 2.13+ xmlStrncat that frees on failure; Apple's xmlstring.c:xmlStrncat:465 returns `cur` | N/A |
| libxml2 | xmlCopyEntity drops children (ID/SAML bypass), #1074 (NEWS 2.15.3 Security) | 2.15.3 52397a02 / master c9aa3048 | No: entities.c:xmlCopyEntity:947-973 | **patch 0021** |
| libxml2 | xmlParseReference wrong SAX userData, #1097 (NEWS 2.15.3 Security) | 2.15.3, ad0f009a | n/a: the cached-tree fast path is not in 2.9.13 (no `sax->characters(ctxt,` calls) | N/A |
| libxml2 | relaxng use-after-free in xmlRelaxNGValidateValue, #1055 | 2.15.2 df2ba65f / master 5dfd9065 | No: relaxng.c:8933 (VALID_ERR2) | **patch 0020** |
| libxml2 | xmlAddChild frees a linked text node, #1129 | 2.15.4 7873093e / master 6be0acd2 | No: tree.c:xmlAddChild text-merge branches | **patch 0027** |
| libxml2 | infinite loop in xmlCtxtParseContent; xmlEscapeText size_t; xmlRemoveEntity; maxAmpl div-by-0; xmlIOParseDTD double free | 2.15.2-2.15.4, 0975b758 / 5b5aeffb / 2e55fbde / 52c726e9 / 386b85e3 | n/a: functions or semantics introduced in 2.12-2.14 | N/A |
| libxml2 | xmlBuildRelativeURISafe int counters, #1062 | 2.15.2, f0e757a2 | Partly: uri.c:xmlBuildRelativeURI already guards the malloc size (`nbslash > (INT32_MAX-len)/3`); ix/pos remain int | not carried. Low: needs a base URI larger than 2 GiB; the heap-overflow half is already guarded |
| libxml2 | schematron sch:let NULL deref and error-path leaks | 2.15.2, 41ba80b9 / 97233ebe / 28da8549 | n/a or leak-only: sch:let is not in 2.9.13; the rest are leaks | N/A / not carried (leaks only) |
| libxml2 | 2.9.14-2.11.0 NEWS "Security" items: XPtr double free 10291f83, htmlSkipBlankChars overflow 44e9118c, xmlElementContent leak, xmlBuf/xmlBuffer NUL and overflow series (6ef16dee, 4ce2abf6, c14cac8b, a6df42e6), xmlSetTreeDoc ownership 4bc3ebf3, XIncludeCopyRange 0aa8652e, reader close UAF c50196c1, xmlParseContentInternal UAF 86105c04, xmllint --maxmem UAF d7daf9fd, error-message OOB 5d55315e, xmlParseCharDataInternal UAF 9ec9a7c9 | 2.9.14 / 2.10.0 / 2.11.x | Yes: all added lines or an equivalent (size_t + clamp) are present | already fixed |
| libxml2 | 2.9.14 "Normalize XPath strings in-place" 57b81c20; XPath parser-ctxt double delete on OOM 74263eff | 2.9.14 / 2.10.0 | No: xpath.c:xmlXPathNormalizeFunction:9114 | not carried. Low: leak or malloc-failure only |
| libxml2 | entity amplification rework (2.11.0 "Security") | 2.11.0, 463bbeec and series | No: Apple has the 2.9.x-style xmlParserEntityCheck plus its own limits | not carried. Large refactor; the related UAF (CVE-2026-6653) is handled as above |
