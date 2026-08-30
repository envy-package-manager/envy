# FindLicenses.cmake - Discover and concatenate third-party licenses
#
# find_and_collect_licenses(
#     OUTPUT <output.txt>
#     SOURCES <name1>=<path1> [<name2>=<path2> ...]
# )
#
# Discovers license files at configure time, concatenates at build time. Creates custom
# target 'envy_licenses_collected'; embed_resources() COMPRESS does the gzipping.

function(find_and_collect_licenses)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "OUTPUT" "SOURCES")

    if(NOT ARG_OUTPUT)
        message(FATAL_ERROR "find_and_collect_licenses: OUTPUT is required")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "find_and_collect_licenses: SOURCES is required")
    endif()

    set(FIND_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/find_licenses.py")
    set(COLLECT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/collect_licenses.py")
    set(MANIFEST_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/licenses.manifest")

    # Run find_licenses.py at configure time to discover license paths
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${FIND_SCRIPT}" "${MANIFEST_FILE}" ${ARG_SOURCES}
        RESULT_VARIABLE FIND_RESULT
        OUTPUT_VARIABLE FIND_OUTPUT
        ERROR_VARIABLE FIND_ERROR
    )
    if(NOT FIND_RESULT EQUAL 0)
        message(FATAL_ERROR "License discovery failed:\n${FIND_ERROR}")
    endif()

    # Parse manifest to get list of license file paths for DEPENDS
    file(STRINGS "${MANIFEST_FILE}" MANIFEST_LINES)
    set(LICENSE_FILES "")
    foreach(LINE IN LISTS MANIFEST_LINES)
        if(LINE)
            string(REGEX REPLACE "^[^|]+\\|" "" LICENSE_PATH "${LINE}")
            list(APPEND LICENSE_FILES "${LICENSE_PATH}")
        endif()
    endforeach()

    # Create output directory
    get_filename_component(OUTPUT_DIR "${ARG_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${OUTPUT_DIR}")

    # Build-time concatenation command
    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND "${Python3_EXECUTABLE}" "${COLLECT_SCRIPT}" "${ARG_OUTPUT}" "${MANIFEST_FILE}"
        DEPENDS ${LICENSE_FILES} "${MANIFEST_FILE}" "${COLLECT_SCRIPT}"
        COMMENT "Collecting third-party licenses"
        VERBATIM
    )

    # Custom target for dependency tracking
    add_custom_target(envy_licenses_collected DEPENDS "${ARG_OUTPUT}")
endfunction()
