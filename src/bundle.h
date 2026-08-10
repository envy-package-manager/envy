#pragma once

#include "pkg_cfg.h"

#include "sol/forward.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace envy {

// Parsed in-memory representation of envy-bundle.lua
// Immutable after construction, shared across all specs from this bundle
struct bundle {
  std::string identity;                                // "namespace.name@revision"
  int schema{ 0 };                                     // @envy schema (0 = absent)
  std::unordered_map<std::string, std::string> specs;  // spec identity -> relative path
  std::filesystem::path cache_path;  // e.g., ~/.envy/specs/acme.toolchain@v1/

  // Look up spec path within bundle. Returns empty path if not found.
  std::filesystem::path resolve_spec_path(std::string const &spec_identity) const;

  // Parse envy-bundle.lua from cache_path and construct bundle
  // Throws on parse error or validation failure
  static bundle from_path(std::filesystem::path const &cache_path);

  // Validate bundle (threaded):
  // - All spec files exist at declared paths
  // - All spec files execute successfully in Lua
  // - All spec files have IDENTITY matching the SPECS table key
  // Throws with detailed error message on failure
  void validate() const;

  // Parse BUNDLES table from manifest into alias -> fetch config map
  // Returns empty map if bundles_obj is nil or missing
  // Throws on invalid format
  static std::unordered_map<std::string, pkg_cfg::bundle_source> parse_aliases(
      sol::object const &bundles_obj,
      std::filesystem::path const &base_path);

  // Parse inline bundle = {...} declaration directly to bundle_source
  // Throws on invalid format
  static pkg_cfg::bundle_source parse_inline(sol::table const &table,
                                             std::filesystem::path const &base_path);

  // Bundle → BUNDLE_ONLY package cfg, memoized by bundle identity. Every
  // referenced bundle becomes a package so it rides the ordinary scheduling,
  // progress-bar, and outcome-row machinery instead of a bundle-only copy of it;
  // spec-from-bundle cfgs name it as a source dependency, which blocks their
  // spec_fetch until the bundle is materialized.
  // `declared_by` is the spec whose DEPENDENCIES declared the bundle (null for a
  // manifest-declared one) and decides where a custom fetch function is looked up.
  // It must be the declarer, never a consumer: consumers are blocked on the bundle,
  // so their Lua state is not loaded when the bundle runs.
  static pkg_cfg *ensure_pkg_cfg(pkg_cfg::bundle_source const &src,
                                 std::filesystem::path const &decl_path,
                                 pkg_cfg const *declared_by,
                                 std::unordered_map<std::string, pkg_cfg *> &memo);

  // Configure an existing lua state's package.path to include this bundle's root.
  // Call this before loading any spec files from the bundle.
  void configure_package_path(sol::state &lua) const;
};

}  // namespace envy
