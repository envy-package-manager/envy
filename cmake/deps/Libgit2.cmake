if(WIN32)
    set(USE_HTTPS WinHTTP CACHE STRING "" FORCE)
else()
    set(USE_HTTPS mbedTLS CACHE STRING "" FORCE)
endif()
set(USE_GSSAPI OFF CACHE BOOL "" FORCE)
set(USE_SSH ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_LIBGIT2_ARCHIVE}" OUTPUT_VARIABLE _libgit2_archive)
set(_libgit2_url "${ENVY_LIBGIT2_URL}")
if(EXISTS "${_libgit2_archive}")
    file(TO_CMAKE_PATH "${_libgit2_archive}" _libgit2_archive_norm)
    set(_libgit2_url "file://${_libgit2_archive_norm}")
endif()

cmake_path(APPEND ENVY_CACHE_DIR "libgit2-src" OUTPUT_VARIABLE libgit2_SOURCE_DIR)
cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "libgit2-build" OUTPUT_VARIABLE libgit2_BINARY_DIR)

if(NOT EXISTS "${libgit2_SOURCE_DIR}/CMakeLists.txt")
    FetchContent_Populate(libgit2
        SOURCE_DIR "${libgit2_SOURCE_DIR}"
        BINARY_DIR "${libgit2_BINARY_DIR}"
        URL ${_libgit2_url}
        URL_HASH SHA256=${ENVY_LIBGIT2_SHA256}
    )
endif()

if(DEFINED libgit2_SOURCE_DIR AND DEFINED libgit2_BINARY_DIR AND
        DEFINED libssh2_SOURCE_DIR AND DEFINED libssh2_BINARY_DIR)
    # Upstream SelectSSH insists on pkg-config; patch it so our bundled
    # libssh2 target is used directly when present.
    envy_patch_libgit2_select("${libgit2_SOURCE_DIR}" "${libgit2_BINARY_DIR}" "${libssh2_SOURCE_DIR}" "${libssh2_BINARY_DIR}")
endif()
if(DEFINED libgit2_SOURCE_DIR AND DEFINED libgit2_BINARY_DIR)
    # Adjust FindStatNsec to apply Linux feature flags without poisoning other
    # platforms with strict POSIX macros.
    envy_patch_libgit2_nsec("${libgit2_SOURCE_DIR}" "${libgit2_BINARY_DIR}")
    envy_patch_libgit2_install("${libgit2_SOURCE_DIR}" "${libgit2_BINARY_DIR}")
    envy_patch_libgit2_cflags("${libgit2_SOURCE_DIR}" "${libgit2_BINARY_DIR}")
    envy_patch_libgit2_longpaths("${libgit2_SOURCE_DIR}" "${libgit2_BINARY_DIR}")
    envy_patch_libgit2_https_timeouts("${libgit2_SOURCE_DIR}" "${libgit2_BINARY_DIR}")
endif()

add_subdirectory(${libgit2_SOURCE_DIR} ${libgit2_BINARY_DIR})
foreach(_envy_libgit2_c_target IN ITEMS libgit2 libgit2package git2 http-parser ntlmclient ntlmclient_shared ntlmclient_static util)
    if(TARGET ${_envy_libgit2_c_target})
        set_property(TARGET ${_envy_libgit2_c_target} PROPERTY C_STANDARD 99)
    endif()
endforeach()
unset(_envy_libgit2_c_target)
if(TARGET libgit2package)
    add_library(envy::libgit2 ALIAS libgit2package)
    target_include_directories(libgit2package INTERFACE
        "$<BUILD_INTERFACE:${libgit2_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${libgit2_BINARY_DIR}/include>")
elseif(TARGET git2)
    add_library(envy::libgit2 ALIAS git2)
elseif(TARGET libgit2)
    add_library(envy::libgit2 ALIAS libgit2)
else()
    message(FATAL_ERROR "libgit2 target was not created by FetchContent")
endif()

foreach(_envy_git_target IN ITEMS libgit2package git2 libgit2)
    if(TARGET ${_envy_git_target})
        get_target_property(_envy_git_iface ${_envy_git_target} INTERFACE_LINK_LIBRARIES)
        if(_envy_git_iface)
            set(_envy_git_iface_filtered ${_envy_git_iface})
            # Libgit2 exports libssh2 and TLS backends both as targets and as raw archive paths; strip them so we don't pass duplicate libs downstream.
            list(REMOVE_ITEM _envy_git_iface_filtered
                libssh2::libssh2
                "${LIBSSH2_LIBRARY}"
                MbedTLS::mbedtls
                MbedTLS::mbedx509
                MbedTLS::mbedcrypto
                mbedtls
                mbedx509
                mbedcrypto)
            list(FILTER _envy_git_iface_filtered EXCLUDE REGEX "libssh2\\\\.a$")
            if(DEFINED MBEDTLS_LIBRARIES)
                foreach(_envy_mbed_lib IN LISTS MBEDTLS_LIBRARIES)
                    list(REMOVE_ITEM _envy_git_iface_filtered "${_envy_mbed_lib}")
                endforeach()
            endif()
            if(DEFINED MBEDTLS_LIBRARY_DIRS)
                foreach(_envy_mbed_libdir IN LISTS MBEDTLS_LIBRARY_DIRS)
                    list(FILTER _envy_git_iface_filtered EXCLUDE REGEX "${_envy_mbed_libdir}/libmbedtls\\\\.a$")
                    list(FILTER _envy_git_iface_filtered EXCLUDE REGEX "${_envy_mbed_libdir}/libmbedx509\\\\.a$")
                    list(FILTER _envy_git_iface_filtered EXCLUDE REGEX "${_envy_mbed_libdir}/libmbedcrypto\\\\.a$")
                endforeach()
            endif()
            set_target_properties(${_envy_git_target} PROPERTIES
                INTERFACE_LINK_LIBRARIES "${_envy_git_iface_filtered}")
        endif()
    endif()
endforeach()
unset(_envy_git_target)
unset(_envy_git_iface)

set(_envy_libgit2_warning_silencers
    $<$<COMPILE_LANG_AND_ID:C,AppleClang>:-Wno-declaration-after-statement>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang>:-Wno-unused-but-set-parameter>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang>:-Wno-single-bit-bitfield-constant-conversion>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang>:-Wno-array-parameter>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang>:-Wno-typedef-redefinition>
    $<$<COMPILE_LANG_AND_ID:C,MSVC>:/wd5287>
    # GCC+TSAN warning: atomic_thread_fence not supported with -fsanitize=thread
    # Only GCC emits this warning; Clang/AppleClang don't recognize -Wno-tsan
    $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-tsan>
    # libgit2 enables -Wc99-c11-compat; libssh2.h retypedefs credential.h's
    # forward typedefs (legal C11) in ssh_libssh2.c
    $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-c99-c11-compat>
)
foreach(_libgit2_target IN ITEMS libgit2 libgit2package util ntlmclient http-parser xdiff)
    if(TARGET ${_libgit2_target})
        target_compile_options(${_libgit2_target} PRIVATE ${_envy_libgit2_warning_silencers})
        # Ensure libgit2 sees the same mbedTLS config as mbedTLS library itself
        if(DEFINED MBEDTLS_USER_CONFIG_FILE)
            target_compile_definitions(${_libgit2_target} PRIVATE
                MBEDTLS_USER_CONFIG_FILE="${MBEDTLS_USER_CONFIG_FILE}")
        endif()
    endif()
endforeach()

# Bundled libssh2 includes must appear as -I (not -isystem) so they
# outrank any system-installed libssh2 in /usr/local/include.
if(DEFINED libssh2_SOURCE_DIR)
    foreach(_envy_git_target IN ITEMS libgit2 libgit2package git2)
        if(TARGET ${_envy_git_target})
            target_include_directories(${_envy_git_target} BEFORE PRIVATE
                "${libssh2_SOURCE_DIR}/include")
        endif()
    endforeach()
endif()

if(DEFINED envy_zlib_SOURCE_DIR AND DEFINED envy_zlib_BINARY_DIR)
    foreach(_envy_git_target IN ITEMS libgit2 libgit2package git2 util)
        if(TARGET ${_envy_git_target})
            target_include_directories(${_envy_git_target} PRIVATE
                "${envy_zlib_SOURCE_DIR}"
                "${envy_zlib_BINARY_DIR}")
        endif()
    endforeach()
endif()

unset(_libgit2_archive)
unset(_libgit2_archive_norm)
unset(_libgit2_url)
unset(_libgit2_target)
unset(_envy_libgit2_warning_silencers)
unset(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig)
