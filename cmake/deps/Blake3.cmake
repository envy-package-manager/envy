cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_BLAKE3_ARCHIVE}" OUTPUT_VARIABLE _blake3_archive)
set(_blake3_url "${ENVY_BLAKE3_URL}")
if(EXISTS "${_blake3_archive}")
    file(TO_CMAKE_PATH "${_blake3_archive}" _blake3_archive_norm)
    set(_blake3_url "file://${_blake3_archive_norm}")
endif()

cmake_path(APPEND ENVY_CACHE_DIR "blake3-src" OUTPUT_VARIABLE blake3_SOURCE_DIR)
cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "blake3-build" OUTPUT_VARIABLE blake3_BINARY_DIR)

envy_populate_pinned(blake3 "${blake3_SOURCE_DIR}" "${blake3_BINARY_DIR}"
    ${_blake3_url} ${ENVY_BLAKE3_SHA256})

if(NOT TARGET blake3::blake3)
    set(BLAKE3_SOURCES
        "${blake3_SOURCE_DIR}/c/blake3.c"
        "${blake3_SOURCE_DIR}/c/blake3_dispatch.c"
        "${blake3_SOURCE_DIR}/c/blake3_portable.c"
    )
    # Detect target architecture: MSVC cross-compiles report the host in
    # CMAKE_SYSTEM_PROCESSOR, so prefer CMAKE_C_COMPILER_ARCHITECTURE_ID
    # which always reflects the *target* (matching official BLAKE3 CMake).
    set(_blake3_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(MSVC AND DEFINED CMAKE_C_COMPILER_ARCHITECTURE_ID)
        set(_blake3_arch "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
    endif()

    if(_blake3_arch MATCHES "([Xx]86_64|AMD64|amd64|[Xx]64)")
        if(MSVC)
            enable_language(ASM_MASM)
            if(NOT CMAKE_ASM_MASM_FLAGS MATCHES "/nologo")
                set(CMAKE_ASM_MASM_FLAGS "${CMAKE_ASM_MASM_FLAGS} /nologo" CACHE STRING "" FORCE)
            endif()
            list(APPEND BLAKE3_SOURCES
                "${blake3_SOURCE_DIR}/c/blake3_sse2_x86-64_windows_msvc.asm"
                "${blake3_SOURCE_DIR}/c/blake3_sse41_x86-64_windows_msvc.asm"
                "${blake3_SOURCE_DIR}/c/blake3_avx2_x86-64_windows_msvc.asm"
                "${blake3_SOURCE_DIR}/c/blake3_avx512_x86-64_windows_msvc.asm"
            )
        elseif(WIN32)
            enable_language(ASM)
            list(APPEND BLAKE3_SOURCES
                "${blake3_SOURCE_DIR}/c/blake3_sse2_x86-64_windows_gnu.S"
                "${blake3_SOURCE_DIR}/c/blake3_sse41_x86-64_windows_gnu.S"
                "${blake3_SOURCE_DIR}/c/blake3_avx2_x86-64_windows_gnu.S"
                "${blake3_SOURCE_DIR}/c/blake3_avx512_x86-64_windows_gnu.S"
            )
        else()
            enable_language(ASM)
            list(APPEND BLAKE3_SOURCES
                "${blake3_SOURCE_DIR}/c/blake3_sse2_x86-64_unix.S"
                "${blake3_SOURCE_DIR}/c/blake3_sse41_x86-64_unix.S"
                "${blake3_SOURCE_DIR}/c/blake3_avx2_x86-64_unix.S"
                "${blake3_SOURCE_DIR}/c/blake3_avx512_x86-64_unix.S"
            )
        endif()
    elseif(_blake3_arch MATCHES "([Aa][Rr][Mm]64|aarch64)")
        list(APPEND BLAKE3_SOURCES "${blake3_SOURCE_DIR}/c/blake3_neon.c")
    endif()
    unset(_blake3_arch)
    add_library(blake3 STATIC ${BLAKE3_SOURCES})
    target_include_directories(blake3 PUBLIC "${blake3_SOURCE_DIR}/c")
    target_compile_features(blake3 PUBLIC c_std_99)
    add_library(blake3::blake3 ALIAS blake3)
endif()
unset(_blake3_archive)
unset(_blake3_archive_norm)
unset(_blake3_url)
