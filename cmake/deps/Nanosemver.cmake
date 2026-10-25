# nanosemver - in-place Semantic Versioning 2.0.0 parse/compare (C99)
# https://github.com/charlesnicholson/nanosemver

set(NANOSEMVER_CACHE_DIR "${ENVY_CACHE_DIR}/nanosemver-src")
file(MAKE_DIRECTORY "${NANOSEMVER_CACHE_DIR}")

# Prefer release assets to the tag's raw files: only the assets carry the stamped header.
envy_fetch_verified_file(
  PATH "${NANOSEMVER_CACHE_DIR}/nanosemver.h"
  URL "${ENVY_NANOSEMVER_URL}/nanosemver.h"
  SHA256 "${ENVY_NANOSEMVER_H_SHA256}"
  HUMAN_NAME "nanosemver ${ENVY_NANOSEMVER_VERSION}"
  SHOW_PROGRESS
)
envy_fetch_verified_file(
  PATH "${NANOSEMVER_CACHE_DIR}/nanosemver.c"
  URL "${ENVY_NANOSEMVER_URL}/nanosemver.c"
  SHA256 "${ENVY_NANOSEMVER_C_SHA256}"
)
envy_fetch_verified_file(
  PATH "${NANOSEMVER_CACHE_DIR}/LICENSE"
  URL "${ENVY_NANOSEMVER_LICENSE_URL}"
  SHA256 "${ENVY_NANOSEMVER_LICENSE_SHA256}"
)

if(NOT TARGET nanosemver::nanosemver)
  add_library(nanosemver STATIC "${NANOSEMVER_CACHE_DIR}/nanosemver.c")
  target_include_directories(nanosemver PUBLIC "${NANOSEMVER_CACHE_DIR}")
  target_compile_features(nanosemver PUBLIC c_std_99)
  target_compile_definitions(nanosemver PUBLIC NSV_DIAG=1)  # library and callers must agree
  add_library(nanosemver::nanosemver ALIAS nanosemver)
endif()
