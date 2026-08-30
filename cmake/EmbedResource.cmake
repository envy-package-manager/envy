# EmbedResource.cmake - Generate C++ header from embedded files
#
# embed_resources(<custom_target_name>
#     OUTPUT <header_name>
#     RESOURCES <var1>=<file1> <var2>=<file2> ...
#     [DEFINES <KEY1>=<VALUE1> <KEY2>=<VALUE2> ...]
#     [NORMALIZE_EOL]
#     [COMPRESS]
# )
#
# Creates a custom target that generates ${CMAKE_CURRENT_BINARY_DIR}/generated/<header_name>
# containing all resources as constexpr byte arrays. Re-generates when sources change.
# DEFINES replaces @@KEY@@ with VALUE in resource content at embed time.
# NORMALIZE_EOL converts CR/CRLF to LF in all resources (use for text resources only).
# COMPRESS gzips each resource, emitting a gz_resource instead of a raw byte array.

function(embed_resources CUSTOM_TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "NORMALIZE_EOL;COMPRESS" "OUTPUT" "RESOURCES;DEFINES")

    if(NOT ARG_OUTPUT)
        message(FATAL_ERROR "embed_resources: OUTPUT required")
    endif()
    if(NOT ARG_RESOURCES)
        message(FATAL_ERROR "embed_resources: RESOURCES required")
    endif()

    set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(OUTPUT_FILE "${OUTPUT_DIR}/${ARG_OUTPUT}")
    set(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/embed_resource.py")

    # Build command arguments
    set(CMD_ARGS "${OUTPUT_FILE}")

    if(ARG_NORMALIZE_EOL)
        list(APPEND CMD_ARGS "--normalize-eol")
    endif()

    if(ARG_COMPRESS)
        list(APPEND CMD_ARGS "--compress")
    endif()

    # Add -D flags for defines
    foreach(DEF IN LISTS ARG_DEFINES)
        list(APPEND CMD_ARGS "-D" "${DEF}")
    endforeach()

    # Collect source files for DEPENDS and add var=file args
    set(SOURCE_FILES "")
    foreach(RES IN LISTS ARG_RESOURCES)
        string(REGEX MATCH "^[^=]+=" VARNAME_EQ "${RES}")
        string(REGEX REPLACE "^[^=]+=" "" FILEPATH "${RES}")
        get_filename_component(FILEPATH_ABS "${FILEPATH}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND SOURCE_FILES "${FILEPATH_ABS}")
        string(REGEX REPLACE "=$" "" VARNAME "${VARNAME_EQ}")
        list(APPEND CMD_ARGS "${VARNAME}=${FILEPATH_ABS}")
    endforeach()

    add_custom_command(
        OUTPUT "${OUTPUT_FILE}"
        COMMAND "${Python3_EXECUTABLE}" "${SCRIPT}" ${CMD_ARGS}
        DEPENDS ${SOURCE_FILES} "${SCRIPT}"
        COMMENT "Embedding resources -> ${ARG_OUTPUT}"
        VERBATIM
    )

    add_custom_target(${CUSTOM_TARGET} DEPENDS "${OUTPUT_FILE}")
endfunction()
