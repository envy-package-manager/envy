# picojson - JSON parser/serializer (single-header)
# https://github.com/kazuho/picojson

set(PICOJSON_CACHE_DIR "${ENVY_CACHE_DIR}/picojson-src")
file(MAKE_DIRECTORY "${PICOJSON_CACHE_DIR}")

envy_fetch_verified_file(
  PATH "${PICOJSON_CACHE_DIR}/picojson.h"
  URL "${ENVY_PICOJSON_URL}"
  SHA256 "${ENVY_PICOJSON_SHA256}"
  HUMAN_NAME "picojson ${ENVY_PICOJSON_VERSION}"
  SHOW_PROGRESS
)
envy_fetch_verified_file(
  PATH "${PICOJSON_CACHE_DIR}/LICENSE"
  URL "${ENVY_PICOJSON_LICENSE_URL}"
  SHA256 "${ENVY_PICOJSON_LICENSE_SHA256}"
)

if(NOT TARGET picojson::picojson)
  add_library(picojson INTERFACE)
  target_include_directories(picojson INTERFACE "${PICOJSON_CACHE_DIR}")
  target_compile_definitions(picojson INTERFACE
    ENVY_PICOJSON_VERSION="${ENVY_PICOJSON_VERSION}")
  add_library(picojson::picojson ALIAS picojson)
endif()
