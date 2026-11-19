# tomlc17 - TOML 1.1 parser (C99)
# https://github.com/cktan/tomlc17

set(TOMLC17_CACHE_DIR "${ENVY_CACHE_DIR}/tomlc17-src")
file(MAKE_DIRECTORY "${TOMLC17_CACHE_DIR}")

envy_fetch_verified_file(
  PATH "${TOMLC17_CACHE_DIR}/tomlc17.h"
  URL "${ENVY_TOMLC17_URL}/src/tomlc17.h"
  SHA256 "${ENVY_TOMLC17_H_SHA256}"
  HUMAN_NAME "tomlc17 ${ENVY_TOMLC17_VERSION}"
  SHOW_PROGRESS
)
envy_fetch_verified_file(
  PATH "${TOMLC17_CACHE_DIR}/tomlc17.c"
  URL "${ENVY_TOMLC17_URL}/src/tomlc17.c"
  SHA256 "${ENVY_TOMLC17_C_SHA256}"
)
envy_fetch_verified_file(
  PATH "${TOMLC17_CACHE_DIR}/LICENSE"
  URL "${ENVY_TOMLC17_URL}/LICENSE"
  SHA256 "${ENVY_TOMLC17_LICENSE_SHA256}"
)

if(NOT TARGET tomlc17::tomlc17)
  add_library(tomlc17 STATIC "${TOMLC17_CACHE_DIR}/tomlc17.c")
  target_include_directories(tomlc17 PUBLIC "${TOMLC17_CACHE_DIR}")
  target_compile_features(tomlc17 PUBLIC c_std_99)
  target_compile_definitions(tomlc17 PUBLIC ENVY_TOMLC17_VERSION="${ENVY_TOMLC17_VERSION}")
  add_library(tomlc17::tomlc17 ALIAS tomlc17)
endif()
