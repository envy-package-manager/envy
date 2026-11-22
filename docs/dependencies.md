# Third-Party Dependencies

## picojson

**Version:** 1.3.0
**License:** BSD-2-Clause
**Purpose:** Single-header JSON parser/serializer—parse, edit, serialize `.luarc.json` and structured JSON output; `envy.json` string lexing
**Integration:** Single header downloaded to `out/cache/picojson-src/`; `envy.json.decode` drives picojson's string lexer from an iterative reader with its own RFC 8259 number grammar—picojson's `_parse` recurses on the C stack, has no depth cap, and accepts `01`, `2.`, `-.5`
**Rationale:** Smallest viable JSON library (34 KB); replaces bespoke JSON output in `cmd_product.cpp` and enables safe round-trip editing of user `.luarc.json` files

## tomlc17

**Version:** R261116
**License:** MIT
**Purpose:** TOML 1.1 parser behind `envy.toml.decode`
**Integration:** `tomlc17.h`/`.c` from the release tag as a static C99 library; `check_utf8` set once before the first parse
**Rationale:** Measured against toml++ 3.4.0, toml11 4.4.0, toml-c, cpptoml 0.1.1, libtoml under envy's flags: smallest fully conformant (681/681 toml-test 1.1, ~18 KB), fastest (894 KB Rust manifest in 6.5 ms vs toml++ 18 ms, toml11 360 ms), and the only one both fully conformant and crash-free under 100k-deep nesting and ASan/UBSan fuzzing; toml++ reaches `__builtin_unreachable` on valid CJK input

## Sol2

**Version:** c1f95a773c6f8f4fde8ca3efe872e7286afe4444
**License:** MIT
**Purpose:** Header-only C++/Lua bindings—type-safe stack manipulation, STL interop
**Integration:** Amalgamation generated via `single.py` during CMake configure
**Rationale:** Eliminates error-prone manual stack manipulation; enables idiomatic C++ patterns for Lua integration

## Mbed TLS

**Version:** 3.6.8
**License:** Apache-2.0
**Purpose:** Crypto (SHA-256) and the TLS client behind libgit2, libssh2, and Linux libcurl
**Integration:** `cmake/templates/mbedtls_user_config.h.in` layers TLS 1.3 and threading atop the stock config and `#undef`s `MBEDTLS_SSL_SRV_C`; every consumer compiles against the same file, so struct layouts agree
**Rationale:** envy only dials out; the TLS server state machines cost ~35 KB. Stays on 3.6 LTS (EOL 2027-03)—libgit2 1.9 and libssh2 1.11 need `ctr_drbg`/`entropy`, gone in 4.x

## libgit2

**Version:** 1.9.7
**License:** GPL-2.0 with linking exception
**Purpose:** Every git fetch—spec, bundle, and package sources with their submodules—and `envy git-resolve`
**Integration:** Smart protocol v0 only (no v2 client through 2.0.0-rc.1), so an unadvertised commit needs the server's `allow-reachable-sha1-in-want`; GitHub, GitLab, googlesource, and kernel.org advertise it. `USE_SHA1` stays unset: the default collision-detecting SHA-1 is what makes a commit pin the integrity check for a whole submodule tree; `cmake/templates/libgit2_longpaths_patch.py.in` raises libgit2's Windows `MAX_PATH` cap on repository-internal paths (gitdir, refs, packs) to `GIT_PATH_MAX`, which its `\\?\` path layer already reaches—nested submodule gitdirs outgrow 260; `cmake/templates/libgit2_cert_location_patch.py.in` skips the configure-time `openssl version -d` probe, so the build host's CA path isn't compiled in—envy loads CA certificates for each http(s) remote, after `insteadOf`; `cmake/templates/libgit2_https_transport_patch.py.in` has https fail as git:// does—WinHTTP takes the server timeouts and words a timeout in English; mbedTLS keeps the socket's error
**Rationale:** 1.9.7 escapes repository paths in the libssh2 transport (CVE-2026-5917); 2.0 waits for a release

## AWS C libraries

**Version:** aws-c-s3 0.13.7, aws-c-auth 0.10.5, aws-c-http 0.11.1, aws-c-io 0.27.7 and their deps—each library's last 0.x, the code its 1.0.0 retagged; Linux adds aws-lc 5.5.0 and s2n-tls 1.7.7, the pair aws-crt-cpp 0.43.8 pinned atop 0.x
**License:** Apache-2.0 (aws-c-common's `THIRD-PARTY-LICENSES.txt` covers its cJSON and libcbor)
**Purpose:** S3 transfers (auto-ranged GET/PUT, CRC64NVME) and the credential chain
**Integration:** `cmake/deps/AwsC.cmake` fetches each pinned release archive (`ENVY_AWS_C_DEPS`), extracts it to `out/cache/aws-c/<repo>-<tag>`, drops the archive, prunes unpinned trees, and adds them in-tree as aws-crt-cpp's `BUILD_DEPS` did; macOS and Windows use the OS's TLS and never fetch aws-lc/s2n-tls; `AWS_STATIC_LOG_LEVEL=0`; patches (stamped with their script's hash beside the sources): static MSVC runtime, AVX2 base64 signature, s2n feature probes, aws-lc and s2n noexecstack; `src/aws_util.cpp` uses private `aws/auth/private/{sso_token_utils,credentials_utils}.h` to name a lapsed SSO token offline and to bound credential HTTP responses; `refresh_sso_token` renews an sso-session's cached token near expiry via SSO-OIDC `CreateToken` (aws-c-auth's TODO), as every AWS SDK does
**Rationale:** Replaced aws-sdk-cpp + aws-crt-cpp (#424), whose `std::function`-heavy C++ carried most of RTTI's cost: −730 KB on macOS arm64. Once the session or client registration ends, envy says to run `aws sso login`

## nanosemver

**Version:** 0.5.0
**License:** Unlicense OR 0BSD
**Purpose:** Semver 2.0.0 parse, precedence, ranges—OPTIONS `semver` type and `range`, self-update ordering, `.luarc.json` envy paths
**Integration:** Release-asset `nanosemver.h`/`.c` (stamped; the tag's raw header is not) as a static library, `NSV_DIAG=1`; `src/semver.h` routes every parse through one process-wide print hook into a per-thread string, so a failure that throws says where and why
**Rationale:** Replaces Neargye/semver—no templates or allocation; strict npm range grammar (no hyphens, plus `!=`) with no flags, so pre-releases rank below their release
