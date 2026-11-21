include(FetchContent)
include(ExternalProject)

include("${CMAKE_CURRENT_LIST_DIR}/EnvyFetchContent.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DependencyPatches.cmake")

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 NEW)
endif()

set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "Disable deprecated CMake warnings from third-party builds" FORCE)

set_property(GLOBAL PROPERTY JOB_POOLS envy_fetch=4)

# Persist fetched sources outside the build tree so deleting `out/build`
# forces a rebuild while reusing cached downloads.
cmake_path(APPEND PROJECT_SOURCE_DIR "out" "cache" OUTPUT_VARIABLE ENVY_CACHE_DIR)
file(MAKE_DIRECTORY "${ENVY_CACHE_DIR}")
set(FETCHCONTENT_BASE_DIR "${ENVY_CACHE_DIR}")

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
if(DEFINED ENV{ENVY_FETCH_FULLY_DISCONNECTED})
    set(FETCHCONTENT_FULLY_DISCONNECTED ON)
endif()

# ---------------------------------------------------------------------------
# Third-party version catalog
# ---------------------------------------------------------------------------
set(ENVY_MBEDTLS_VERSION "3.6.7")
set(ENVY_MBEDTLS_ARCHIVE "mbedtls-${ENVY_MBEDTLS_VERSION}.tar.bz2")
set(ENVY_MBEDTLS_URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${ENVY_MBEDTLS_VERSION}/${ENVY_MBEDTLS_ARCHIVE}")
set(ENVY_MBEDTLS_SHA256 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)

set(ENVY_LIBSSH2_VERSION "1.11.1")
set(ENVY_LIBSSH2_ARCHIVE "libssh2-${ENVY_LIBSSH2_VERSION}.tar.gz")
set(ENVY_LIBSSH2_URL "https://www.libssh2.org/download/${ENVY_LIBSSH2_ARCHIVE}")
set(ENVY_LIBSSH2_SHA256 d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7)

set(ENVY_LIBGIT2_VERSION "1.9.7")
set(ENVY_LIBGIT2_ARCHIVE "libgit2-${ENVY_LIBGIT2_VERSION}.tar.gz")
set(ENVY_LIBGIT2_URL "https://github.com/libgit2/libgit2/archive/refs/tags/v${ENVY_LIBGIT2_VERSION}.tar.gz")
set(ENVY_LIBGIT2_SHA256 1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7)

set(ENVY_LIBCURL_VERSION "8.21.0")
set(ENVY_LIBCURL_ARCHIVE "curl-${ENVY_LIBCURL_VERSION}.tar.xz")
set(ENVY_LIBCURL_URL "https://curl.se/download/${ENVY_LIBCURL_ARCHIVE}")
set(ENVY_LIBCURL_SHA256 aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6)

# S3 and the AWS C libraries beneath it: "<github org/repo> <tag> <archive sha256>".
# aws-lc and s2n-tls are Linux's TLS; macOS and Windows use the OS's.
set(ENVY_AWS_C_DEPS
    "awslabs/aws-c-common v0.14.2 da518accb60eb08e0d56f641f00c3ea856c36a0972394770c978e5ff3bfc3728"
    "aws/aws-lc v5.2.0 f9ad6bde29eb9c8ce585085d9a12bb2ffe89bfd940a631f760eeb1b2d9b2d1a7"
    "aws/s2n-tls v1.7.5 841f67cb1eb4ce71c829fa12ce940ae33556429c7e137be5c16461999bb7f666"
    "awslabs/aws-c-cal v0.9.14 0e96e0067fa921768e07b5b4ebad82011ccf474903e9286419ef428d68f317ea"
    "awslabs/aws-c-io v0.27.4 0f2ce32de3685ca5ba3a8395d461c783253c1718cf70d31378eb6be890db1e3e"
    "awslabs/aws-checksums v0.2.10 cb6509f75e42ee25c372a6d379e8582ce5179e5335183842e808f7d8abb0c314"
    "awslabs/aws-c-compression v0.3.2 f93f5a5d8b3fee3a6d97b14ba279efacd4d4016ef9cc7dc4be7d43519ecfbe93"
    "awslabs/aws-c-http v0.11.0 4ccbdd33c798b590288330dec9e93abe2ff6cfb198b7a4db036c9d362f2e6506"
    "awslabs/aws-c-sdkutils v0.2.7 802b8c4169da2b4cf5c48f9598fb778faccd3e052e443a482089193411c2b7bb"
    "awslabs/aws-c-auth v0.10.4 6fb567f496a450d4b6d3f5749d735977a0156957e8ccbca9af7a5ee15d1ffda7"
    "awslabs/aws-c-s3 v0.13.1 30cd8deec12a6995b8dda32d8f3a53b3c1f2d2dcad56287aab64dfee80eb7630"
)

set(ENVY_LIBARCHIVE_VERSION "3.8.8")
set(ENVY_LIBARCHIVE_ARCHIVE "libarchive-${ENVY_LIBARCHIVE_VERSION}.tar.gz")
set(ENVY_LIBARCHIVE_URL "https://www.libarchive.org/downloads/libarchive-${ENVY_LIBARCHIVE_VERSION}.tar.gz")
set(ENVY_LIBARCHIVE_SHA256 038918ea315cdd446cc63acfe880d6011832bbe1711c887de5de5441b306c190)

set(ENVY_BLAKE3_VERSION "1.8.5")
set(ENVY_BLAKE3_ARCHIVE "blake3-${ENVY_BLAKE3_VERSION}.tar.gz")
set(ENVY_BLAKE3_URL "https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/${ENVY_BLAKE3_VERSION}.tar.gz")
set(ENVY_BLAKE3_SHA256 220bd81286e2a0585beac66d41ac3f4c2c33ae8a4e339fc88cf22d5e00514fe9)

set(ENVY_LUA_VERSION "5.4.8")
set(ENVY_LUA_ARCHIVE "lua-${ENVY_LUA_VERSION}.tar.gz")
set(ENVY_LUA_URL "https://www.lua.org/ftp/lua-${ENVY_LUA_VERSION}.tar.gz")
set(ENVY_LUA_SHA256 4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae)

set(ENVY_SOL2_GIT_TAG "c1f95a773c6f8f4fde8ca3efe872e7286afe4444")

set(ENVY_ZLIB_VERSION "1.3.2")
set(ENVY_ZLIB_ARCHIVE "zlib-${ENVY_ZLIB_VERSION}.tar.gz")
set(ENVY_ZLIB_URL "https://github.com/madler/zlib/releases/download/v${ENVY_ZLIB_VERSION}/${ENVY_ZLIB_ARCHIVE}")
set(ENVY_ZLIB_SHA256 bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16)

set(ENVY_LIBLZMA_VERSION "5.8.3")
set(ENVY_LIBLZMA_ARCHIVE "xz-${ENVY_LIBLZMA_VERSION}.tar.gz")
set(ENVY_LIBLZMA_URL "https://github.com/tukaani-project/xz/releases/download/v${ENVY_LIBLZMA_VERSION}/${ENVY_LIBLZMA_ARCHIVE}")
set(ENVY_LIBLZMA_SHA256 3d3a1b973af218114f4f889bbaa2f4c037deaae0c8e815eec381c3d546b974a0)

set(ENVY_LIBBZ2_VERSION "1.0.8")
set(ENVY_LIBBZ2_ARCHIVE "bzip2-${ENVY_LIBBZ2_VERSION}.tar.gz")
set(ENVY_LIBBZ2_URL "https://github.com/libarchive/bzip2/archive/refs/tags/bzip2-${ENVY_LIBBZ2_VERSION}.tar.gz")
set(ENVY_LIBBZ2_SHA256 db106b740252669664fd8f3a1c69fe7f689d5cd4b132f82ba82b9afba27627df)

set(ENVY_LIBZSTD_VERSION "1.5.7")
set(ENVY_LIBZSTD_ARCHIVE "zstd-${ENVY_LIBZSTD_VERSION}.tar.gz")
set(ENVY_LIBZSTD_URL "https://github.com/facebook/zstd/archive/refs/tags/v${ENVY_LIBZSTD_VERSION}.tar.gz")
set(ENVY_LIBZSTD_SHA256 37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3)

set(ENVY_NANOSEMVER_VERSION "0.5.0")
set(ENVY_NANOSEMVER_URL "https://github.com/charlesnicholson/nanosemver/releases/download/v${ENVY_NANOSEMVER_VERSION}")
set(ENVY_NANOSEMVER_H_SHA256 4f52a58059de5048f59c7f4f2c21a9df04706c6d993589261a7085026d245cd0)
set(ENVY_NANOSEMVER_C_SHA256 a4e57abd8cbec669b4449013140b5f729fbbe92c890c885127a2390b99dceeb2)
set(ENVY_NANOSEMVER_LICENSE_URL "https://raw.githubusercontent.com/charlesnicholson/nanosemver/v${ENVY_NANOSEMVER_VERSION}/LICENSE")
set(ENVY_NANOSEMVER_LICENSE_SHA256 9018b7787e30335f5f84d0d66abf5ae8862f04a05a7d42cede16faa5509de23e)

set(ENVY_PICOJSON_VERSION "1.3.0")
set(ENVY_PICOJSON_URL "https://raw.githubusercontent.com/kazuho/picojson/v${ENVY_PICOJSON_VERSION}/picojson.h")
set(ENVY_PICOJSON_SHA256 5ddf7276d04926da7be243e7af49258e78cc27278ee9097ba45b942c7a6b5f9d)
set(ENVY_PICOJSON_LICENSE_URL "https://raw.githubusercontent.com/kazuho/picojson/v${ENVY_PICOJSON_VERSION}/LICENSE")
set(ENVY_PICOJSON_LICENSE_SHA256 5585fe141cc7bb08c953f3859db608852968d0bbc625b9b6d95c0bd6349bacb6)

set(ENVY_TOMLC17_VERSION "R261116")
set(ENVY_TOMLC17_URL "https://raw.githubusercontent.com/cktan/tomlc17/${ENVY_TOMLC17_VERSION}")
set(ENVY_TOMLC17_H_SHA256 29c0909092f4b23c7edce705167064f4a90548db5334b4aa2cd95d7ad801adb9)
set(ENVY_TOMLC17_C_SHA256 41cedef3c1214a5bbfac750bd2017ac7ba2952429b436af50f2582decf6e29cf)
set(ENVY_TOMLC17_LICENSE_SHA256 f949d0976f85076c96fc5122f0632b5a1b806d8b2badbe968c42c97230e6e79e)

set(ENVY_DOCTEST_VERSION "2.5.3")
set(ENVY_DOCTEST_URL "https://raw.githubusercontent.com/doctest/doctest/v${ENVY_DOCTEST_VERSION}/doctest/doctest.h")
set(ENVY_DOCTEST_SHA256 cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421)

set(PLATFORM_NETWORK_LIBS)
if(WIN32)
    set(PLATFORM_NETWORK_LIBS ws2_32 dnsapi iphlpapi advapi32 crypt32 wldap32 winhttp bcrypt wininet rstrtmgr)
else()
    find_library(RESOLV_LIBRARY resolv REQUIRED)
    set(PLATFORM_NETWORK_LIBS ${RESOLV_LIBRARY})
endif()
if(APPLE)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(CORESERVICES_FRAMEWORK CoreServices REQUIRED)
    find_library(SYSTEMCONFIGURATION_FRAMEWORK SystemConfiguration REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
endif()

# Enforce static libraries across third-party builds.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable dependency test targets" FORCE)

include("${CMAKE_CURRENT_LIST_DIR}/deps/Zlib.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Liblzma.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Libbz2.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Libzstd.cmake")
if(NOT WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/deps/MbedTLS.cmake")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/deps/Libssh2.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Libgit2.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Libcurl.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/AwsC.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Libarchive.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Blake3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Lua.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Sol2.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Nanosemver.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Picojson.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Tomlc17.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Doctest.cmake")

# libgit2 caches CMAKE_C_STANDARD=90 and CMAKE_C_EXTENSIONS=OFF partway through, so a
# reconfigure would apply them to deps it never touched; dropping them keeps it fresh-identical.
unset(CMAKE_C_STANDARD CACHE)
unset(CMAKE_C_EXTENSIONS CACHE)

# Aggregate -----------------------------------------------------------------
add_library(envy_thirdparty INTERFACE)
add_library(envy::thirdparty ALIAS envy_thirdparty)

target_link_libraries(envy_thirdparty
    INTERFACE
        envy::libgit2
        $<$<NOT:$<PLATFORM_ID:Windows>>:CURL::libcurl>
        libssh2::libssh2
        $<$<NOT:$<PLATFORM_ID:Windows>>:MbedTLS::mbedtls>
        $<$<NOT:$<PLATFORM_ID:Windows>>:MbedTLS::mbedx509>
        $<$<NOT:$<PLATFORM_ID:Windows>>:MbedTLS::mbedcrypto>
        ZLIB::ZLIB
        LibLZMA::LibLZMA
        BZip2::BZip2
        ${PLATFORM_NETWORK_LIBS}
        libarchive::libarchive
        lua::lua
        sol2::sol2
        blake3::blake3
        nanosemver::nanosemver
        picojson::picojson
        tomlc17::tomlc17
        aws-c-s3
)

if(APPLE)
    target_link_libraries(envy_thirdparty INTERFACE
        ${SYSTEMCONFIGURATION_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${CORESERVICES_FRAMEWORK}
        ${SECURITY_FRAMEWORK}
    )
endif()

target_compile_definitions(envy_thirdparty INTERFACE
    $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>>:CURL_STATICLIB>
    ENVY_AWS_C_S3_VERSION="${ENVY_AWS_C_S3_VERSION}"
)

target_include_directories(envy_thirdparty INTERFACE
    $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>,$<BOOL:${ENVY_LIBCURL_INCLUDE}>>:$<BUILD_INTERFACE:${ENVY_LIBCURL_INCLUDE}>>
    $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>,$<BOOL:${ENVY_LIBCURL_BINARY_INCLUDE}>>:$<BUILD_INTERFACE:${ENVY_LIBCURL_BINARY_INCLUDE}>>
)
