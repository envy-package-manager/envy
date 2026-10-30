# Third-Party Dependencies

## picojson

**Version:** 1.3.0
**License:** BSD-2-Clause
**Purpose:** Single-header JSON parser/serializer—parse, edit, serialize `.luarc.json` and structured JSON output
**Integration:** Single header downloaded to `out/cache/picojson-src/`
**Rationale:** Smallest viable JSON library (34 KB); replaces bespoke JSON output in `cmd_product.cpp` and enables safe round-trip editing of user `.luarc.json` files

## Sol2

**Version:** c1f95a773c6f8f4fde8ca3efe872e7286afe4444
**License:** MIT
**Purpose:** Header-only C++/Lua bindings—type-safe stack manipulation, STL interop
**Integration:** Amalgamation generated via `single.py` during CMake configure
**Rationale:** Eliminates error-prone manual stack manipulation; enables idiomatic C++ patterns for Lua integration

## Mbed TLS

**Version:** 3.6.7
**License:** Apache-2.0
**Purpose:** Crypto (SHA-256) and the TLS client behind libgit2, libssh2, and Linux libcurl
**Integration:** `cmake/templates/mbedtls_user_config.h.in` layers TLS 1.3 and threading atop the stock config and `#undef`s `MBEDTLS_SSL_SRV_C`; every consumer compiles against the same file, so struct layouts agree
**Rationale:** envy only dials out; the TLS server state machines cost ~35 KB

## libgit2

**Version:** 1.9.7
**License:** GPL-2.0 with linking exception
**Purpose:** Every git fetch—spec, bundle, and package sources with their submodules—and `envy git-resolve`
**Integration:** Smart protocol v0 only (no v2 client through 2.0.0-rc.1), so an unadvertised commit needs the server's `allow-reachable-sha1-in-want`; GitHub, GitLab, googlesource, and kernel.org advertise it. `USE_SHA1` stays unset: the default collision-detecting SHA-1 is what makes a commit pin the integrity check for a whole submodule tree; `cmake/templates/libgit2_longpaths_patch.py.in` raises libgit2's Windows `MAX_PATH` cap on repository-internal paths (gitdir, refs, packs) to `GIT_PATH_MAX`, which its `\\?\` path layer already reaches—nested submodule gitdirs outgrow 260
**Rationale:** 1.9.7 escapes repository paths in the libssh2 transport (CVE-2026-5917); 2.0 waits for a release

## AWS SDK for C++

**Version:** 1.11.850 (CRT 0.42.1)
**License:** Apache-2.0
**Purpose:** S3 transfers and the credential provider chain
**Integration:** `BUILD_ONLY=s3;transfer;sso;sso-oidc`; patches in `cmake/DependencyPatches.cmake` (curl, runtime, S3 endpoint ruleset deflated); CRT targets build with `AWS_STATIC_LOG_LEVEL=0`; `ENABLE_RTTI=OFF` and `-fno-rtti` on `aws-crt-cpp`, matching envy
**Rationale:** envy installs a discarding CRT logger, so the CRT's ~700 log sites compile out (~140 KB); the SDK's own C++ logging stays, since credential failures explain themselves there; mixed RTTI gave GCC LTO -Wodr vtable mismatches, and the SDK's and CRT's typeinfo was ~138 KB

## nanosemver

**Version:** 0.5.0
**License:** Unlicense OR 0BSD
**Purpose:** Semver 2.0.0 parse, precedence, ranges—OPTIONS `semver` type and `range`, self-update ordering, `.luarc.json` envy paths
**Integration:** Release-asset `nanosemver.h`/`.c` (stamped; the tag's raw header is not) as a static library, `NSV_DIAG=1`; `src/semver.h` routes every parse through one process-wide print hook into a per-thread string, so a failure that throws says where and why
**Rationale:** Replaces Neargye/semver—no templates or allocation; strict npm range grammar (no hyphens, plus `!=`) with no flags, so pre-releases rank below their release
