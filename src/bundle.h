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

// One transfer of a bundle's payload. The caller owns progress: a BUNDLE_ONLY
// package draws a row for it, manifest scope has no row to draw on.
using bundle_fetcher =
    std::function<void(fetch_request, std::string const &url, char const *what)>;

// A bundle's custom fetch, which needs a phase context and so stays with whoever
// has one. Called with the fetch dir it must leave `envy-bundle.lua` in.
using bundle_custom_fetcher = std::function<void()>;

// Put a bundle's payload in `install_dir`, whatever its source shape. Shared by the
// BUNDLE_ONLY package's spec_fetch and by manifest scope, so the two cannot drift on
// what a bundle declaration means.
void bundle_fetch_payload(pkg_cfg::bundle_source const &src,
                          std::filesystem::path const &fetch_dir,
                          std::filesystem::path const &install_dir,
                          bundle_fetcher const &fetch_one,
                          bundle_custom_fetcher const &run_custom);

// The cache entry key for a bundle's payload, the same canonical source description
// a spec's entry uses. `cfg` is read only for the custom-fetch shape, which has no
// fingerprint of its own, so a caller that cannot reach one passes null.
std::string bundle_source_key(pkg_cfg::bundle_source const &src, pkg_cfg const *cfg);

// Parse, identity-check and validate a materialized bundle. Callers run this while
// the cache entry is still unfinalized: `envy-complete` is never revalidated, so
// marking first would make a malformed bundle a permanent entry that fails
// identically on every later run.
bundle bundle_verify(std::filesystem::path const &root, std::string const &expected_id);

// Materialize a bundle with no engine, package or phase behind it -- manifest scope
// has none of the three -- and hand back its root. Cache-backed, so the BUNDLE_ONLY
// package the engine schedules later finds the entry already complete. Throws on a
// custom-fetch bundle, which has nowhere to run this early. A null `c` is a caller
// with no cache in hand: fine for a 'local.' bundle, which is read where it stands,
// and refused by name for every shape that has to be fetched.
std::filesystem::path bundle_materialize_bare(pkg_cfg::bundle_source const &src,
                                              cache *c,
                                              std::string_view fn);

}  // namespace envy
