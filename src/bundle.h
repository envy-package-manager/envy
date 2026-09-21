#pragma once

#include "fetch.h"
#include "pkg_cfg.h"

#include "sol/forward.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace envy {

class cache;

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
  // A declaration tagged 'ENVY_BASE' anchors on that file instead of `origin`, which
  // is how an imported manifest's BUNDLES keep resolving against their own directory.
  static std::unordered_map<std::string, pkg_cfg::bundle_source> parse_aliases(
      sol::object const &bundles_obj,
      pkg_decl_origin const &origin);

  // Parse inline bundle = {...} declaration directly to bundle_source
  // Throws on invalid format
  static pkg_cfg::bundle_source parse_inline(sol::table const &table,
                                             pkg_decl_origin const &origin);

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

// One transfer of a bundle's payload. The caller owns progress; manifest scope has none.
using bundle_fetcher =
    std::function<void(fetch_request, std::string const &url, char const *what)>;

// A bundle's custom fetch. Needs a phase context, so it stays with whoever has one.
using bundle_custom_fetcher = std::function<void()>;

// Put a bundle's payload in `install_dir`, whatever its source shape. Shared with the
// BUNDLE_ONLY package's spec_fetch, so the two cannot drift.
void bundle_fetch_payload(pkg_cfg::bundle_source const &src,
                          std::filesystem::path const &fetch_dir,
                          std::filesystem::path const &install_dir,
                          bundle_fetcher const &fetch_one,
                          bundle_custom_fetcher const &run_custom);

// A bundle payload's cache entry key. `cfg` is read only for the custom-fetch shape,
// which has no fingerprint of its own; a caller without one passes null.
std::string bundle_source_key(pkg_cfg::bundle_source const &src, pkg_cfg const *cfg);

// Parse, identity-check and validate a materialized bundle, before its entry is
// finalized: `envy-complete` is never revalidated, so a bad one would be permanent.
bundle bundle_verify(std::filesystem::path const &root, std::string const &expected_id);

// A bundle's root, materialized with no engine, package or phase behind it -- manifest
// scope has none. Throws on custom fetch; a null `c` suits only a 'local.' bundle.
std::filesystem::path bundle_materialize_bare(pkg_cfg::bundle_source const &src,
                                              cache *c,
                                              std::string_view fn);

}  // namespace envy
