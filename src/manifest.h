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

  // Anchor precedence: explicit_path (this file, no walk), then project_dir, then the
  // CWD. nearest=true stops at the first envy.lua. Absolute path, or throws.
  static std::filesystem::path find_manifest_path(
      std::optional<std::filesystem::path> const &explicit_path,
      bool nearest,
      std::optional<std::filesystem::path> const &project_dir = std::nullopt);

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

  // Where the upward walk starts: project_dir canonicalized, else the CWD.
  static std::filesystem::path discovery_start_dir(
      std::optional<std::filesystem::path> const &project_dir);

  // Which project a command settled on, and what anchored it. A caller that walks with
  // discover() itself reports here; discover() also probes (deploy's bin-dir round trip),
  // and a probe must not claim to be the command's answer.
  static void trace_resolved(std::filesystem::path const &path,
                             std::filesystem::path const &anchor,
                             char const *mode,
                             bool nearest);

  // Discover + load, with find_manifest_path's anchor precedence.
  static std::unique_ptr<manifest> find_and_load(
      std::optional<std::filesystem::path> const &explicit_path,
      bool nearest = false,
      std::optional<std::filesystem::path> const &project_dir = std::nullopt);

  static std::unique_ptr<manifest> load(std::filesystem::path const &manifest_path);
  static std::unique_ptr<manifest> load(std::vector<unsigned char> const &content,
                                        std::filesystem::path const &manifest_path);
  static std::unique_ptr<manifest> load(char const *script,
                                        std::filesystem::path const &manifest_path);

  // Parse the DEFAULT_SHELL global. Value forms (ENVY_SHELL constant, custom shell
  // table) resolve here; a function — bare, or the SHELL field of a
  // {DEPENDS, SHELL} table — only records that it must be evaluated later.
  // Returns a decl with no value and no depends when DEFAULT_SHELL is absent.
  default_shell_decl get_default_shell() const;

  // Execute the DEFAULT_SHELL function under the manifest Lua lock with `phase_ctx`
  // installed, so envy.product/envy.package resolve against the caller's consumer.
  // Throws on Lua error or an unusable return value.
  default_shell_value run_default_shell_fn(void *phase_ctx) const;

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
