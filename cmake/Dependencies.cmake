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

set(ENVY_LIBGIT2_VERSION "1.9.6")
set(ENVY_LIBGIT2_ARCHIVE "libgit2-${ENVY_LIBGIT2_VERSION}.tar.gz")
set(ENVY_LIBGIT2_URL "https://github.com/libgit2/libgit2/archive/refs/tags/v${ENVY_LIBGIT2_VERSION}.tar.gz")
set(ENVY_LIBGIT2_SHA256 a88a42a4ea9bdab7aa8686eead3bf7d9c6dd74529caca16ab22eaa92433d31d9)

set(ENVY_LIBCURL_VERSION "8.21.0")
set(ENVY_LIBCURL_ARCHIVE "curl-${ENVY_LIBCURL_VERSION}.tar.xz")
set(ENVY_LIBCURL_URL "https://curl.se/download/${ENVY_LIBCURL_ARCHIVE}")
set(ENVY_LIBCURL_SHA256 aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6)

set(ENVY_AWS_SDK_VERSION "1.11.850")
set(ENVY_AWS_SDK_ARCHIVE "aws-sdk-cpp-${ENVY_AWS_SDK_VERSION}.zip")
set(ENVY_AWS_SDK_URL "https://github.com/aws/aws-sdk-cpp/archive/refs/tags/${ENVY_AWS_SDK_VERSION}.zip")
set(ENVY_AWS_SDK_SHA256 980e32369e9cb6d2dea70b68e78faf35a5380608088fc1e7193a9620a3bf4964)

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

set(ENVY_SEMVER_VERSION "1.0.0-rc")
set(ENVY_SEMVER_URL "https://raw.githubusercontent.com/Neargye/semver/v${ENVY_SEMVER_VERSION}/include/semver.hpp")
set(ENVY_SEMVER_SHA256 af2c0c53124dc7f52c58a7205e458ad3efbac2f61ce55addf9c8f94338a04182)
set(ENVY_SEMVER_LICENSE_URL "https://raw.githubusercontent.com/Neargye/semver/v${ENVY_SEMVER_VERSION}/LICENSE")
set(ENVY_SEMVER_LICENSE_SHA256 9cc0e3435da3c8bdff3bb984929759a6018b5f3eae666c9424be90fe27cffa5a)

set(ENVY_PICOJSON_VERSION "1.3.0")
set(ENVY_PICOJSON_URL "https://raw.githubusercontent.com/kazuho/picojson/v${ENVY_PICOJSON_VERSION}/picojson.h")
set(ENVY_PICOJSON_SHA256 5ddf7276d04926da7be243e7af49258e78cc27278ee9097ba45b942c7a6b5f9d)
set(ENVY_PICOJSON_LICENSE_URL "https://raw.githubusercontent.com/kazuho/picojson/v${ENVY_PICOJSON_VERSION}/LICENSE")
set(ENVY_PICOJSON_LICENSE_SHA256 5585fe141cc7bb08c953f3859db608852968d0bbc625b9b6d95c0bd6349bacb6)

set(ENVY_DOCTEST_VERSION "2.5.3")
set(ENVY_DOCTEST_URL "https://raw.githubusercontent.com/doctest/doctest/v${ENVY_DOCTEST_VERSION}/doctest/doctest.h")
set(ENVY_DOCTEST_SHA256 cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421)

set(PLATFORM_NETWORK_LIBS)
if(WIN32)
    set(PLATFORM_NETWORK_LIBS ws2_32 dnsapi iphlpapi advapi32 crypt32 wldap32 winhttp bcrypt wininet)
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
include("${CMAKE_CURRENT_LIST_DIR}/deps/AwsSdk.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Libarchive.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Blake3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Lua.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Sol2.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Semver.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Picojson.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/Doctest.cmake")

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
        semver::semver
        picojson::picojson
        AWS::aws-cpp-sdk-s3
        AWS::aws-cpp-sdk-transfer
        AWS::aws-cpp-sdk-sso
        AWS::aws-cpp-sdk-sso-oidc
        aws-crt-cpp
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
)

target_include_directories(envy_thirdparty INTERFACE
    "$<BUILD_INTERFACE:${aws_sdk_SOURCE_DIR}/aws-cpp-sdk-core/include>"
    "$<BUILD_INTERFACE:${aws_sdk_SOURCE_DIR}/src/aws-cpp-sdk-core/include>"
    "$<BUILD_INTERFACE:${aws_sdk_BINARY_DIR}/generated/src/aws-cpp-sdk-core/include>"
    "$<BUILD_INTERFACE:${ENVY_AWSCRT_ROOT}/include>"
    $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>,$<BOOL:${ENVY_LIBCURL_INCLUDE}>>:$<BUILD_INTERFACE:${ENVY_LIBCURL_INCLUDE}>>
    $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>,$<BOOL:${ENVY_LIBCURL_BINARY_INCLUDE}>>:$<BUILD_INTERFACE:${ENVY_LIBCURL_BINARY_INCLUDE}>>
)
