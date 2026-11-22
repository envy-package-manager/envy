cmake_path(APPEND ENVY_CACHE_DIR "${ENVY_LUA_ARCHIVE}" OUTPUT_VARIABLE _lua_archive)
set(_lua_url "${ENVY_LUA_URL}")
if(EXISTS "${_lua_archive}")
    file(TO_CMAKE_PATH "${_lua_archive}" _lua_archive_norm)
    set(_lua_url "file://${_lua_archive_norm}")
endif()

cmake_path(APPEND ENVY_CACHE_DIR "lua-src" OUTPUT_VARIABLE lua_SOURCE_DIR)
cmake_path(APPEND CMAKE_BINARY_DIR "_deps" "lua-build" OUTPUT_VARIABLE lua_BINARY_DIR)

envy_populate_pinned(lua "${lua_SOURCE_DIR}" "${lua_BINARY_DIR}"
    ${_lua_url} ${ENVY_LUA_SHA256})

if(NOT TARGET lua)
    set(_lua_source_root "${lua_SOURCE_DIR}")
    if(EXISTS "${_lua_source_root}/src/lapi.c")
        set(_lua_source_root "${lua_SOURCE_DIR}/src")
    endif()
    file(GLOB LUA_CORE_SOURCES
        "${_lua_source_root}/lapi.c"
        "${_lua_source_root}/lcode.c"
        "${_lua_source_root}/lctype.c"
        "${_lua_source_root}/ldebug.c"
        "${_lua_source_root}/ldo.c"
        "${_lua_source_root}/ldump.c"
        "${_lua_source_root}/lfunc.c"
        "${_lua_source_root}/lgc.c"
        "${_lua_source_root}/llex.c"
        "${_lua_source_root}/lmem.c"
        "${_lua_source_root}/lobject.c"
        "${_lua_source_root}/lopcodes.c"
        "${_lua_source_root}/lparser.c"
        "${_lua_source_root}/lstate.c"
        "${_lua_source_root}/lstring.c"
        "${_lua_source_root}/ltable.c"
        "${_lua_source_root}/ltm.c"
        "${_lua_source_root}/lundump.c"
        "${_lua_source_root}/lvm.c"
        "${_lua_source_root}/lzio.c"
        "${_lua_source_root}/lauxlib.c"
        "${_lua_source_root}/lbaselib.c"
        "${_lua_source_root}/lcorolib.c"
        "${_lua_source_root}/ldblib.c"
        "${_lua_source_root}/liolib.c"
        "${_lua_source_root}/lmathlib.c"
        "${_lua_source_root}/loslib.c"
        "${_lua_source_root}/lstrlib.c"
        "${_lua_source_root}/ltablib.c"
        "${_lua_source_root}/lutf8lib.c"
        "${_lua_source_root}/loadlib.c"
        "${_lua_source_root}/linit.c"
    )
    add_library(lua STATIC ${LUA_CORE_SOURCES})
    target_include_directories(lua PUBLIC "${lua_SOURCE_DIR}" "${_lua_source_root}")
    target_compile_definitions(lua PRIVATE
        $<$<NOT:$<PLATFORM_ID:Windows>>:LUA_USE_POSIX>
        $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
    )
    set_target_properties(lua PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON)
    if(NOT MSVC)
        target_compile_options(lua PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=c99>)
    endif()
    if(NOT WIN32)
        target_compile_options(lua PRIVATE -fexceptions)
    endif()
    add_library(lua::lua ALIAS lua)
    unset(_lua_source_root)
endif()
unset(_lua_archive)
unset(_lua_archive_norm)
unset(_lua_url)
