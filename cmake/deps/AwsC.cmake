# aws-c-s3 and the AWS C libraries beneath it, built in-tree from the release archives
# pinned in ENVY_AWS_C_DEPS. Each lands in out/cache/aws-c/<repo>-<version>.
set(USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(AWS_USE_LIBCRYPTO_TO_SUPPORT_ED25519_EVERYWHERE OFF CACHE BOOL "" FORCE)
if(APPLE)
    set(USE_S2N OFF CACHE BOOL "" FORCE)  # Secure Transport; s2n and aws-lc never build
endif()
if(MSVC)
    set(AWS_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
    set(STATIC_CRT ON CACHE BOOL "" FORCE)
endif()

foreach(_spec IN LISTS ENVY_AWS_C_DEPS)
    string(REPLACE " " ";" _spec "${_spec}")
    list(GET _spec 0 _org_repo)
    list(GET _spec 1 _version)
    list(GET _spec 2 _sha256)
    cmake_path(GET _org_repo FILENAME _repo)
    if(APPLE AND _repo MATCHES "^(aws-lc|s2n-tls)$")
        continue()
    endif()
    cmake_path(APPEND ENVY_CACHE_DIR "aws-c" "${_repo}-${_version}" OUTPUT_VARIABLE _src)
    if(NOT EXISTS "${_src}/CMakeLists.txt")
        FetchContent_Populate(envy_${_repo}
            SOURCE_DIR "${_src}"
            BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/${_repo}-fetch"
            URL "https://github.com/${_org_repo}/archive/refs/tags/${_version}.tar.gz"
            URL_HASH SHA256=${_sha256}
        )
    endif()
    string(MAKE_C_IDENTIFIER "${_repo}" _id)
    set(envy_${_id}_SOURCE_DIR "${_src}")
    if(_repo STREQUAL "aws-c-s3")
        string(REGEX REPLACE "^v" "" ENVY_AWS_C_S3_VERSION "${_version}")  # envy version
    endif()
    list(APPEND ENVY_AWS_C_SOURCE_DIRS "${_repo}=${_src}")
endforeach()

# Patches stamp beside the sources they edit, so a version bump re-patches.
envy_patch_aws_c_static_runtime("${envy_aws_c_common_SOURCE_DIR}")
envy_patch_aws_c_base64_sig("${envy_aws_c_common_SOURCE_DIR}")
if(UNIX AND NOT APPLE)
    envy_patch_s2n_feature_probes("${envy_s2n_tls_SOURCE_DIR}")
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

envy_add_aws_c(aws-c-common)
if(UNIX AND NOT APPLE)  # Linux TLS: s2n-tls over aws-lc's libcrypto
    include(CheckCCompilerFlag)
    set(DISABLE_GO ON CACHE BOOL "" FORCE)
    set(DISABLE_PERL ON CACHE BOOL "" FORCE)
    set(BUILD_LIBSSL OFF CACHE BOOL "" FORCE)
    set(MSAN 0 CACHE BOOL "" FORCE)
    set(OPENSSL_NO_ASM 0 CACHE BOOL "" FORCE)
    envy_add_aws_c(aws-lc)
    foreach(_flag -Wno-stringop-overflow -Wno-array-parameter)
        string(MAKE_C_IDENTIFIER "HAS${_flag}" _has)
        check_c_compiler_flag(${_flag} ${_has})
        if(${_has})
            target_compile_options(crypto PRIVATE ${_flag})
        endif()
    endforeach()
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
