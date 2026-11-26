# aws-c-s3 and the AWS C libraries beneath it, built in-tree from the release archives
# pinned in ENVY_AWS_C_DEPS. Each extracts to out/cache/aws-c/<repo>-<version>.
set(USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(AWS_USE_LIBCRYPTO_TO_SUPPORT_ED25519_EVERYWHERE OFF CACHE BOOL "" FORCE)
if(APPLE)
    set(USE_S2N OFF CACHE BOOL "" FORCE)  # Secure Transport; s2n and aws-lc never build
endif()
if(MSVC)
    set(AWS_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
    set(STATIC_CRT ON CACHE BOOL "" FORCE)
endif()

# Linux's TLS; macOS and Windows use the OS's, so they never fetch aws-lc or s2n-tls.
set(_linux_tls "aws-lc|s2n-tls")
cmake_path(APPEND ENVY_CACHE_DIR "aws-c" OUTPUT_VARIABLE _aws_c_root)
set(_aws_c_trees "")
foreach(_spec IN LISTS ENVY_AWS_C_DEPS)
    string(REPLACE " " ";" _spec "${_spec}")
    list(GET _spec 0 _org_repo)
    list(GET _spec 1 _version)
    list(GET _spec 2 _sha256)
    cmake_path(GET _org_repo FILENAME _repo)
    if(_repo MATCHES "^(${_linux_tls})$" AND NOT (UNIX AND NOT APPLE))
        continue()
    endif()
    set(_tree "${_repo}-${_version}")
    set(_src "${_aws_c_root}/${_tree}")
    list(APPEND _aws_c_trees "${_tree}")
    if(NOT EXISTS "${_src}/CMakeLists.txt")
        if(FETCHCONTENT_FULLY_DISCONNECTED)
            message(FATAL_ERROR "[envy] ${_tree} is missing from ${_aws_c_root} and fetching is disconnected")
        endif()
        # The archive is transient: out/cache keeps only the extracted tree.
        set(_archive "${CMAKE_BINARY_DIR}/_deps/aws-c/${_tree}.tar.gz")
        envy_fetch_verified_file(
            PATH "${_archive}"
            URL "https://github.com/${_org_repo}/archive/refs/tags/${_version}.tar.gz"
            SHA256 ${_sha256}
            HUMAN_NAME "${_tree}")
        set(_stage "${_src}.stage")
        file(REMOVE_RECURSE "${_stage}" "${_src}")
        file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_stage}")
        file(GLOB _top LIST_DIRECTORIES TRUE "${_stage}/*")  # GitHub's <repo>-<tag>/
        list(LENGTH _top _count)
        if(NOT _count EQUAL 1)
            message(FATAL_ERROR "[envy] ${_archive} should hold one top-level directory")
        endif()
        file(RENAME "${_top}" "${_src}")
        file(REMOVE_RECURSE "${_stage}")
        file(REMOVE "${_archive}")
    endif()
    string(MAKE_C_IDENTIFIER "${_repo}" _id)
    set(envy_${_id}_SOURCE_DIR "${_src}")
    list(APPEND ENVY_AWS_C_SOURCE_DIRS "${_repo}=${_src}")
    if(_repo STREQUAL "aws-c-s3")
        string(REGEX REPLACE "^v" "" ENVY_AWS_C_S3_VERSION "${_version}")  # envy version
    elseif(_repo MATCHES "^(${_linux_tls})$")
        string(REGEX REPLACE "^v" "" _tls_version "${_version}")
        list(APPEND ENVY_AWS_C_TLS "${_repo} ${_tls_version}")
    endif()
endforeach()
string(JOIN ", " ENVY_AWS_C_TLS ${ENVY_AWS_C_TLS})
# cJSON and libcbor, compiled into aws-c-common, carry their own licenses.
list(APPEND ENVY_AWS_C_SOURCE_DIRS
    "aws-c-common third-party=${envy_aws_c_common_SOURCE_DIR}/THIRD-PARTY-LICENSES.txt")

# A version no longer pinned would otherwise ride along in out/cache forever.
file(GLOB _cached LIST_DIRECTORIES TRUE RELATIVE "${_aws_c_root}" "${_aws_c_root}/*")
foreach(_entry IN LISTS _cached)
    if(NOT _entry IN_LIST _aws_c_trees)
        file(REMOVE_RECURSE "${_aws_c_root}/${_entry}")
    endif()
endforeach()

# Patches stamp beside the sources they edit, so a version bump re-patches.
envy_patch_aws_c_static_runtime("${envy_aws_c_common_SOURCE_DIR}")
envy_patch_aws_c_base64_sig("${envy_aws_c_common_SOURCE_DIR}")
if(UNIX AND NOT APPLE)
    envy_patch_s2n_feature_probes("${envy_s2n_tls_SOURCE_DIR}" "${envy_aws_lc_SOURCE_DIR}")
    envy_patch_aws_noexecstack("${envy_aws_lc_SOURCE_DIR}" "${envy_s2n_tls_SOURCE_DIR}")
endif()

# What aws-crt-cpp's BUILD_DEPS did: aws_use_package() links in-tree targets by name.
list(APPEND CMAKE_MODULE_PATH "${envy_aws_c_common_SOURCE_DIR}/cmake")
include(AwsFindPackage)
set(IN_SOURCE_BUILD ON)
set(_envy_prev_build_testing ${BUILD_TESTING})
set(BUILD_TESTING OFF)

function(envy_add_aws_c repo)
    string(MAKE_C_IDENTIFIER "${repo}" _id)
    add_subdirectory("${envy_${_id}_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/${repo}-build"
        EXCLUDE_FROM_ALL)
endfunction()

# aws-lc compiles into object libraries that see only directory flags, and adds -Werror.
function(envy_add_aws_lc)
    foreach(_flag -Wno-stringop-overflow -Wno-array-parameter)
        string(MAKE_C_IDENTIFIER "HAS${_flag}" _has)
        check_c_compiler_flag(${_flag} ${_has})
        if(${_has})
            string(APPEND CMAKE_C_FLAGS " ${_flag}")
        endif()
    endforeach()
    envy_add_aws_c(aws-lc)
endfunction()

envy_add_aws_c(aws-c-common)
# lookup3's hashlittle2 otherwise reads a string's tail as whole words, past its end (ASan:
# global-buffer-overflow); VALGRIND is its own switch to the byte-wise tail, same hashes.
target_compile_definitions(aws-c-common PRIVATE VALGRIND)
if(UNIX AND NOT APPLE)  # Linux TLS: s2n-tls over aws-lc's libcrypto
    include(CheckCCompilerFlag)
    set(DISABLE_GO ON CACHE BOOL "" FORCE)
    set(DISABLE_PERL ON CACHE BOOL "" FORCE)
    set(BUILD_LIBSSL OFF CACHE BOOL "" FORCE)
    set(MSAN 0 CACHE BOOL "" FORCE)
    set(OPENSSL_NO_ASM 0 CACHE BOOL "" FORCE)
    envy_add_aws_lc()
    set(UNSAFE_TREAT_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    envy_add_aws_c(s2n-tls)
    if(NOT ENVY_SANITIZER STREQUAL "NONE")
        set_property(TARGET s2n PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
    endif()
    if(NOT MSVC)
        target_compile_options(s2n PRIVATE -Wno-builtin-macro-redefined)
    endif()
endif()
foreach(_repo aws-c-sdkutils aws-c-cal aws-c-io aws-c-compression aws-c-http aws-c-auth
              aws-checksums aws-c-s3)
    envy_add_aws_c(${_repo})
endforeach()

set(BUILD_TESTING ${_envy_prev_build_testing})
unset(_envy_prev_build_testing)

if(NOT MSVC)
    target_compile_options(aws-c-common PRIVATE -Wno-builtin-macro-redefined)
    target_compile_options(aws-c-io PRIVATE -Wno-builtin-macro-redefined)
    if(APPLE)
        target_compile_options(aws-c-io PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang>:-Wno-gnu-folding-constant -Wno-deprecated-declarations>)
    endif()
endif()

foreach(_target aws-c-common aws-c-io aws-c-http aws-c-auth aws-c-cal aws-c-compression
                aws-c-s3 aws-c-sdkutils aws-checksums)
    # envy reads errors, not logs: every AWS_LOGF_* compiles out, leaving locals and
    # helpers that only fed a log line unused.
    target_compile_definitions(${_target} PRIVATE AWS_STATIC_LOG_LEVEL=0)
    target_compile_options(${_target} PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-function>
        $<$<C_COMPILER_ID:MSVC>:/wd4100 /wd4189 /wd4505>
    )
    # lookup3.inl's hash overreads strings by design and inlines everywhere; sanitizer
    # builds keep it out of LTO. Release keeps LTO for cross-module dead stripping.
    if(NOT ENVY_SANITIZER STREQUAL "NONE")
        set_property(TARGET ${_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${_target} PRIVATE -fno-sanitize=all)
    endif()
endforeach()
unset(_target)
unset(_spec)
unset(_org_repo)
unset(_version)
unset(_sha256)
unset(_repo)
unset(_src)
unset(_id)
unset(_tree)
unset(_stage)
unset(_top)
unset(_count)
unset(_archive)
unset(_cached)
unset(_entry)
unset(_tls_version)
unset(_linux_tls)
unset(_aws_c_root)
unset(_aws_c_trees)
