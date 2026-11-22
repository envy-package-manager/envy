# Helper utilities for fetching third-party dependencies while
# preserving the repository's cache layout and offline workflows.

include(FetchContent)

# Download a single hash-pinned file into the cache, verifying its SHA-256.
#
# EXPECTED_HASH on file(DOWNLOAD) only validates a fresh download; an existing
# cached copy is never re-checked. So any stale file left from a previous pin
# (classically, one restored via CI cache `restore-keys` fallback) would be
# silently reused. Hash any existing copy and remove it on mismatch so the
# download path re-runs. When the cached copy already matches this is a no-op,
# keeping offline reconfigures fast.
#
#   envy_fetch_verified_file(
#       PATH <dest> URL <url> SHA256 <hex>
#       [HUMAN_NAME <label>]   # printed while downloading
#       [SHOW_PROGRESS])       # forwarded to file(DOWNLOAD)
function(envy_fetch_verified_file)
    set(options SHOW_PROGRESS)
    set(oneValueArgs PATH URL SHA256 HUMAN_NAME)
    cmake_parse_arguments(_evf "${options}" "${oneValueArgs}" "" ${ARGN})

    if(EXISTS "${_evf_PATH}")
        file(SHA256 "${_evf_PATH}" _evf_cached_hash)
        if(NOT _evf_cached_hash STREQUAL "${_evf_SHA256}")
            file(REMOVE "${_evf_PATH}")
        endif()
    endif()

    if(NOT EXISTS "${_evf_PATH}")
        if(_evf_HUMAN_NAME)
            message(STATUS "[envy] Downloading ${_evf_HUMAN_NAME}...")
        endif()
        set(_evf_progress "")
        if(_evf_SHOW_PROGRESS)
            set(_evf_progress SHOW_PROGRESS)
        endif()
        file(DOWNLOAD
            "${_evf_URL}"
            "${_evf_PATH}"
            EXPECTED_HASH SHA256=${_evf_SHA256}
            ${_evf_progress}
        )
    endif()
endfunction()

# Extracts a pinned archive into <source_dir> unless its .envy-pin names <sha256>. Any other
# tree (a re-pin, a CI `restore-keys` restore) is replaced, with <binary_dir>'s patch stamps.
function(envy_populate_pinned name source_dir binary_dir url sha256)
    set(_pin "${source_dir}/.envy-pin")
    if(EXISTS "${_pin}")
        file(READ "${_pin}" _have)
        if(_have STREQUAL sha256)
            return()
        endif()
    endif()
    if(FETCHCONTENT_FULLY_DISCONNECTED)
        message(FATAL_ERROR "[envy] ${source_dir} doesn't hold ${url} and fetching is disconnected")
    endif()
    # The archive is transient, as is out/cache/<name>-tmp, where FetchContent once kept it.
    cmake_path(GET url FILENAME _file)
    set(_dl "${CMAKE_BINARY_DIR}/_deps/${name}-archive")
    set(_archive "${_dl}/${_file}")
    envy_fetch_verified_file(PATH "${_archive}" URL "${url}" SHA256 ${sha256} HUMAN_NAME "${name}")
    set(_stage "${source_dir}.stage")
    file(REMOVE_RECURSE "${_stage}" "${source_dir}" "${binary_dir}" "${ENVY_CACHE_DIR}/${name}-tmp")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_stage}")
    file(GLOB _top LIST_DIRECTORIES TRUE "${_stage}/*")
    list(LENGTH _top _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "[envy] ${_archive} should hold one top-level directory")
    endif()
    file(RENAME "${_top}" "${source_dir}")
    file(REMOVE_RECURSE "${_stage}" "${_dl}")
    file(WRITE "${_pin}" "${sha256}")
endfunction()

function(envy_fetchcontent_acquire name human_name)
    set(options MAKE_AVAILABLE)
    set(oneValueArgs OUT_WAS_POPULATED)
    cmake_parse_arguments(_envy "${options}" "${oneValueArgs}" "" ${ARGN})

    string(TOLOWER "${name}" _envy_lower)
    cmake_path(APPEND FETCHCONTENT_BASE_DIR "${_envy_lower}-src" OUTPUT_VARIABLE _envy_source_dir)
    cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "${_envy_lower}-build" OUTPUT_VARIABLE _envy_binary_dir)

    set(_envy_prev_defined FALSE)
    if(DEFINED FETCHCONTENT_FULLY_DISCONNECTED)
        set(_envy_prev_defined TRUE)
        set(_envy_prev_value "${FETCHCONTENT_FULLY_DISCONNECTED}")
    endif()

    set(_envy_have_cached_sources FALSE)
    if(EXISTS "${_envy_source_dir}")
        set(_envy_have_cached_sources TRUE)
        set(FETCHCONTENT_FULLY_DISCONNECTED ON)
    endif()

    FetchContent_GetProperties(${name})
    set(_envy_was_populated FALSE)
    if(DEFINED ${name}_POPULATED)
        set(_envy_was_populated "${${name}_POPULATED}")
    endif()

    if(_envy_have_cached_sources AND NOT _envy_was_populated)
        message(STATUS "[envy] Reusing cached ${human_name} sources at ${_envy_source_dir}")
    elseif(NOT _envy_was_populated)
        message(STATUS "[envy] No cached ${human_name} sources at ${_envy_source_dir}; fetching")
    endif()

    if(NOT DEFINED ${name}_SOURCE_DIR)
        string(TOLOWER "${name}" _envy_lower_auto)
        cmake_path(APPEND FETCHCONTENT_BASE_DIR "${_envy_lower_auto}-src" OUTPUT_VARIABLE ${name}_SOURCE_DIR)
        cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "${_envy_lower_auto}-build" OUTPUT_VARIABLE ${name}_BINARY_DIR)
        unset(_envy_lower_auto)
    endif()

    FetchContent_Populate(${name}
        SOURCE_DIR "${${name}_SOURCE_DIR}"
        BINARY_DIR "${${name}_BINARY_DIR}"
    )
    FetchContent_GetProperties(${name})

    if(_envy_prev_defined)
        set(FETCHCONTENT_FULLY_DISCONNECTED "${_envy_prev_value}")
    else()
        unset(FETCHCONTENT_FULLY_DISCONNECTED)
    endif()

    if(_envy_MAKE_AVAILABLE)
        FetchContent_MakeAvailable(${name})
    endif()

    set(${name}_SOURCE_DIR "${_envy_source_dir}" PARENT_SCOPE)
    set(${name}_BINARY_DIR "${_envy_binary_dir}" PARENT_SCOPE)
    if(DEFINED ${name}_POPULATED)
        set(${name}_POPULATED "${${name}_POPULATED}" PARENT_SCOPE)
    endif()

    if(_envy_OUT_WAS_POPULATED)
        set(${_envy_OUT_WAS_POPULATED} ${_envy_was_populated} PARENT_SCOPE)
    endif()

    unset(_envy_lower)
    unset(_envy_source_dir)
    unset(_envy_binary_dir)
    unset(_envy_prev_defined)
    unset(_envy_prev_value)
    unset(_envy_have_cached_sources)
    unset(_envy_was_populated)
    unset(_envy_MAKE_AVAILABLE)
    unset(_envy_OUT_WAS_POPULATED)
endfunction()
