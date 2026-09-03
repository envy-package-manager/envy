cmake_minimum_required(VERSION 4.1.2)

# Version can be overridden via -DENVY_VERSION=x.y.z; defaults to 0.0.0 for local builds
if(NOT DEFINED ENVY_VERSION)
    set(ENVY_VERSION "0.0.0")
endif()

project(envy
    VERSION ${ENVY_VERSION}
    DESCRIPTION "Unopionated adaptive package manager"
    LANGUAGES C CXX)

list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/modules")

if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()

if(POLICY CMP0194)
    cmake_policy(SET CMP0194 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0194 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Python3 3.13 REQUIRED COMPONENTS Interpreter)

if(MSVC)
    enable_language(ASM_MASM)
    if(NOT CMAKE_ASM_MASM_COMPILER)
        message(FATAL_ERROR "MASM assembler was not detected")
    endif()
    set(CMAKE_ASM_COMPILER "${CMAKE_ASM_MASM_COMPILER}" CACHE FILEPATH "" FORCE)
    set(CMAKE_ASM_MASM_FLAGS "/nologo" CACHE STRING "" FORCE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
    set(_envy_msvc_flag_vars
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS_MINSIZEREL
    )
    foreach(_envy_flag_var IN LISTS _envy_msvc_flag_vars)
        if(DEFINED ${_envy_flag_var})
            string(REGEX REPLACE "/M[TD]d?" "" _envy_sanitized_flags "${${_envy_flag_var}}")
            string(STRIP "${_envy_sanitized_flags}" _envy_sanitized_flags)
            set(${_envy_flag_var} "${_envy_sanitized_flags}" CACHE STRING "" FORCE)
        endif()
    endforeach()
    unset(_envy_msvc_flag_vars)
    unset(_envy_flag_var)
    unset(_envy_sanitized_flags)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

# Favor size-optimized release binaries by normalizing optimization flags.
# MSVC: /O1 = minimize size (/Os /Oy /Ob2 /GF /Gy).  /Os alone is just a
# preference flag that has no effect without /O1 or /O2; MSVC has no /Oz.
# -Oz over -Os: measured -18.6% binary on arm64 with no runtime regression -- the
# hot loops are hardware-SHA and zstd, which -Oz does not touch.  Needs clang or
# GCC 12+, which every CI builder has; it constrains builders, not run hosts.
set(_envy_size_opt "-Oz")
if(MSVC)
    set(_envy_size_opt "/O1")
endif()

foreach(_envy_lang IN ITEMS C CXX)
    set(_envy_flags "${CMAKE_${_envy_lang}_FLAGS_RELEASE}")
    string(REGEX REPLACE "-O[0-9A-Za-z]*" "" _envy_flags "${_envy_flags}")
    string(REGEX REPLACE "/O[0-9A-Za-z]*" "" _envy_flags "${_envy_flags}")
    string(STRIP "${_envy_flags}" _envy_flags)
    if(_envy_flags STREQUAL "")
        set(_envy_flags "${_envy_size_opt}")
    else()
        string(APPEND _envy_flags " ${_envy_size_opt}")
    endif()
    set(CMAKE_${_envy_lang}_FLAGS_RELEASE "${_envy_flags}" CACHE STRING "Release flags" FORCE)
endforeach()
unset(_envy_size_opt)
unset(_envy_flags)
unset(_envy_lang)

add_compile_options(
    $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-ffunction-sections>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-ffunction-sections>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-fdata-sections>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-fdata-sections>
)

option(ENABLE_LTO "Enable interprocedural optimization" ON)
option(ENVY_BUILD_ENVY "Build the main envy executable" ON)
option(ENVY_BUILD_FUNCTIONAL_TESTER "Build the functional test executable" ON)
set(ENVY_SANITIZER "NONE" CACHE STRING "Sanitizer configuration (NONE, ASAN, UBSAN, TSAN)")
set_property(CACHE ENVY_SANITIZER PROPERTY STRINGS NONE ASAN UBSAN TSAN)
if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
        # Explicitly add LTO flags to linker for all compilers
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            foreach(_type RELEASE RELWITHDEBINFO MINSIZEREL)
                if(NOT "${CMAKE_EXE_LINKER_FLAGS_${_type}}" MATCHES "-flto")
                    set(CMAKE_EXE_LINKER_FLAGS_${_type} "${CMAKE_EXE_LINKER_FLAGS_${_type}} -flto=thin" CACHE STRING "${_type} exe linker flags" FORCE)
                endif()
                if(NOT "${CMAKE_SHARED_LINKER_FLAGS_${_type}}" MATCHES "-flto")
                    set(CMAKE_SHARED_LINKER_FLAGS_${_type} "${CMAKE_SHARED_LINKER_FLAGS_${_type}} -flto=thin" CACHE STRING "${_type} shared linker flags" FORCE)
                endif()
            endforeach()
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            foreach(_type RELEASE RELWITHDEBINFO MINSIZEREL)
                if(NOT "${CMAKE_EXE_LINKER_FLAGS_${_type}}" MATCHES "-flto")
                    set(CMAKE_EXE_LINKER_FLAGS_${_type} "${CMAKE_EXE_LINKER_FLAGS_${_type}} -flto" CACHE STRING "${_type} exe linker flags" FORCE)
                endif()
                if(NOT "${CMAKE_SHARED_LINKER_FLAGS_${_type}}" MATCHES "-flto")
                    set(CMAKE_SHARED_LINKER_FLAGS_${_type} "${CMAKE_SHARED_LINKER_FLAGS_${_type}} -flto" CACHE STRING "${_type} shared linker flags" FORCE)
                endif()
            endforeach()
        elseif(MSVC)
            foreach(_type RELEASE RELWITHDEBINFO MINSIZEREL)
                if(NOT "${CMAKE_EXE_LINKER_FLAGS_${_type}}" MATCHES "/LTCG")
                    set(CMAKE_EXE_LINKER_FLAGS_${_type} "${CMAKE_EXE_LINKER_FLAGS_${_type}} /LTCG" CACHE STRING "${_type} exe linker flags" FORCE)
                endif()
                if(NOT "${CMAKE_SHARED_LINKER_FLAGS_${_type}}" MATCHES "/LTCG")
                    set(CMAKE_SHARED_LINKER_FLAGS_${_type} "${CMAKE_SHARED_LINKER_FLAGS_${_type}} /LTCG" CACHE STRING "${_type} shared linker flags" FORCE)
                endif()
            endforeach()
        endif()
    else()
        message(WARNING "IPO/LTO not supported: ${ipo_output}")
    endif()
endif()

include(cmake/Dependencies.cmake)
include(cmake/EmbedResource.cmake)

# Extract shell hook version from shell_hooks.h (single source of truth)
file(STRINGS src/shell_hooks.h _hook_ver_line REGEX "kShellHookVersion = [0-9]+")
if(NOT _hook_ver_line)
    message(FATAL_ERROR "Failed to extract kShellHookVersion from src/shell_hooks.h")
endif()
string(REGEX MATCH "[0-9]+" ENVY_SHELL_HOOK_VERSION "${_hook_ver_line}")

# Extract product script version from deploy.h (single source of truth)
file(STRINGS src/deploy.h _prod_script_ver_line REGEX "kProductScriptVersion = [0-9]+")
if(NOT _prod_script_ver_line)
    message(FATAL_ERROR "Failed to extract kProductScriptVersion from src/deploy.h")
endif()
string(REGEX MATCH "[0-9]+" ENVY_PRODUCT_SCRIPT_VERSION "${_prod_script_ver_line}")

# Both versions above are read at configure time and stamped into the embedded scripts, while
# the C++ that compares against them reads the headers directly. Without this, bumping either
# constant leaves an incremental build's resources stamped with the old number, so `ensure`
# rewrites a hook and then decides the result is still stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    src/shell_hooks.h
    src/deploy.h
)

# Generate embedded resource header (all platform variants unconditionally)
embed_resources(envy_embedded_resources
    OUTPUT embedded_init_resources.h
    NORMALIZE_EOL
    COMPRESS
    DEFINES
        ENVY_HOOK_VERSION=${ENVY_SHELL_HOOK_VERSION}
        ENVY_PRODUCT_SCRIPT_VERSION=${ENVY_PRODUCT_SCRIPT_VERSION}
    RESOURCES
        bootstrap_posix=src/resources/envy
        bootstrap_windows=src/resources/envy.bat
        manifest_template=src/resources/manifest_template.lua
        type_definitions=src/resources/envy.lua
        luarc_template=src/resources/luarc_template.json
        product_script_posix=src/resources/product_script
        product_script_windows=src/resources/product_script.bat
        shell_hook_bash=src/resources/shell_hook.bash
        shell_hook_zsh=src/resources/shell_hook.zsh
        shell_hook_fish=src/resources/shell_hook.fish
        shell_hook_ps1=src/resources/shell_hook.ps1
)

# Discover and compress third-party licenses for embedding
include(cmake/FindLicenses.cmake)

# Note: Doctest is a single-header file with its license embedded in comments.
# All other dependencies have separate license files that are discovered automatically.
set(ENVY_LICENSE_SOURCES
    "envy=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "AWS SDK for C++=${aws_sdk_SOURCE_DIR}"
    "BLAKE3=${blake3_SOURCE_DIR}"
    "bzip2=${envy_libbz2_SOURCE_DIR}"
    "libarchive=${libarchive_SOURCE_DIR}"
    "libgit2=${libgit2_SOURCE_DIR}"
    "liblzma=${envy_liblzma_SOURCE_DIR}"
    "libssh2=${libssh2_SOURCE_DIR}"
    "libzstd=${envy_libzstd_SOURCE_DIR}"
    "Lua=${lua_SOURCE_DIR}"
    "picojson=${PICOJSON_CACHE_DIR}"
    "Semver=${SEMVER_CACHE_DIR}"
    "Sol2=${sol2_SOURCE_DIR}"
    "zlib=${envy_zlib_SOURCE_DIR}"
)

# libcurl only bundled on Linux (macOS uses system libcurl, Windows uses WinINet)
if(NOT APPLE AND NOT WIN32)
    list(APPEND ENVY_LICENSE_SOURCES "libcurl=${libcurl_SOURCE_DIR}")
endif()

# mbedTLS only used on non-Windows
if(NOT WIN32)
    list(APPEND ENVY_LICENSE_SOURCES "mbedTLS=${mbedtls_SOURCE_DIR}")
endif()

find_and_collect_licenses(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/licenses.txt"
    SOURCES ${ENVY_LICENSE_SOURCES}
)

# Embed compressed licenses
embed_resources(envy_embedded_licenses
    OUTPUT embedded_licenses.h
    COMPRESS
    RESOURCES
        licenses=${CMAKE_CURRENT_BINARY_DIR}/generated/licenses.txt
)
add_dependencies(envy_embedded_licenses envy_licenses_collected)

# Common source files shared by all envy executables (main added separately)
set(ENVY_BASE_SOURCES
    src/bootstrap.cpp
    src/cache.cpp
    src/cli.cpp
    src/cli_parse.cpp
    src/cmd.cpp
    src/cmds/cmd_cache.cpp
    src/cmds/cmd_deploy.cpp
    src/cmds/cmd_export.cpp
    src/cmds/cmd_extract.cpp
    src/cmds/cmd_fetch.cpp
    src/cmds/cmd_git_resolve.cpp
    src/cmds/cmd_import.cpp
    src/cmds/cmd_package.cpp
    src/cmds/cmd_hash.cpp
    src/cmds/cmd_init.cpp
    src/cmds/cmd_install.cpp
    src/cmds/cmd_lua.cpp
    src/cmds/cmd_merge_depot.cpp
    src/cmds/cmd_mirror_envy.cpp
    src/cmds/cmd_product.cpp
    src/cmds/cmd_run.cpp
    src/cmds/cmd_sync.cpp
    src/cmds/cmd_shell.cpp
    src/cmds/cmd_use.cpp
    src/cmds/cmd_version.cpp
    src/deploy.cpp
    src/engine.cpp
    src/task_engine.cpp
    src/pkg_phase.cpp
    src/pkg_key.cpp
    src/lua_ctx/lua_envy_dep_util.cpp
    src/lua_ctx/lua_envy_extract.cpp
    src/lua_ctx/lua_envy_fetch.cpp
    src/lua_ctx/lua_envy_file_ops.cpp
    src/lua_ctx/lua_envy_import.cpp
    src/lua_ctx/lua_envy_loadenv_spec.cpp
    src/lua_ctx/lua_envy_options.cpp
    src/lua_ctx/lua_envy_package.cpp
    src/lua_ctx/lua_envy_path.cpp
    src/lua_ctx/lua_envy_product.cpp
    src/lua_ctx/lua_envy_run.cpp
    src/lua_ctx/lua_phase_context.cpp
    src/product_util.cpp
    src/reexec.cpp
    src/self_deploy.cpp
    src/phases/phase_build.cpp
    src/phases/phase_check.cpp
    src/phases/phase_completion.cpp
    src/phases/phase_export.cpp
    src/phases/phase_fetch.cpp
    src/phases/phase_import.cpp
    src/phases/phase_install.cpp
    src/phases/phase_setup.cpp
    src/phases/phase_spec_fetch.cpp
    src/phases/phase_stage.cpp
    src/luarc.cpp
    src/extract.cpp
    src/aws_util.cpp
    src/envy_release.cpp
    src/blake3_util.cpp
    src/git_resolve.cpp
    src/libgit2_util.cpp
    src/sol_util.cpp
    src/lua_shell.cpp
    src/lua_envy.cpp
    src/lua_error_formatter.cpp
    src/manifest.cpp
    src/package_depot.cpp
    src/pkg_cfg.cpp
    src/bundle.cpp
    src/spec_util.cpp
    $<$<PLATFORM_ID:Windows>:src/sha256_win.cpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/sha256_mbedtls.cpp>
    $<$<PLATFORM_ID:Darwin>:src/ssl_certs_macos.mm>
    src/fetch.cpp
    src/fetch_error.cpp
    $<$<PLATFORM_ID:Windows>:src/fetch_http_win32.cpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/fetch_http_curl.cpp>
    src/platform.cpp
    $<$<PLATFORM_ID:Windows>:src/platform_win.cpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/platform_posix.cpp>
    src/termination.cpp
    src/shell.cpp
    src/shell_hooks.cpp
    $<$<PLATFORM_ID:Windows>:src/shell_win.cpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/shell_posix.cpp>
    src/trace.cpp
    src/tui.cpp
    src/tui_actions.cpp
    src/uri.cpp
    src/util.cpp
    src/version.cpp
)

# Add main for production/functional binaries
set(ENVY_COMMON_SOURCES ${ENVY_BASE_SOURCES} src/main.cpp)

# Functional test-specific sources
set(ENVY_FUNCTIONAL_TEST_SOURCES
    src/cmds/cmd_cache_functional_test.cpp
    src/cmds/cmd_trace_schema.cpp
)

# Enable ARC for Objective-C++ files
if(APPLE)
    set_source_files_properties(src/ssl_certs_macos.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

# Common compile options
set(ENVY_COMMON_COMPILE_OPTIONS
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-ffunction-sections>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-fdata-sections>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-ffunction-sections>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-fdata-sections>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-format-zero-length>
    $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-format-zero-length>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wno-deprecated-declarations>
    $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-Wno-deprecated-declarations>
)

# Function to configure an envy executable (base configuration, no sanitizers)
function(configure_envy_executable TARGET_NAME)
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    add_dependencies(${TARGET_NAME} envy_embedded_resources envy_embedded_licenses)
    target_compile_options(${TARGET_NAME} PRIVATE ${ENVY_COMMON_COMPILE_OPTIONS})
    target_link_options(${TARGET_NAME} PRIVATE
        $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
        $<$<PLATFORM_ID:Linux>:-Wl,--gc-sections>
    )
    target_link_libraries(${TARGET_NAME} PRIVATE envy::thirdparty)

    # Add noexecstack for all GCC builds (security hardening)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(${TARGET_NAME} PRIVATE
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-Wl,-z,noexecstack>
        )
    endif()
endfunction()

# Function to apply sanitizers based on ENVY_SANITIZER option
function(apply_sanitizers TARGET_NAME)
    if(ENVY_SANITIZER STREQUAL "NONE")
        return()
    endif()

    # MSVC doesn't support sanitizers
    if(MSVC)
        return()
    endif()

    # Map option to compiler flags
    if(ENVY_SANITIZER STREQUAL "ASAN")
        # GCC: Disable ASan to avoid third-party false positives (mbedTLS, AWS SDK)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            message(STATUS "GCC: ASan disabled due to third-party false positives; using UBSAN instead")
            return()
        endif()
        set(_sanitizer_flags "address")
    elseif(ENVY_SANITIZER STREQUAL "UBSAN")
        set(_sanitizer_flags "undefined")
    elseif(ENVY_SANITIZER STREQUAL "TSAN")
        set(_sanitizer_flags "thread")
    else()
        message(FATAL_ERROR "Unknown ENVY_SANITIZER value: ${ENVY_SANITIZER}")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${TARGET_NAME} PRIVATE
            -fsanitize=${_sanitizer_flags} -fno-omit-frame-pointer -g
            $<$<OR:$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:Clang>>:-fsanitize-blacklist=${PROJECT_SOURCE_DIR}/sanitizer_blacklist.txt>
        )
        target_link_options(${TARGET_NAME} PRIVATE -fsanitize=${_sanitizer_flags})

        # Suppress TSAN warnings about atomic_thread_fence in third-party code
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND _sanitizer_flags STREQUAL "thread")
            target_link_options(${TARGET_NAME} PRIVATE -Wno-tsan)
        endif()
    endif()
endfunction()

# Production executable -------------------------------------------------------
if(ENVY_BUILD_ENVY)
add_executable(envy)
target_sources(envy PRIVATE ${ENVY_COMMON_SOURCES})
target_compile_definitions(envy PRIVATE ENVY_VERSION_STR=\"${PROJECT_VERSION}\")
configure_envy_executable(envy)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_C_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE CACHE BOOL "" FORCE)
    set(CMAKE_C_LINK_GROUP_USING_RESCAN
        "LINKER:--start-group"
        "LINKER:--end-group"
        CACHE STRING "" FORCE
    )
    set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE CACHE BOOL "" FORCE)
    set(CMAKE_CXX_LINK_GROUP_USING_RESCAN
        "LINKER:--start-group"
        "LINKER:--end-group"
        CACHE STRING "" FORCE
    )
endif()

target_link_options(envy PRIVATE
    $<$<PLATFORM_ID:Darwin>:-Wl,-map,$<TARGET_FILE_DIR:envy>/envy.map>
    $<$<PLATFORM_ID:Linux>:-Wl,-Map,$<TARGET_FILE_DIR:envy>/envy.map>
    $<$<PLATFORM_ID:Windows>:/MAP:$<TARGET_FILE_DIR:envy>/envy.map>
)

if((CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    if(NOT CMAKE_STRIP)
        find_program(CMAKE_STRIP NAMES strip)
    endif()
    if(CMAKE_STRIP)
        add_custom_command(
            TARGET envy POST_BUILD
            COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:envy>"
            COMMENT "Stripping envy executable"
            VERBATIM
        )
    endif()
endif()
endif() # ENVY_BUILD_ENVY

# Functional tester executable ------------------------------------------------
if(ENVY_BUILD_FUNCTIONAL_TESTER)
add_executable(envy_functional_tester)
target_sources(envy_functional_tester PRIVATE ${ENVY_COMMON_SOURCES} ${ENVY_FUNCTIONAL_TEST_SOURCES})
target_compile_definitions(envy_functional_tester PRIVATE
    ENVY_VERSION_STR=\"${PROJECT_VERSION}\"
    ENVY_FUNCTIONAL_TESTER=1
)
configure_envy_executable(envy_functional_tester)
apply_sanitizers(envy_functional_tester)
endif() # ENVY_BUILD_FUNCTIONAL_TESTER

set(ENVY_UNIT_TEST_SOURCES
    src/unit_test_main.cpp
    src/aws_util_tests.cpp
    src/envy_release_tests.cpp
    src/blake3_util_tests.cpp
    src/cache_tests.cpp
    src/cli_tests.cpp
    src/cmds/cmd_tests.cpp
    src/phases/phase_setup_tests.cpp
    src/phases/phase_spec_fetch_tests.cpp
    src/phases/phase_fetch_tests.cpp
    src/phases/phase_install_tests.cpp
    src/cmds/cmd_extract_tests.cpp
    src/cmds/cmd_merge_depot_tests.cpp
    src/cmds/cmd_mirror_envy_tests.cpp
    src/cmds/cmd_use_tests.cpp
    src/deploy_tests.cpp
    src/luarc_tests.cpp
    src/cmds/cmd_lua_tests.cpp
    src/cmds/cmd_version_tests.cpp
    src/bundle_tests.cpp
    src/spec_util_tests.cpp
    src/extract_tests.cpp
    src/engine_tests.cpp
    src/engine_weak_resolution_tests.cpp
    src/task_engine_tests.cpp
    src/fetch_tests.cpp
    src/fetch_error_tests.cpp
    src/lua_error_formatter_tests.cpp
    src/lua_shell_tests.cpp
    src/manifest_tests.cpp
    src/package_depot_tests.cpp
    src/pkg_cfg_tests.cpp
    src/pkg_cfg_custom_source_tests.cpp
    src/pkg_key_tests.cpp
    src/git_resolve_tests.cpp
    src/lua_ctx/lua_envy_dep_util_tests.cpp
    src/lua_ctx/lua_envy_options_tests.cpp
    src/lua_envy_tests.cpp
    src/platform_tests.cpp
    src/product_util_tests.cpp
    src/reexec_tests.cpp
    src/sha256_tests.cpp
    src/sol_util_tests.cpp
    src/shell_hooks_tests.cpp
    src/shell_tests_common.cpp
    $<$<PLATFORM_ID:Windows>:src/shell_tests_win.cpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/shell_tests_posix.cpp>
    src/trace_tests.cpp
    src/uri_tests.cpp
    src/tui_tests.cpp
    src/util_tests.cpp
    src/version_tests.cpp
)

# The test-only cache drivers build here too: their CLI surface is only registered
# under ENVY_FUNCTIONAL_TESTER, so cli_tests.cpp cannot reach it otherwise.
add_executable(envy_unit_tests)
target_sources(envy_unit_tests PRIVATE ${ENVY_BASE_SOURCES} ${ENVY_FUNCTIONAL_TEST_SOURCES} ${ENVY_UNIT_TEST_SOURCES})
target_compile_definitions(envy_unit_tests PRIVATE ENVY_VERSION_STR=\"${PROJECT_VERSION}\" ENVY_UNIT_TEST ENVY_FUNCTIONAL_TESTER=1)
target_link_libraries(envy_unit_tests PRIVATE doctest::doctest)
configure_envy_executable(envy_unit_tests)
apply_sanitizers(envy_unit_tests)


# Run tests and create timestamp on success
# MSVC ASan doesn't use the same suppression file format, so only pass ASAN_OPTIONS on Clang/GCC
if(MSVC)
    set(_envy_test_env_prefix)
else()
    set(_envy_test_env_prefix ${CMAKE_COMMAND} -E env "ASAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/asan.supp")
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/envy_unit_tests.timestamp"
    COMMAND ${_envy_test_env_prefix} $<TARGET_FILE:envy_unit_tests>
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/envy_unit_tests.timestamp"
    DEPENDS envy_unit_tests ${TEST_DATA_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Running unit tests"
    VERBATIM
)
unset(_envy_test_env_prefix)

add_custom_target(run_unit_tests ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/envy_unit_tests.timestamp"
)

# Build-script unit tests (Python)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# CONFIGURE_DEPENDS so a newly added workflow or script re-runs the lint on the next build
# rather than whenever something else happens to force a reconfigure.
file(GLOB _envy_shell_lint_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/.github/workflows/*.yml"
    "${PROJECT_SOURCE_DIR}/*.sh"
    "${PROJECT_SOURCE_DIR}/test_data/archives/*.sh"
)
list(APPEND _envy_shell_lint_inputs
    "${PROJECT_SOURCE_DIR}/src/resources/envy"
    "${PROJECT_SOURCE_DIR}/src/resources/product_script"
    "${PROJECT_SOURCE_DIR}/examples/bin/envy"
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/script_tests.timestamp"
    COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/cmake/scripts/test_embed_resource.py"
    COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/cmake/scripts/test_shell_strictness.py"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/script_tests.timestamp"
    DEPENDS "${PROJECT_SOURCE_DIR}/cmake/scripts/embed_resource.py"
            "${PROJECT_SOURCE_DIR}/cmake/scripts/test_embed_resource.py"
            "${PROJECT_SOURCE_DIR}/cmake/scripts/test_shell_strictness.py"
            ${_envy_shell_lint_inputs}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Running build-script tests"
    VERBATIM
)
add_custom_target(run_script_tests ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/script_tests.timestamp"
)

# Functional test executables are built but not run automatically
# Run manually with: python -m functional_tests
