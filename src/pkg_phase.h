#pragma once

#include <optional>
#include <string_view>

namespace envy {

enum class pkg_phase : int {
  none = -1,  // Not started yet
  spec_fetch = 0,
  pkg_check = 1,
  pkg_import = 2,
  pkg_fetch = 3,
  pkg_stage = 4,
  pkg_build = 5,
  pkg_install = 6,
  pkg_setup = 7,  // Host-side SETUP pairs (check-gated, ephemeral)
  pkg_export = 8,
  pkg_vendor = 9,   // Copy the payload into the project's vendor tree, if asked
  completion = 10,  // All phases complete
};

constexpr int pkg_phase_count = static_cast<int>(pkg_phase::completion) + 1;

std::string_view pkg_phase_name(pkg_phase p);
std::optional<pkg_phase> pkg_phase_parse(std::string_view name);

// Parse a user-facing `needed_by` phase name (check/import/fetch/stage/build/install).
// Throws std::runtime_error naming `context` on any other value.
pkg_phase pkg_phase_parse_needed_by(std::string_view name, std::string_view context);

}  // namespace envy
