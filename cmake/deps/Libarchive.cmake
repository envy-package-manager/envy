set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)
set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA ON CACHE BOOL "" FORCE)
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
set(ENABLE_BZip2 ON CACHE BOOL "" FORCE)
set(ENABLE_ZSTD ON CACHE BOOL "" FORCE)
set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(POSIX_REGEX_LIB "NONE" CACHE STRING "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_ICONV OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
set(ENABLE_BSDXML OFF CACHE BOOL "" FORCE)
set(ENABLE_MD OFF CACHE BOOL "" FORCE)
set(ENABLE_CNG OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(LIBARCHIVE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_COMMONCRYPTO OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_LIBARCHIVE_ARCHIVE}" OUTPUT_VARIABLE _libarchive_archive)
set(_libarchive_url "${ENVY_LIBARCHIVE_URL}")
if(EXISTS "${_libarchive_archive}")
    file(TO_CMAKE_PATH "${_libarchive_archive}" _libarchive_archive_norm)
    set(_libarchive_url "file://${_libarchive_archive_norm}")
endif()

cmake_path(APPEND ENVY_CACHE_DIR "libarchive-src" OUTPUT_VARIABLE libarchive_SOURCE_DIR)
cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "libarchive-build" OUTPUT_VARIABLE libarchive_BINARY_DIR)

envy_populate_pinned(libarchive "${libarchive_SOURCE_DIR}" "${libarchive_BINARY_DIR}"
    ${_libarchive_url} ${ENVY_LIBARCHIVE_SHA256})

if(DEFINED libarchive_SOURCE_DIR AND DEFINED libarchive_BINARY_DIR)
    if(NOT WIN32)
        set(HAVE_STRUCT_TM_TM_GMTOFF 1 CACHE INTERNAL "" FORCE)
    else()
        set(HAVE_STRUCT_TM_TM_GMTOFF 0 CACHE INTERNAL "" FORCE)
    endif()
    set(HAVE_STRUCT_TM___TM_GMTOFF 0 CACHE INTERNAL "" FORCE)
    # Ensure libarchive sees sys/types.h during type-size probes so Linux
    # builds inherit the platform definitions instead of redefining POSIX
    # aliases like id_t.
    envy_patch_libarchive_cmakelists("${libarchive_SOURCE_DIR}" "${libarchive_BINARY_DIR}")

    # Pre-configure zstd detection for libarchive (CHECK_FUNCTION_EXISTS can't
    # link against targets that haven't been built yet, so we hint the variables)
    if(DEFINED ZSTD_LIBRARY AND DEFINED ZSTD_INCLUDE_DIR)
        set(HAVE_LIBZSTD 1 CACHE INTERNAL "" FORCE)
        set(HAVE_ZSTD_compressStream 1 CACHE INTERNAL "" FORCE)
        set(HAVE_ZSTD_minCLevel 1 CACHE INTERNAL "" FORCE)
    endif()
endif()

# Hide MBEDTLS_FOUND from libarchive - mbedTLS 3.x removed ripemd160 which libarchive needs
set(_envy_save_mbedtls_found ${MBEDTLS_FOUND})
unset(MBEDTLS_FOUND CACHE)
add_subdirectory(${libarchive_SOURCE_DIR} ${libarchive_BINARY_DIR})
set(MBEDTLS_FOUND ${_envy_save_mbedtls_found} CACHE BOOL "" FORCE)
unset(_envy_save_mbedtls_found)
unset(_libarchive_archive)
unset(_libarchive_archive_norm)
unset(_libarchive_cmake)
unset(_libarchive_sources)

if(TARGET archive)
    add_library(libarchive::libarchive ALIAS archive)
    target_include_directories(archive INTERFACE
        ${libarchive_SOURCE_DIR}/libarchive
        ${libarchive_BINARY_DIR}/libarchive)
    target_compile_definitions(archive INTERFACE LIBARCHIVE_STATIC)
elseif(TARGET libarchive)
    add_library(libarchive::libarchive ALIAS libarchive)
    target_include_directories(libarchive INTERFACE
        ${libarchive_SOURCE_DIR}/libarchive
        ${libarchive_BINARY_DIR}/libarchive)
    target_compile_definitions(libarchive INTERFACE LIBARCHIVE_STATIC)
elseif(TARGET archive_static)
    add_library(libarchive::libarchive ALIAS archive_static)
    target_include_directories(archive_static INTERFACE
        ${libarchive_SOURCE_DIR}/libarchive
        ${libarchive_BINARY_DIR}/libarchive)
    target_compile_definitions(archive_static INTERFACE LIBARCHIVE_STATIC)
else()
    message(FATAL_ERROR "libarchive target was not created by FetchContent")
endif()
