cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_MBEDTLS_ARCHIVE}" OUTPUT_VARIABLE _mbedtls_archive)
set(_mbedtls_url "${ENVY_MBEDTLS_URL}")
if(EXISTS "${_mbedtls_archive}")
    file(TO_CMAKE_PATH "${_mbedtls_archive}" _mbedtls_archive_norm)
    set(_mbedtls_url "file://${_mbedtls_archive_norm}")
endif()

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)

cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "mbedtls-config" OUTPUT_VARIABLE _mbedtls_config_dir)
file(MAKE_DIRECTORY "${_mbedtls_config_dir}")
set(_mbedtls_user_config "${_mbedtls_config_dir}/envy_mbedtls_user_config.h")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../templates/mbedtls_user_config.h.in"
    "${_mbedtls_user_config}"
    @ONLY
)
set(MBEDTLS_USER_CONFIG_FILE "${_mbedtls_user_config}" CACHE PATH "" FORCE)

# Add src/mbedtls_alt to mbedtls include path so it can find threading_alt.h
cmake_path(APPEND CMAKE_CURRENT_SOURCE_DIR "src" "mbedtls_alt" OUTPUT_VARIABLE _mbedtls_alt_dir)
include_directories(BEFORE SYSTEM "${_mbedtls_alt_dir}")

FetchContent_Declare(mbedtls
    URL ${_mbedtls_url}
    URL_HASH SHA256=${ENVY_MBEDTLS_SHA256}
)
FetchContent_MakeAvailable(mbedtls)
FetchContent_GetProperties(mbedtls)

if(DEFINED mbedtls_SOURCE_DIR AND DEFINED mbedtls_BINARY_DIR)
    set(ENVY_MBEDTLS_SOURCE_DIR "${mbedtls_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(ENVY_MBEDTLS_BINARY_DIR "${mbedtls_BINARY_DIR}" CACHE PATH "" FORCE)
    set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    if(MSVC)
        set(_mbedtls_libprefix "")
        set(_mbedtls_libsuffix ".lib")
    else()
        set(_mbedtls_libprefix "lib")
        set(_mbedtls_libsuffix ".a")
    endif()
    cmake_path(APPEND mbedtls_BINARY_DIR "library" "${_mbedtls_libprefix}mbedcrypto${_mbedtls_libsuffix}" OUTPUT_VARIABLE _mbedcrypto_lib)
    set(MBEDCRYPTO_LIBRARY "${_mbedcrypto_lib}" CACHE FILEPATH "" FORCE)
    cmake_path(GET _mbedcrypto_lib PARENT_PATH _mbedtls_libdir)
    set(MBEDTLS_INCLUDE_DIRS "${MBEDTLS_INCLUDE_DIR}" CACHE STRING "" FORCE)
    set(MBEDTLS_LIBRARIES "${MBEDCRYPTO_LIBRARY}" CACHE STRING "" FORCE)
    cmake_path(APPEND _mbedtls_libdir "${_mbedtls_libprefix}mbedtls${_mbedtls_libsuffix}" OUTPUT_VARIABLE _mbedtls_lib)
    cmake_path(APPEND _mbedtls_libdir "${_mbedtls_libprefix}mbedx509${_mbedtls_libsuffix}" OUTPUT_VARIABLE _mbedx509_lib)
    set(MBEDTLS_LIBRARY "${_mbedtls_lib}" CACHE FILEPATH "" FORCE)
    set(MBEDX509_LIBRARY "${_mbedx509_lib}" CACHE FILEPATH "" FORCE)
    set(MBEDTLS_LIBRARY_DIRS "" CACHE STRING "" FORCE)
    set(MBEDTLS_LIBRARIES
        "${MBEDTLS_LIBRARY}"
        "${MBEDX509_LIBRARY}"
        "${MBEDCRYPTO_LIBRARY}"
        CACHE STRING "" FORCE)
    set(MBEDTLS_VERSION "${ENVY_MBEDTLS_VERSION}" CACHE STRING "" FORCE)
    unset(_mbedcrypto_lib)
    unset(_mbedtls_libdir)
    unset(_mbedtls_lib)
    unset(_mbedx509_lib)
    unset(_mbedtls_libprefix)
    unset(_mbedtls_libsuffix)
endif()

set(_envy_mbedtls_targets mbedtls mbedx509 mbedcrypto)
foreach(_envy_target IN LISTS _envy_mbedtls_targets)
    if(TARGET ${_envy_target})
        # Keep LTO off only under sanitizers (see AwsC.cmake); Release keeps
        # the global LTO so cross-module DCE can shrink mbedTLS.
        if(NOT ENVY_SANITIZER STREQUAL "NONE")
            set_property(TARGET ${_envy_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            # Clang uses -fsanitize-blacklist (configured globally in envy targets)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${_envy_target} PRIVATE -fno-sanitize=all)
        endif()
    endif()
endforeach()
unset(_envy_mbedtls_targets)
unset(_envy_target)

unset(_mbedtls_archive)
unset(_mbedtls_archive_norm)
unset(_mbedtls_url)
unset(_mbedtls_config_dir)
unset(_mbedtls_user_config)
unset(_mbedtls_alt_dir)
