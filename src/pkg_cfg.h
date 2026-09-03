#pragma once

#include "pkg_phase.h"
#include "util.h"

#include "sol/forward.hpp"

#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace envy {

class pkg_cfg_pool;

// What declared an entry, and what its relative paths resolve against: the two differ
// only for an imported manifest, whose paths were written relative to itself.
struct pkg_decl_origin {
  std::filesystem::path declaring_file;  // provenance, project root, custom-fetch key
  std::filesystem::path anchor;          // relative-path base; its parent dir is used

  // Implicit on purpose: every declaration but an imported one anchors on its declarer.
  pkg_decl_origin(std::filesystem::path p) : declaring_file{ p }, anchor{ std::move(p) } {}
  pkg_decl_origin(std::filesystem::path declarer, std::filesystem::path base)
      : declaring_file{ std::move(declarer) }, anchor{ std::move(base) } {}
};

// Reserved keys an imported manifest's entries carry: the file their relative paths
// anchor on, and the BUNDLES table their aliases resolve against.
inline constexpr char kEnvyBaseKey[]{ "ENVY_BASE" };
inline constexpr char kEnvyBundlesKey[]{ "ENVY_BUNDLES" };

struct pkg_cfg : unmovable {
 private:
  struct ctor_tag {
    ctor_tag() = default;
    friend class pkg_cfg_pool;
  };

 public:
  ~pkg_cfg() = default;

  struct remote_source {
    std::string url;
    std::string sha256;
    std::optional<std::string> subdir;  // Path within archive to spec entry point
  };

  struct local_source {
    std::filesystem::path file_path;  // Can be file or directory
  };

  struct git_source {
    std::string url;
    std::string ref;  // commit SHA or committish
    std::optional<std::string> subdir;
  };

  struct fetch_function {};  // Spec defines custom fetch()

  struct weak_ref {};  // Reference-only or weak dependency (no source)

  // Custom fetch source for bundles: fetch function + dependencies
  struct custom_fetch_source {
    std::vector<pkg_cfg *> dependencies;  // Needed before fetch function can run
  };

  // Bundle source: spec comes from within a bundle
  struct bundle_source {
    std::string bundle_identity;  // The bundle's identity
    // The underlying fetch source for the bundle itself
    std::variant<remote_source, local_source, git_source, custom_fetch_source>
        fetch_source;
  };

  using source_t = std::variant<remote_source,
                                local_source,
                                git_source,
                                fetch_function,
                                weak_ref,
                                bundle_source>;

  pkg_cfg(ctor_tag,
          std::string identity,
          source_t source,
          std::string serialized_options,
          std::optional<pkg_phase> needed_by,
          pkg_cfg const *parent,
          pkg_cfg *weak,
          std::vector<pkg_cfg *> source_dependencies,
          std::optional<std::string> product,
          std::filesystem::path declaring_file_path);

  std::string identity;  // "namespace.name@version"
  source_t source;
  std::string serialized_options;      // Serialized Lua table literal (empty "{}" if none)
  std::optional<pkg_phase> needed_by;  // Phase dependency annotation
  mutable pkg_cfg const *parent{ nullptr };  // Owning parent cfg
  pkg_cfg *weak{ nullptr };                  // Weak fallback cfg (if any)

  // Custom source fetch (nested source dependencies)
  std::vector<pkg_cfg *> source_dependencies{};  // Needed for fetching this spec

  // Product name if this is a product-based dependency
  std::optional<std::string> product;

  // Provenance: manifest or parent spec file that declared this cfg
  std::filesystem::path declaring_file_path;

  // Platform constraints (empty = all platforms)
  std::vector<std::string> platforms;

  // SETUP pair selection (manifest or dependency entries; never hashed into the
  // package key). Explicit-only: nullopt or empty list selects nothing; the
  // effective set is the union across all referrers, closed over pair DEPENDS.
  std::optional<std::vector<std::string>> setup;

  // Bundle-related fields (for specs that come from bundles)
  std::optional<std::string> bundle_identity;  // Which bundle contains this spec
  std::optional<std::string> bundle_path;      // Relative path within bundle to spec file

  // Parse pkg_cfg from Sol2 object (allocates via pool)
  static pkg_cfg *parse(sol::object const &lua_val,
                        pkg_decl_origin const &origin,
                        bool allow_weak_without_source = false);

  // Parse pkg_cfg directly from Lua stack (for tables containing functions)
  // Used primarily for testing; production code should use parse() with sol::object
  static pkg_cfg *parse_from_stack(sol::state_view lua,
                                   int index,
                                   pkg_decl_origin const &origin,
                                   bool allow_weak_without_source = false);

  // Parse one `source.dependencies` entry, for either a spec's source table or a
  // BUNDLES declaration's. Same as parse(..., true) except that the entry must name
  // a 'spec' and be a strong reference — reference-only and `weak = {...}` forms are
  // both rejected. The weak pass runs only at a resolution barrier, after every
  // spec_fetch has finished, so no dependency edge could ever gate the fetch
  // function waiting on the entry.
  static pkg_cfg *parse_fetch_dependency(sol::object const &entry,
                                         pkg_decl_origin const &origin);

  // Serialize sol::object to canonical string for stable package option hashing
  static std::string serialize_option_table(sol::object const &val);

  // Format canonical key: "identity" or "identity{opt=val,...}"
  // Used for logging, result maps, and any place needing a unique package identifier
  static std::string format_key(std::string const &identity,
                                std::string const &serialized_options);

  std::string format_key() const;
  bool is_remote() const;
  bool is_local() const;
  bool is_git() const;
  bool has_fetch_function() const;
  bool is_weak_reference() const;
  bool is_bundle_source() const;
  bool is_from_bundle() const;  // True if this spec comes from within a bundle

  // Look up source.fetch function for a dependency from Lua state's DEPENDENCIES global
  // Returns the fetch function if found, nullopt otherwise
  static std::optional<sol::protected_function> get_source_fetch(
      sol::state_view lua,
      std::string const &dep_identity);

  // Look up bundle source.fetch function for a bundle dependency from Lua state
  // Searches DEPENDENCIES for entries with bundle=identity and source={fetch=...}
  // Returns the fetch function if found, nullopt otherwise
  static std::optional<sol::protected_function> get_bundle_fetch(
      sol::state_view lua,
      std::string const &bundle_identity);

  static void set_pool(pkg_cfg_pool *pool);
  static pkg_cfg_pool *pool();

  // Compute project root directory from pkg cfg's declaring file path.
  // Walks up to root cfg and returns parent directory of manifest file.
  // Falls back to current_path() if no declaring file path is available.
  static std::filesystem::path compute_project_root(pkg_cfg const *cfg);

 private:
  friend class pkg_cfg_pool;
  static pkg_cfg_pool *pool_;
};

class pkg_cfg_pool {
 public:
  template <class... Args>
  pkg_cfg *emplace(Args &&...args) {
    std::lock_guard const lock(mutex_);
    storage_.emplace_back(pkg_cfg::ctor_tag{}, std::forward<Args>(args)...);
    return &storage_.back();
  }

 private:
  std::mutex mutex_;
  std::deque<pkg_cfg> storage_;
};

bool operator==(pkg_cfg::remote_source const &lhs, pkg_cfg::remote_source const &rhs);
bool operator==(pkg_cfg::local_source const &lhs, pkg_cfg::local_source const &rhs);
bool operator==(pkg_cfg::git_source const &lhs, pkg_cfg::git_source const &rhs);

// How two declarations of one bundle relate. A custom fetch source carries a Lua
// closure (and per-parse cfg pointers), so two of them can be neither proven the
// same nor proven different — hence the third answer.
enum class bundle_source_match { SAME, DIFFERENT, INCOMPARABLE };

bundle_source_match bundle_source_compare(pkg_cfg::bundle_source const &lhs,
                                          pkg_cfg::bundle_source const &rhs);

}  // namespace envy
