#pragma once

#include "cache.h"
#include "pkg_cfg.h"
#include "pkg_key.h"
#include "pkg_phase.h"
#include "shell.h"
#include "sol_util.h"
#include "tui.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace envy {

enum class pkg_type;

// Closures whose members run outside the window where resolve_weak_references() can
// satisfy a reference, so no member of one may hold a weak reference. Set via
// engine::mark_closure, which propagates transitively over dependencies.
enum class pkg_closure : uint8_t {
  // package-depot DEPENDS closure: never consults the depot, which breaks the
  // bootstrap circularity, and may run after the resolution loop has finished.
  depot_bootstrap = 1u << 0,
  // Some package's source.dependencies closure: runs its whole phase ladder during
  // graph resolution, because a consumer parked in spec_fetch waiting for it holds
  // the resolution barrier shut for that entire window.
  fetch = 1u << 1,
};

constexpr std::string_view pkg_closure_name(pkg_closure kind) {
  return kind == pkg_closure::depot_bootstrap ? "package-depot dependency closure"
                                              : "source.dependencies closure";
}

struct product_entry {
  std::string value;
  bool script = true;
  std::vector<std::string> platforms;  // empty = inherit from package
};

struct pkg {
  struct dependency_info {  // pkg and phase by which dependency must be complete
    pkg *p;
    pkg_phase needed_by;
  };

  struct product_dependency {  // product name, required phase, and resolved provider
    std::string name;
    pkg_phase needed_by{ pkg_phase::pkg_build };
    pkg *provider{ nullptr };
    std::string constraint_identity;
  };

  struct weak_reference {  // unresolved dependency, may match multiple packages or
                           // fallback
    std::string query;
    pkg_cfg const *fallback{ nullptr };
    pkg_phase needed_by{ pkg_phase::pkg_build };
    pkg *resolved{ nullptr };
    bool is_product{ false };
    std::string constraint_identity;
    std::vector<std::string> setup;
  };

  // Immutable after construction
  pkg_key const key;
  pkg_cfg const *const cfg;
  cache *const cache_ptr;
  default_shell_cfg_t const *const default_shell_ptr;
  tui::section_handle const tui_section;

  // Execution mirror: the phase currently executing on this package's worker
  // (written by the engine's step wrapper, read by lua_ctx access gating) and
  // whether spec_fetch has completed (read on failure, to balance the pending
  // spec-fetch count exactly once).
  std::atomic<pkg_phase> current_phase{ pkg_phase::none };
  std::atomic_bool spec_fetch_completed{ false };

  // Closure memberships as a pkg_closure bitmask; a package can be in more than one.
  std::atomic<uint8_t> closures{ 0 };
  bool in_closure(pkg_closure kind) const;

  // Outcome accounting, all read only by this package's own worker thread at
  // completion (single-writer/single-reader, no synchronization needed).
  // build_start is stamped when the worker begins spec_fetch; imported flips
  // true when the artifact came from a depot rather than a fresh build.
  std::chrono::steady_clock::time_point build_start{};
  bool imported{ false };
  bool was_cache_hit{ false };   // set by check when the payload was already cached
  bool bundle_in_situ{ false };  // BUNDLE_ONLY: local bundle used from its source dir

  // Ancestor identities for dependency-cycle detection. Set before this
  // package's worker starts; immutable after.
  std::vector<std::string> ancestor_chain;

  sol_state_guard lua;
  cache::scoped_entry_lock::ptr_t lock;

  // Single-writer fields (set during specific phases, read after)
  std::string canonical_identity_hash;
  std::filesystem::path pkg_path;
  std::optional<std::filesystem::path> spec_file_path;
  std::string result_hash;
  pkg_type type;
  int schema{ 0 };

  // SETUP pairs declared by the spec. Sorted map gives deterministic node
  // creation order. Written once during spec_fetch, read by the setup phase.
  struct setup_pair_decl {
    std::vector<std::string> platforms;  // empty = all
    std::vector<std::string> depends;    // sibling pair names (parse-validated, acyclic)
  };
  std::map<std::string, setup_pair_decl> setup_pairs;

  // Dependency state — deps_mutex guards every field below. The engine's resolution
  // loop mutates these maps while worker threads traverse them. Lock one node at a
  // time; snapshot before recursing or blocking so no two pkg locks nest.
  mutable std::mutex deps_mutex;
  std::vector<std::string> declared_dependencies;
  std::vector<pkg_cfg *> owned_dependency_cfgs;
  std::unordered_map<std::string, dependency_info> dependencies;
  std::unordered_map<std::string, product_dependency> product_dependencies;
  std::vector<weak_reference> weak_references;
  std::unordered_map<std::string, product_entry> products;
  std::vector<std::string> resolved_platforms;
  std::vector<std::string> resolved_weak_dependency_keys;

  // SETUP selection: union of explicit `setup` lists from all referring cfgs
  // (manifest or dependency entries). Explicit-only — unselected pairs never
  // run. setup_selection_consumed flips when the setup phase snapshots the set;
  // a merge that adds names afterward is a hard error (selection arrived too late).
  std::unordered_set<std::string> setup_selected;
  bool setup_selection_consumed{ false };
};

inline bool pkg::in_closure(pkg_closure kind) const {
  return (closures.load() & static_cast<uint8_t>(kind)) != 0;
}

}  // namespace envy
