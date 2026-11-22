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
set(ENVY_MBEDTLS_VERSION "3.6.8")
set(ENVY_MBEDTLS_ARCHIVE "mbedtls-${ENVY_MBEDTLS_VERSION}.tar.bz2")
set(ENVY_MBEDTLS_URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${ENVY_MBEDTLS_VERSION}/${ENVY_MBEDTLS_ARCHIVE}")
set(ENVY_MBEDTLS_SHA256 bc54b818e87f49c0b3281dd85d24adaede0e63b4e822e3fbd49c005a38b9595b)

set(ENVY_LIBSSH2_VERSION "1.11.1")
set(ENVY_LIBSSH2_ARCHIVE "libssh2-${ENVY_LIBSSH2_VERSION}.tar.gz")
set(ENVY_LIBSSH2_URL "https://www.libssh2.org/download/${ENVY_LIBSSH2_ARCHIVE}")
set(ENVY_LIBSSH2_SHA256 d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7)

set(ENVY_LIBGIT2_VERSION "1.9.7")
set(ENVY_LIBGIT2_ARCHIVE "libgit2-${ENVY_LIBGIT2_VERSION}.tar.gz")
set(ENVY_LIBGIT2_URL "https://github.com/libgit2/libgit2/archive/refs/tags/v${ENVY_LIBGIT2_VERSION}.tar.gz")
set(ENVY_LIBGIT2_SHA256 1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7)

set(ENVY_LIBCURL_VERSION "8.23.0")
set(ENVY_LIBCURL_ARCHIVE "curl-${ENVY_LIBCURL_VERSION}.tar.xz")
set(ENVY_LIBCURL_URL "https://curl.se/download/${ENVY_LIBCURL_ARCHIVE}")
set(ENVY_LIBCURL_SHA256 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5)

# S3 and the AWS C libraries beneath it: "<github org/repo> <tag> <archive sha256>".
# aws-lc and s2n-tls are Linux's TLS; macOS and Windows use the OS's.
set(ENVY_AWS_C_DEPS
    "awslabs/aws-c-common v0.14.5 103273767fea478545b75a0835c7dc60842baee0a191a112c72f904d22693c84"
    "aws/aws-lc v5.5.0 d79a5beb1c2f7fd86a17d91eb230ae12da71dc28bedeb775c179863cf279c650"
    "aws/s2n-tls v1.7.7 a6f77d2e3343b554bfc2cce27e0407b3c758bb105533c8f0a56e233036b7541d"
    "awslabs/aws-c-cal v0.9.15 215dd31c12ea49c4f40aa7882a800f9648e4095cfcb2d6abdd27e957574ad6e2"
    "awslabs/aws-c-io v0.27.7 cc84b1639c64af41ec7ea7397b020483f9bf73e3b07a72b16b4051b421079aa5"
    "awslabs/aws-checksums v0.2.11 6917e18b8d6079c02f36478ac59174eb3c47dc3bf040ea63bd93d127837f873f"
    "awslabs/aws-c-compression v0.3.3 33a91db709a547f417b1b23fdb76a64727ee8fb7ed88dd1a43be117f402db356"
    "awslabs/aws-c-http v0.11.1 2988843d5c95d92249d40e59480c2a4376533a91d8e38a5106dc4da5a8720ce5"
    "awslabs/aws-c-sdkutils v0.2.10 23bfb010c7a5becc48b7e36212a4f401319a46ec3e981eb9adb9d6b215e7a65e"
    "awslabs/aws-c-auth v0.10.5 1ddd1dc476debdbf9ff083e254396fdc3ea0846dccf7d5f983a6571303abcb35"
    "awslabs/aws-c-s3 v0.13.7 f15b0696d915a3189b4eee6b750fe8d1fbc84b33f94518071725a0f3a8ffc63d"
)

set(ENVY_LIBARCHIVE_VERSION "3.8.11")
set(ENVY_LIBARCHIVE_ARCHIVE "libarchive-${ENVY_LIBARCHIVE_VERSION}.tar.gz")
set(ENVY_LIBARCHIVE_URL "https://www.libarchive.org/downloads/libarchive-${ENVY_LIBARCHIVE_VERSION}.tar.gz")
set(ENVY_LIBARCHIVE_SHA256 900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b)

set(ENVY_BLAKE3_VERSION "1.8.5")
set(ENVY_BLAKE3_ARCHIVE "blake3-${ENVY_BLAKE3_VERSION}.tar.gz")
set(ENVY_BLAKE3_URL "https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/${ENVY_BLAKE3_VERSION}.tar.gz")
set(ENVY_BLAKE3_SHA256 220bd81286e2a0585beac66d41ac3f4c2c33ae8a4e339fc88cf22d5e00514fe9)

set(ENVY_LUA_VERSION "5.4.9")
set(ENVY_LUA_ARCHIVE "lua-${ENVY_LUA_VERSION}.tar.gz")
set(ENVY_LUA_URL "https://www.lua.org/ftp/lua-${ENVY_LUA_VERSION}.tar.gz")
set(ENVY_LUA_SHA256 2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6)

set(ENVY_SOL2_GIT_TAG "c1f95a773c6f8f4fde8ca3efe872e7286afe4444")

set(ENVY_ZLIB_VERSION "1.4")
set(ENVY_ZLIB_ARCHIVE "zlib-${ENVY_ZLIB_VERSION}.tar.gz")
set(ENVY_ZLIB_URL "https://github.com/madler/zlib/releases/download/v${ENVY_ZLIB_VERSION}/${ENVY_ZLIB_ARCHIVE}")
set(ENVY_ZLIB_SHA256 02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584)

set(ENVY_LIBLZMA_VERSION "5.8.4")
set(ENVY_LIBLZMA_ARCHIVE "xz-${ENVY_LIBLZMA_VERSION}.tar.gz")
set(ENVY_LIBLZMA_URL "https://github.com/tukaani-project/xz/releases/download/v${ENVY_LIBLZMA_VERSION}/${ENVY_LIBLZMA_ARCHIVE}")
set(ENVY_LIBLZMA_SHA256 0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9)

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
    ENVY_AWS_C_TLS="${ENVY_AWS_C_TLS}"
)

target_include_directories(envy_thirdparty INTERFACE
    $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>,$<BOOL:${ENVY_LIBCURL_INCLUDE}>>:$<BUILD_INTERFACE:${ENVY_LIBCURL_INCLUDE}>>
    $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>,$<BOOL:${ENVY_LIBCURL_BINARY_INCLUDE}>>:$<BUILD_INTERFACE:${ENVY_LIBCURL_BINARY_INCLUDE}>>
)
