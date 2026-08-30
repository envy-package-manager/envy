set(AWS_SDK_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AWS_SDK_CPP_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(AWS_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(AWS_SDK_CPP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AWS_SDK_CPP_CUSTOM_MEMORY_MANAGEMENT OFF CACHE BOOL "" FORCE)
set(BUILD_ONLY "s3;transfer;sso;sso-oidc" CACHE STRING "" FORCE)
set(AWS_BUILD_ONLY "s3;transfer;sso;sso-oidc" CACHE STRING "" FORCE)
set(AWS_SDK_CPP_BUILD_ONLY "s3;transfer;sso;sso-oidc" CACHE STRING "" FORCE)
set(ENFORCE_SUBMODULE_VERSIONS OFF CACHE BOOL "" FORCE)
set(USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(USE_OPENSSL_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(NO_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(AWS_USE_LIBCRYPTO_TO_SUPPORT_ED25519_EVERYWHERE OFF CACHE BOOL "" FORCE)
set(BUILD_AWS_CRT_MQTT OFF CACHE BOOL "" FORCE)
set(AWS_MQTT_WITH_WEBSOCKETS OFF CACHE BOOL "" FORCE)
# On Apple, aws-c-io defaults USE_S2N=ON, which drags in s2n-tls plus its
# aws-lc/libcrypto dependency (~1.7 MB) even though the Security framework
# already provides TLS. Force the native Secure Transport backend instead.
if(APPLE)
    set(USE_S2N OFF CACHE BOOL "" FORCE)
endif()
if(MSVC)
    set(AWS_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
    set(STATIC_CRT ON CACHE BOOL "" FORCE)
endif()
cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_AWS_SDK_ARCHIVE}" OUTPUT_VARIABLE _aws_sdk_archive)
set(_aws_sdk_url "${ENVY_AWS_SDK_URL}")
if(EXISTS "${_aws_sdk_archive}")
    file(TO_CMAKE_PATH "${_aws_sdk_archive}" _aws_sdk_archive_norm)
    set(_aws_sdk_url "file://${_aws_sdk_archive_norm}")
endif()

cmake_path(APPEND ENVY_CACHE_DIR "aws_sdk-src" OUTPUT_VARIABLE aws_sdk_SOURCE_DIR)
cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "aws_sdk-build" OUTPUT_VARIABLE aws_sdk_BINARY_DIR)

if(NOT DEFINED ENVY_PYTHON_LAUNCHER)
    if(WIN32)
        find_program(ENVY_PYTHON_LAUNCHER py REQUIRED)
        set(ENVY_PYTHON_ARGS "-3" CACHE INTERNAL "Arguments passed to the Python launcher" FORCE)
    else()
        find_program(ENVY_PYTHON_LAUNCHER python3 REQUIRED)
        set(ENVY_PYTHON_ARGS "" CACHE INTERNAL "Arguments passed to the Python interpreter" FORCE)
    endif()
endif()

if(NOT EXISTS "${aws_sdk_SOURCE_DIR}/CMakeLists.txt")
    # Download archive into cache if missing
    if(NOT EXISTS "${_aws_sdk_archive}")
        file(DOWNLOAD "${_aws_sdk_url}" "${_aws_sdk_archive}"
            SHOW_PROGRESS
            EXPECTED_HASH SHA256=${ENVY_AWS_SDK_SHA256}
            TLS_VERIFY ON)
    endif()

    # Extract using our Python unzip shim (handles UTF-8 paths reliably)
    get_filename_component(_envy_root "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
    set(_envy_unzip "${_envy_root}/tools/unzip")
    if(NOT EXISTS "${_envy_unzip}")
        message(FATAL_ERROR "Missing unzip helper: ${_envy_unzip}")
    endif()

    set(_unzip_cmd "${ENVY_PYTHON_LAUNCHER}")
    if(ENVY_PYTHON_ARGS)
        list(APPEND _unzip_cmd ${ENVY_PYTHON_ARGS})
    endif()
    list(APPEND _unzip_cmd "${_envy_unzip}")

    set(_stage_dir "${aws_sdk_SOURCE_DIR}.stage")
    file(REMOVE_RECURSE "${_stage_dir}" "${aws_sdk_SOURCE_DIR}")
    file(MAKE_DIRECTORY "${_stage_dir}")

    execute_process(
        COMMAND ${_unzip_cmd} -q "${_aws_sdk_archive}" -d "${_stage_dir}"
        RESULT_VARIABLE _unzip_rv
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT _unzip_rv EQUAL 0)
        message(FATAL_ERROR "Failed to unzip AWS SDK archive: ${_aws_sdk_archive}")
    endif()

    file(GLOB _aws_dirs LIST_DIRECTORIES TRUE "${_stage_dir}/aws-sdk-cpp-*")
    if(_aws_dirs STREQUAL "")
        message(FATAL_ERROR "AWS SDK archive did not contain an aws-sdk-cpp-* directory")
    endif()
    list(SORT _aws_dirs)
    list(GET _aws_dirs 0 _extracted_dir)

    file(RENAME "${_extracted_dir}" "${aws_sdk_SOURCE_DIR}")
    file(REMOVE_RECURSE "${_stage_dir}")

    unset(_aws_dirs)
    unset(_extracted_dir)
    unset(_stage_dir)
    unset(_unzip_cmd)
    unset(_envy_unzip)
    unset(_envy_root)
endif()

unset(_aws_sdk_archive)
unset(_aws_sdk_archive_norm)
unset(_aws_sdk_url)

set(ENVY_AWSCRT_ROOT "${aws_sdk_SOURCE_DIR}/crt/aws-crt-cpp")
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -Daws_sdk_SOURCE_DIR=${aws_sdk_SOURCE_DIR}
        -Denvy_cache_dir=${ENVY_CACHE_DIR}
        -P "${PROJECT_SOURCE_DIR}/cmake/scripts/prefetch_aws_crt.cmake"
    WORKING_DIRECTORY "${aws_sdk_SOURCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${ENVY_AWSCRT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "AWS CRT prefetch failed: missing ${ENVY_AWSCRT_ROOT}/CMakeLists.txt")
endif()

set(_envy_prev_build_testing ${BUILD_TESTING})
if(DEFINED aws_sdk_SOURCE_DIR AND DEFINED aws_sdk_BINARY_DIR)
    envy_patch_aws_sdk_curl("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
    envy_patch_aws_sdk_runtime("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
    envy_patch_aws_crt_disable_prebuild("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
    if(APPLE)
        envy_patch_aws_crt_no_apple_s2n("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
    endif()
    envy_patch_aws_crt_base64_sig("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
    envy_patch_s2n_feature_probes("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
    envy_patch_aws_noexecstack("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
    envy_patch_aws_s3_rules_gzip("${aws_sdk_SOURCE_DIR}" "${aws_sdk_BINARY_DIR}")
endif()
set(BUILD_TESTING OFF CACHE BOOL "Disable tests inside AWS SDK build" FORCE)
add_subdirectory(${aws_sdk_SOURCE_DIR} ${aws_sdk_BINARY_DIR})
if(TARGET s2n)
    if(NOT ENVY_SANITIZER STREQUAL "NONE")
        set_property(TARGET s2n PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
    endif()
    if(NOT MSVC)
        target_compile_options(s2n PRIVATE -Wno-builtin-macro-redefined)
    endif()
endif()
if(TARGET aws-c-common)
    if(NOT MSVC)
        target_compile_options(aws-c-common PRIVATE -Wno-builtin-macro-redefined)
    endif()
endif()
if(TARGET aws-c-io)
    if(NOT MSVC)
        target_compile_options(aws-c-io PRIVATE -Wno-builtin-macro-redefined)
        if(APPLE)
            target_compile_options(aws-c-io PRIVATE $<$<C_COMPILER_ID:AppleClang,Clang>:-Wno-gnu-folding-constant -Wno-deprecated-declarations>)
        endif()
    endif()
endif()
set(BUILD_TESTING ${_envy_prev_build_testing} CACHE BOOL "Restart project testing flag" FORCE)
unset(_envy_prev_build_testing)

# Disable sanitizers for AWS CRT libraries due to false positive in hashlittle2
# lookup3.inl hash intentionally overreads strings; gets inlined via headers into many compilation units
set(_envy_aws_crt_targets
    aws-c-common aws-c-io aws-c-http aws-c-auth aws-c-cal aws-c-compression
    aws-c-event-stream aws-c-s3 aws-c-sdkutils aws-checksums aws-crt-cpp
)
foreach(_envy_target IN LISTS _envy_aws_crt_targets)
    if(TARGET ${_envy_target})
        # LTO is only disabled under sanitizers (the inlined lookup3 hash trips
        # ASAN/TSAN); Release keeps global LTO so cross-module DCE can shrink the
        # CRT — the largest non-LTO component on Linux.
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
unset(_envy_aws_crt_targets)
unset(_envy_target)
