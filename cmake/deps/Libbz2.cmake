cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_LIBBZ2_ARCHIVE}" OUTPUT_VARIABLE _libbz2_archive)
set(_libbz2_url "${ENVY_LIBBZ2_URL}")
if(EXISTS "${_libbz2_archive}")
    file(TO_CMAKE_PATH "${_libbz2_archive}" _libbz2_archive_norm)
    set(_libbz2_url "file://${_libbz2_archive_norm}")
endif()

cmake_path(APPEND ENVY_CACHE_DIR "bzip2-src" OUTPUT_VARIABLE envy_libbz2_SOURCE_DIR)
cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "bzip2-build" OUTPUT_VARIABLE envy_libbz2_BINARY_DIR)

envy_populate_pinned(envy_libbz2 "${envy_libbz2_SOURCE_DIR}" "${envy_libbz2_BINARY_DIR}"
    ${_libbz2_url} ${ENVY_LIBBZ2_SHA256})

if(DEFINED envy_libbz2_SOURCE_DIR AND DEFINED envy_libbz2_BINARY_DIR)
    add_library(bz2 STATIC
        ${envy_libbz2_SOURCE_DIR}/blocksort.c
        ${envy_libbz2_SOURCE_DIR}/huffman.c
        ${envy_libbz2_SOURCE_DIR}/crctable.c
        ${envy_libbz2_SOURCE_DIR}/randtable.c
        ${envy_libbz2_SOURCE_DIR}/compress.c
        ${envy_libbz2_SOURCE_DIR}/decompress.c
        ${envy_libbz2_SOURCE_DIR}/bzlib.c
    )
    target_include_directories(bz2 PUBLIC ${envy_libbz2_SOURCE_DIR})
    set_target_properties(bz2 PROPERTIES OUTPUT_NAME bz2)

    set(_envy_libbz2_primary_target bz2)

    unset(BZIP2_LIBRARY CACHE)
    unset(BZIP2_LIBRARIES CACHE)

    set(BZIP2_LIBRARY "${_envy_libbz2_primary_target}" CACHE STRING "" FORCE)
    if(DEFINED envy_libbz2_SOURCE_DIR)
        set(BZIP2_INCLUDE_DIR "${envy_libbz2_SOURCE_DIR}" CACHE PATH "" FORCE)
        set(BZIP2_INCLUDE_DIRS "${BZIP2_INCLUDE_DIR}" CACHE STRING "" FORCE)
    endif()

    if(NOT TARGET BZip2::BZip2)
        add_library(BZip2::BZip2 INTERFACE IMPORTED)
    endif()
    set(_envy_libbz2_include_paths "")
    if(DEFINED envy_libbz2_SOURCE_DIR)
        list(APPEND _envy_libbz2_include_paths "${envy_libbz2_SOURCE_DIR}")
    endif()

    set_target_properties(BZip2::BZip2 PROPERTIES
        INTERFACE_LINK_LIBRARIES ${_envy_libbz2_primary_target}
        INTERFACE_INCLUDE_DIRECTORIES "${_envy_libbz2_include_paths}")

    set(BZIP2_LIBRARIES "${BZIP2_LIBRARY}" CACHE STRING "" FORCE)
    set(BZIP2_VERSION_STRING "${ENVY_LIBBZ2_VERSION}" CACHE STRING "" FORCE)
    set(BZIP2_FOUND TRUE CACHE BOOL "" FORCE)
    set(BZip2_FOUND TRUE CACHE BOOL "" FORCE)
endif()

unset(_envy_libbz2_include_paths)
unset(_envy_libbz2_primary_target)
unset(_libbz2_archive)
unset(_libbz2_archive_norm)
unset(_libbz2_url)
