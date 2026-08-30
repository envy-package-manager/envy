# Helper utilities for patching third-party projects in-flight.  Each helper
# script rewrites fragile upstream build logic just enough to fit Envy’s
# monorepo layout without forking entire trees.  Keep the interventions
# narrowly scoped and idempotent so reconfiguration remains safe.

if(NOT DEFINED ENVY_PYTHON_LAUNCHER)
    if(WIN32)
        find_program(_envy_python_launcher py REQUIRED)
        set(ENVY_PYTHON_ARGS "-3" CACHE INTERNAL "Arguments passed to the Python launcher" FORCE)
    else()
        find_program(_envy_python_launcher python3 REQUIRED)
        set(ENVY_PYTHON_ARGS "" CACHE INTERNAL "Arguments passed to the Python interpreter" FORCE)
    endif()
    set(ENVY_PYTHON_LAUNCHER "${_envy_python_launcher}" CACHE INTERNAL "Python entrypoint for Envy CMake helpers" FORCE)
    unset(_envy_python_launcher)
endif()

function(envy_run_python script)
    if(NOT EXISTS "${script}")
        message(FATAL_ERROR "Python script '${script}' not found")
    endif()

    set(_envy_python_command "${ENVY_PYTHON_LAUNCHER}")
    if(ENVY_PYTHON_ARGS)
        list(APPEND _envy_python_command ${ENVY_PYTHON_ARGS})
    endif()
    list(APPEND _envy_python_command "${script}")
    if(ARGN)
        list(APPEND _envy_python_command ${ARGN})
    endif()

    execute_process(COMMAND ${_envy_python_command}
        COMMAND_ERROR_IS_FATAL ANY)

    unset(_envy_python_command)
endfunction()

# Patch libssh2’s top-level CMakeLists so it stops assuming it owns the
# superproject: bump the policy floor and pin CMAKE_SOURCE_DIR back to Envy.
function(envy_patch_libssh2 source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libssh2_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libssh2.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libssh2_patch.py.in")

    set(LIBSSH2_CMAKELISTS "${_source_dir_norm}/CMakeLists.txt")
    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
endfunction()

# Rewrite libgit2’s SelectSSH helper to prefer our in-tree libssh2 target over
# pkg-config, keeping headers/libpaths consistent with the fetched dependency.
function(envy_patch_libgit2_select libgit2_source_dir libgit2_binary_dir libssh2_source_dir libssh2_binary_dir)
    set(_source_dir_norm "${libgit2_source_dir}")
    set(_binary_dir_norm "${libgit2_binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libgit2_select_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libgit2_select.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libgit2_select_patch.py.in")

    set(SELECT_PATH "${_source_dir_norm}/cmake/SelectSSH.cmake")
    if(NOT EXISTS "${SELECT_PATH}")
        return()
    endif()

    set(LIBSSH2_SOURCE "${libssh2_source_dir}")
    set(LIBSSH2_BINARY "${libssh2_binary_dir}")
    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(SELECT_PATH)
    unset(LIBSSH2_SOURCE)
    unset(LIBSSH2_BINARY)
endfunction()

# Fix libarchive feature probes that clobber POSIX types by injecting the
# right include context and trimming unsafe fallbacks.
function(envy_patch_libarchive_cmakelists source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libarchive_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libarchive.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libarchive_patch.py.in")

    set(LIBARCHIVE_CMAKELISTS "${_source_dir_norm}/CMakeLists.txt")
    set(LIBARCHIVE_SOURCE "${_source_dir_norm}/libarchive")
    if(NOT EXISTS "${LIBARCHIVE_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(LIBARCHIVE_CMAKELISTS)
    unset(LIBARCHIVE_SOURCE)
endfunction()

# Guard AWS SDK’s curl capability probe against generator expressions so it
# stays compatible with the libcurl target we fetch locally.
function(envy_patch_aws_sdk_curl source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_awssdk_curl_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_aws_sdk_curl.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/aws_sdk_curl_patch.py.in")

    set(AWS_CORE_CMAKELISTS "${_source_dir_norm}/src/aws-cpp-sdk-core/CMakeLists.txt")
    if(NOT EXISTS "${AWS_CORE_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(AWS_CORE_CMAKELISTS)
endfunction()

# Force AWS CRT to stick with the static MSVC runtime regardless of build
# type to match Envy’s global toolchain expectations.
function(envy_patch_aws_sdk_runtime source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_awssdk_runtime_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_aws_sdk_runtime.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/aws_sdk_runtime_patch.py.in")

    set(AWS_CFLAGS_PATH "${_source_dir_norm}/crt/aws-crt-cpp/crt/aws-c-common/cmake/AwsCFlags.cmake")
    if(NOT EXISTS "${AWS_CFLAGS_PATH}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(AWS_CFLAGS_PATH)
endfunction()

# Replace aws_prebuild_dependency() in aws-crt-cpp so aws-lc builds during
# the normal compile phase instead of at configure-time.
function(envy_patch_aws_crt_disable_prebuild source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_awssdk_crt_prebuild_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_aws_crt_disable_prebuild.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/aws_crt_disable_prebuild_patch.py.in")

    set(AWSCRT_CMAKELISTS "${_source_dir_norm}/crt/aws-crt-cpp/CMakeLists.txt")
    if(NOT EXISTS "${AWSCRT_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(AWSCRT_CMAKELISTS)
endfunction()

# On Apple, envy uses Secure Transport (USE_S2N=OFF), so skip compiling s2n and
# its aws-lc dependency entirely instead of building ~1.7 MB that never links.
function(envy_patch_aws_crt_no_apple_s2n source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_awssdk_crt_no_apple_s2n_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_aws_crt_no_apple_s2n.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/aws_crt_no_apple_s2n_patch.py.in")

    set(AWSCRT_CMAKELISTS "${_source_dir_norm}/crt/aws-crt-cpp/CMakeLists.txt")
    if(NOT EXISTS "${AWSCRT_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(AWSCRT_CMAKELISTS)
endfunction()

# Ensure s2n's feature probes include aws-lc headers instead of falling back
# to system OpenSSL when evaluating libcrypto capabilities.
function(envy_patch_s2n_feature_probes source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_s2n_feature_probe_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_s2n_feature_probes.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/s2n_feature_probe_patch.py.in")

    set(S2N_CMAKELISTS "${_source_dir_norm}/crt/aws-crt-cpp/crt/s2n/CMakeLists.txt")
    if(NOT EXISTS "${S2N_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(S2N_CMAKELISTS)
endfunction()

# Remove -Wa,--noexecstack from aws-lc and s2n assembly builds to silence
# lto-wrapper warnings during TSAN+LTO linking.
function(envy_patch_aws_noexecstack source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_aws_noexecstack_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_aws_noexecstack.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/aws_noexecstack_patch.py.in")

    set(AWS_LC_CRYPTO_CMAKELISTS "${_source_dir_norm}/crt/aws-crt-cpp/crt/aws-lc/crypto/CMakeLists.txt")
    set(S2N_CMAKELISTS "${_source_dir_norm}/crt/aws-crt-cpp/crt/s2n/CMakeLists.txt")

    if(NOT EXISTS "${AWS_LC_CRYPTO_CMAKELISTS}" OR NOT EXISTS "${S2N_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(AWS_LC_CRYPTO_CMAKELISTS)
    unset(S2N_CMAKELISTS)
endfunction()

# Align aws-c-common's AVX2 base64 encoder definition with its declaration;
# upstream's Base64url change left the definition one parameter short and GCC
# LTO warns (-Wlto-type-mismatch) at final link.
function(envy_patch_aws_crt_base64_sig source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_aws_crt_base64_sig_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_aws_crt_base64_sig.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/aws_crt_base64_sig_patch.py.in")

    set(AWS_C_COMMON_ENCODING_AVX2 "${_source_dir_norm}/crt/aws-crt-cpp/crt/aws-c-common/source/arch/intel/encoding_avx2.c")
    if(NOT EXISTS "${AWS_C_COMMON_ENCODING_AVX2}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(AWS_C_COMMON_ENCODING_AVX2)
endfunction()

# Make libcurl’s pkg-config metadata reference the actual zlib target we
# build instead of the abstract ZLIB::ZLIB alias.
# The generated S3 endpoint ruleset is a 119 KB JSON blob emitted one char
# literal per byte, read once when an S3 client is built. Deflate it to ~4.5 KB
# and inflate on first use.
function(envy_patch_aws_s3_rules_gzip source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_aws_s3_rules_gzip_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_aws_s3_rules_gzip.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/aws_s3_rules_gzip_patch.py.in")

    set(AWS_S3_ENDPOINT_RULES_CPP
        "${_source_dir_norm}/generated/src/aws-cpp-sdk-s3/source/S3EndpointRules.cpp")
    if(NOT EXISTS "${AWS_S3_ENDPOINT_RULES_CPP}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(AWS_S3_ENDPOINT_RULES_CPP)
endfunction()

function(envy_patch_libcurl_cmakelists source_dir binary_dir zlib_target)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libcurl_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libcurl.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libcurl_patch.py.in")

    set(LIBCURL_CMAKELISTS "${_source_dir_norm}/CMakeLists.txt")
    set(ENVY_ZLIB_TARGET "${zlib_target}")
    if(NOT EXISTS "${LIBCURL_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(LIBCURL_CMAKELISTS)
    unset(ENVY_ZLIB_TARGET)
endfunction()

# Ensure libgit2’s FindStatNsec handles glibc’s macro requirements without
# disturbing macOS headers by toggling the defines around the probe.
function(envy_patch_libgit2_nsec source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libgit2_nsec_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libgit2_nsec.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libgit2_nsec_patch.py.in")

    set(FIND_STAT_NSEC "${_source_dir_norm}/cmake/FindStatNsec.cmake")
    if(NOT EXISTS "${FIND_STAT_NSEC}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(FIND_STAT_NSEC)
endfunction()

# Strip libssh2 install()/export() commands so FetchContent doesn’t try to
# publish its private artifacts into the parent install set.
function(envy_patch_libssh2_install source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libssh2_install_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libssh2_install.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libssh2_install_patch.py.in")

    set(LIBSSH2_SRC_CMAKELISTS "${_source_dir_norm}/src/CMakeLists.txt")
    if(NOT EXISTS "${LIBSSH2_SRC_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(LIBSSH2_SRC_CMAKELISTS)
endfunction()

# Patch libgit2's MSVC Release flags from /O2 (maximize speed) to /O1
# (minimize size).  libgit2's DefaultCFlags.cmake unconditionally replaces
# CMAKE_C_FLAGS_RELEASE, discarding our global size-optimization setting.
function(envy_patch_libgit2_cflags source_dir binary_dir)
    if(NOT MSVC)
        return()
    endif()

    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libgit2_cflags_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libgit2_cflags.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libgit2_cflags_patch.py.in")

    set(LIBGIT2_DEFAULT_CFLAGS "${source_dir}/cmake/DefaultCFlags.cmake")
    if(NOT EXISTS "${LIBGIT2_DEFAULT_CFLAGS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_binary_dir_norm)
    unset(LIBGIT2_DEFAULT_CFLAGS)
endfunction()

# Strip libgit2 install()/export() calls for the same reason—keep it scoped
# to Envy's build tree with no accidental installs.
function(envy_patch_libgit2_install source_dir binary_dir)
    set(_source_dir_norm "${source_dir}")
    set(_binary_dir_norm "${binary_dir}")

    set(_stamp "${_binary_dir_norm}/envy_libgit2_install_patch.stamp")
    if(EXISTS "${_stamp}")
        return()
    endif()

    set(_script "${_binary_dir_norm}/envy_patch_libgit2_install.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/libgit2_install_patch.py.in")

    set(LIBGIT2_LIBGIT2_CMAKELISTS "${_source_dir_norm}/src/libgit2/CMakeLists.txt")
    if(NOT EXISTS "${LIBGIT2_LIBGIT2_CMAKELISTS}")
        return()
    endif()

    configure_file("${_template}" "${_script}" @ONLY)

    envy_run_python("${_script}")

    file(REMOVE "${_script}")
    file(WRITE "${_stamp}" "patched\n")

    unset(_stamp)
    unset(_script)
    unset(_template)
    unset(_source_dir_norm)
    unset(_binary_dir_norm)
    unset(LIBGIT2_LIBGIT2_CMAKELISTS)
endfunction()
