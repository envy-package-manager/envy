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

## AWS SDK for C++

**Version:** 1.11.850 (CRT 0.42.1)
**License:** Apache-2.0
**Purpose:** S3 transfers and the credential provider chain
**Integration:** `BUILD_ONLY=s3;transfer;sso;sso-oidc`; patches in `cmake/DependencyPatches.cmake` (curl, runtime, S3 endpoint ruleset deflated); CRT targets build with `AWS_STATIC_LOG_LEVEL=0`
**Rationale:** envy installs a discarding CRT logger, so the CRT's ~700 log sites compile out (~140 KB); the SDK's own C++ logging stays, since credential failures explain themselves there
