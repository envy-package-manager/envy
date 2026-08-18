#pragma once

#include "package_depot.h"
#include "pkg_cfg.h"
#include "shell.h"
#include "sol_util.h"
#include "util.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace envy {

// @envy metadata parsed from comment headers in manifest
struct envy_meta {
  int schema{ 0 };                         // @envy schema "N" (0 = absent)
  std::optional<std::string> version;      // @envy version "x.y.z"
  std::optional<std::string> cache_posix;  // @envy cache-posix (always parsed)
  std::optional<std::string> cache_win;    // @envy cache-win (always parsed)
  std::optional<std::string> mirror;       // @envy mirror "https://..."
  std::optional<std::string> sha256sums;   // @envy sha256sums "<64 hex of SHA256SUMS>"
  std::optional<std::string> bin;          // @envy bin "relative/path/to/bin"
  std::optional<bool> deploy;              // @envy deploy "true"/"false"
  std::optional<bool> root;                // @envy root "true"/"false"

  std::optional<std::string> const &cache_for_platform() const;
};

// Parse @envy metadata from manifest content. Directives are header comments, so the scan
// stops at the first line of code. Throws std::runtime_error on a directive that is
// present but unusable: a malformed sha256sums pin, or a sums pin with no '@envy version'
// to pin it to.
envy_meta parse_envy_meta(std::string_view content);

// Where one '@envy' directive sits in the manifest bytes: [value_begin, value_end) spans
// the bytes between its quotes, [line_begin, line_end) the line itself.
struct envy_directive_span {
  size_t line_begin{};
  size_t line_end{};  // index of the '\n' (or EOF): a CRLF line's '\r' is inside the span
  size_t value_begin{};
  size_t value_end{};
};

// Locate a header directive by key, sharing parse_envy_meta's grammar and header-end rule.
// Returns the last match, as parse_envy_meta's assignment does; nullopt if absent.
std::optional<envy_directive_span> find_envy_directive(std::string_view content,
                                                       std::string_view key);

struct manifest : unmovable {
  // PACKAGE_DEPOTS entry: plain URI, or FETCH function with optional package
  // DEPENDS (identities resolved against this manifest's PACKAGES).
  struct depot_uri {
    std::string url;
  };
  struct depot_fetch_fn {
    std::vector<std::string> depends;
    size_t lua_index{ 0 };  // 1-based index into the PACKAGE_DEPOTS global
  };
  using depot_source = std::variant<depot_uri, depot_fetch_fn>;

  // FETCH functions return depot manifest text (or a path to it — the caller
  // disambiguates) or an explicit entries table.
  using depot_fetch_result = std::variant<std::string, std::vector<depot_entry>>;

  std::vector<pkg_cfg *> packages;
  std::vector<depot_source> package_depots;
  std::filesystem::path manifest_path;
  envy_meta meta;

  manifest() = default;

  // Find manifest path: use provided path if given, otherwise discover from current
  // directory. When nearest=true, return the first envy.lua found (subproject mode).
  // Returns absolute path or throws if not found
  static std::filesystem::path find_manifest_path(
      std::optional<std::filesystem::path> const &explicit_path,
      bool nearest);

  // A manifest found on disk: its bytes, read once, and the '@envy' directives parsed out
  // of them. A caller that goes on to execute the manifest passes `content` to load()
  // rather than opening the file a second time.
  struct discovery {
    std::filesystem::path path;
    envy_meta meta;
    std::vector<unsigned char> content;
  };

  // Discover manifest by walking up from start_dir. When nearest=true, return the first
  // envy.lua found immediately instead of walking to the root manifest.
  static std::optional<discovery> discover(bool nearest,
                                           std::filesystem::path const &start_dir);

  // Discover + load. Uses explicit_path if given, otherwise discovers from CWD.
  static std::unique_ptr<manifest> find_and_load(
      std::optional<std::filesystem::path> const &explicit_path,
      bool nearest = false);

  static std::unique_ptr<manifest> load(std::filesystem::path const &manifest_path);
  static std::unique_ptr<manifest> load(std::vector<unsigned char> const &content,
                                        std::filesystem::path const &manifest_path);
  static std::unique_ptr<manifest> load(char const *script,
                                        std::filesystem::path const &manifest_path);

  // Get DEFAULT_SHELL global type and value
  // Returns nullopt if no DEFAULT_SHELL specified
  default_shell_cfg_t get_default_shell() const;

  // Execute bundle custom fetch function from BUNDLES table
  // Sets up phase context, executes fetch function, cleans up
  // Returns nullopt if bundle not found or has no custom fetch
  // Returns error message string on failure, nullopt on success
  std::optional<std::string> run_bundle_fetch(std::string const &bundle_identity,
                                              void *phase_ctx,
                                              std::filesystem::path const &tmp_dir) const;

  // Execute PACKAGE_DEPOTS[lua_index].FETCH(ctx) under the manifest Lua lock.
  // `deps` (identity → pkg_path) populates ctx.deps; ctx.tmp_dir = tmp_dir.
  // Throws on Lua error or an invalid return shape.
  depot_fetch_result run_depot_fetch(
      size_t lua_index,
      void *phase_ctx,
      std::filesystem::path const &tmp_dir,
      std::vector<std::pair<std::string, std::string>> const &deps) const;

 private:
  mutable std::mutex lua_mutex_;  // Protects lua_ access from concurrent threads
  sol_state_ptr lua_;
};

}  // namespace envy
