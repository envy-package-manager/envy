#include "pkg_phase.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace envy {

namespace {

// Enum-to-string mapping (order must match pkg_phase enum in pkg_phase.h)
// Index = enum value, so none (-1) and completion handled specially
constinit std::array<std::string_view, 10> const pkg_phase_name_table{ {
    "spec_fetch",  // pkg_phase::spec_fetch (0)
    "check",       // pkg_phase::pkg_check (1)
    "import",      // pkg_phase::pkg_import (2)
    "fetch",       // pkg_phase::pkg_fetch (3)
    "stage",       // pkg_phase::pkg_stage (4)
    "build",       // pkg_phase::pkg_build (5)
    "install",     // pkg_phase::pkg_install (6)
    "setup",       // pkg_phase::pkg_setup (7)
    "export",      // pkg_phase::pkg_export (8)
    "vendor",      // pkg_phase::pkg_vendor (9)
} };

static_assert(pkg_phase_name_table.size() ==
                  static_cast<std::size_t>(pkg_phase::completion),
              "every phase but completion needs a name here");

}  // namespace

std::string_view pkg_phase_name(pkg_phase p) {
  if (p == pkg_phase::none) { return "none"; }
  if (p == pkg_phase::completion) { return "completion"; }
  auto const idx{ static_cast<std::size_t>(p) };
  if (idx >= pkg_phase_name_table.size()) { return "unknown"; }
  return pkg_phase_name_table[idx];
}

std::optional<pkg_phase> pkg_phase_parse(std::string_view name) {
  if (name == "none") { return pkg_phase::none; }
  if (name == "completion") { return pkg_phase::completion; }

  if (auto it{ std::ranges::find(pkg_phase_name_table, name) };
      it != pkg_phase_name_table.end()) {
    return static_cast<pkg_phase>(std::distance(pkg_phase_name_table.begin(), it));
  }
  return std::nullopt;
}

pkg_phase pkg_phase_parse_needed_by(std::string_view name, std::string_view context) {
  if (auto const p{ pkg_phase_parse(name) };
      p && *p >= pkg_phase::pkg_check && *p <= pkg_phase::pkg_install) {
    return *p;
  }
  throw std::runtime_error(std::string{ context } +
                           " 'needed_by' must be one of: check, import, fetch, stage, "
                           "build, install (got: " +
                           std::string{ name } + ")");
}

}  // namespace envy
