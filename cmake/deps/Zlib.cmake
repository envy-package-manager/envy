cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_ZLIB_ARCHIVE}" OUTPUT_VARIABLE _zlib_archive)
set(_zlib_url "${ENVY_ZLIB_URL}")
if(EXISTS "${_zlib_archive}")
    file(TO_CMAKE_PATH "${_zlib_archive}" _zlib_archive_norm)
    set(_zlib_url "file://${_zlib_archive_norm}")
endif()

set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(envy_zlib
    URL ${_zlib_url}
    URL_HASH SHA256=${ENVY_ZLIB_SHA256}
)
FetchContent_MakeAvailable(envy_zlib)
FetchContent_GetProperties(envy_zlib)

# Exclude shared library from build (we only need static)
if(TARGET zlib)
    set_target_properties(zlib PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(DEFINED envy_zlib_BINARY_DIR)
    set(_envy_zlib_primary_target "")
    if(TARGET zlibstatic)
        set(_envy_zlib_primary_target zlibstatic)
    elseif(TARGET zlib)
        set(_envy_zlib_primary_target zlib)
    endif()

    if(_envy_zlib_primary_target STREQUAL "")
        message(FATAL_ERROR "Failed to locate zlib target after FetchContent_MakeAvailable")
    endif()

    get_target_property(_envy_zlib_output_name ${_envy_zlib_primary_target} OUTPUT_NAME)
    if(NOT _envy_zlib_output_name)
        set(_envy_zlib_output_name z)
    endif()

    unset(ZLIB_LIBRARY CACHE)
    unset(ZLIB_LIBRARIES CACHE)

    set(ZLIB_LIBRARY "${_envy_zlib_primary_target}" CACHE STRING "" FORCE)
    if(DEFINED envy_zlib_SOURCE_DIR)
        set(ZLIB_INCLUDE_DIR "${envy_zlib_SOURCE_DIR}" CACHE PATH "" FORCE)
        set(ZLIB_INCLUDE_DIRS "${envy_zlib_SOURCE_DIR};${envy_zlib_BINARY_DIR}" CACHE STRING "" FORCE)
    endif()

    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB INTERFACE IMPORTED)
    endif()
    set(_envy_zlib_include_paths "")
    if(DEFINED envy_zlib_SOURCE_DIR)
        list(APPEND _envy_zlib_include_paths "${envy_zlib_SOURCE_DIR}")
    endif()
    list(APPEND _envy_zlib_include_paths "${envy_zlib_BINARY_DIR}")

    set_target_properties(ZLIB::ZLIB PROPERTIES
        INTERFACE_LINK_LIBRARIES ${_envy_zlib_primary_target}
        INTERFACE_INCLUDE_DIRECTORIES "${_envy_zlib_include_paths}")

    set(ZLIB_LIBRARIES "${ZLIB_LIBRARY}" CACHE STRING "" FORCE)
    set(ZLIB_VERSION_STRING "${ENVY_ZLIB_VERSION}" CACHE STRING "" FORCE)

    if(DEFINED envy_zlib_SOURCE_DIR AND EXISTS "${envy_zlib_BINARY_DIR}/zconf.h")
        # zlib's (1.3.2+) generated zconf.h hard-defines HAVE_UNISTD_H/HAVE_STDARG_H;
        # guard them so consumers whose config headers define these valueless
        # (libssh2_config.h) don't trip macro-redefinition warnings.
        file(READ "${envy_zlib_BINARY_DIR}/zconf.h" _envy_zconf)
        foreach(_envy_zconf_macro HAVE_UNISTD_H HAVE_STDARG_H)
            string(REPLACE
                "#define ${_envy_zconf_macro} 1"
                "#ifndef ${_envy_zconf_macro}\n#define ${_envy_zconf_macro} 1\n#endif"
                _envy_zconf "${_envy_zconf}")
        endforeach()
        file(WRITE "${envy_zlib_BINARY_DIR}/zconf.h.envy" "${_envy_zconf}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${envy_zlib_BINARY_DIR}/zconf.h.envy" "${envy_zlib_SOURCE_DIR}/zconf.h")
        unset(_envy_zconf)
        unset(_envy_zconf_macro)
    endif()
endif()

unset(_envy_zlib_include_paths)
unset(_envy_zlib_primary_target)
unset(_envy_zlib_output_name)
unset(_zlib_archive)
unset(_zlib_archive_norm)
unset(_zlib_url)
