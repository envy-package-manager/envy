#pragma once

#include "bundle.h"
#include "cache.h"
#include "package_depot.h"
#include "pkg_cfg.h"
#include "pkg_key.h"
#include "pkg_phase.h"
#include "shell.h"
#include "task_engine.h"
#include "util.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace envy {

struct manifest;
struct pkg;

enum class pkg_type {
  UNKNOWN,        // Not yet determined or failed
  CACHE_MANAGED,  // Package produces cached artifacts (has fetch)
  USER_MANAGED,   // Package managed by user (has check/install, no cache artifacts)
  BUNDLE_ONLY     // Pure bundle dependency (no spec, just bundle for envy.loadenv_spec())
};

struct pkg_result {
  pkg_type type;
  std::string result_hash;  // BLAKE3(format_key()) if cache-managed, empty otherwise
  std::filesystem::path pkg_path;  // Path to pkg/ dir (empty if user-managed/unknown)
};

using pkg_result_map_t = std::unordered_map<std::string, pkg_result>;

struct export_phase_config {
  std::filesystem::path output_dir;
  std::optional<std::string> depot_prefix;
  bool explicitly_requested{ false };
  std::unordered_set<pkg_key> export_targets;
};

struct product_info {
  std::string product_name;
  std::string value;
  std::string provider_canonical;  // Full canonical identity with options
  pkg_type type;
  std::filesystem::path pkg_path;
  bool script = true;
  std::vector<std::string> platforms;  // Effective constraint (empty = all)
};

// Domain adapter over task_engine: packages are tasks whose steps are the
// pkg_phase ladder; SETUP pairs are one-step tasks spawned by their package's
// setup phase. All envy semantics (spec parsing, weak-reference/product
// resolution, registries) live here; scheduling lives in task_engine.
class engine : unmovable {
 public:
  engine(cache &cache, manifest const *manifest = nullptr);
  ~engine();

  pkg *ensure_pkg(pkg_cfg const *cfg);

  pkg *find_exact(pkg_key const &key) const;
  std::vector<pkg *> find_matches(std::string_view query) const;
  pkg *find_product_provider(std::string const &product_name) const;
  std::vector<product_info> collect_all_products() const;

  // Start the package's worker (idempotent) and ratchet its target so it runs
  // through `run_through` inclusive.
  void start_pkg_thread(pkg *p,
                        pkg_phase run_through,
                        std::vector<std::string> ancestor_chain = {});

  // Ratchet a package's target to full completion (no wait).
  void extend_to_completion(pkg_key const &key);

  // Block until the package has fully completed; throws its failure message.
  void wait_for_completion(pkg_key const &key);

  // Spawn one single-step task per selected SETUP pair of `parent` (sibling
  // DEPENDS become edges), wait for all of them, and aggregate failures into
  // one exception. Called by the parent's setup phase.
  void run_setup_pairs_for(pkg *parent, std::vector<std::string> const &pair_names);

  // High-level execution
  pkg_result_map_t run_full(std::vector<pkg_cfg const *> const &roots);

  void resolve_graph(std::vector<pkg_cfg const *> const &roots);

  struct weak_resolution_result {
    size_t resolved{ 0 };
    size_t fallbacks_started{ 0 };
    std::vector<std::string> missing_without_fallback;
  };
  weak_resolution_result resolve_weak_references();

  void extend_dependencies_to_completion(pkg *p);

  std::filesystem::path const &cache_root() const;

  // Bundle registry management
  // Register a fetched bundle; returns existing if already registered
  bundle *register_bundle(std::string const &identity,
                          std::unordered_map<std::string, std::string> specs,
                          std::filesystem::path cache_path);

  bundle *find_bundle(std::string const &identity) const;

  manifest const *get_manifest() const;
  void set_depot_index(package_depot_index idx);
  void set_ignore_depot(bool ignore);

  // Depot access for the import phase. Lazily interns + starts the single-step
  // "#depot" task on first use (fetches every PACKAGE_DEPOTS manifest, running
  // FETCH functions after their DEPENDS complete) and blocks until the merged
  // index is published. Returns nullptr when no depot is configured, depot is
  // ignored, or `p` is in the depot's DEPENDS closure (depot_bootstrap —
  // bootstrap packages never consult the depot). Throws if the depot task or a
  // depot dependency failed.
  package_depot_index const *depot_index_for(pkg *p);

  // Flag `p` and its dependency closure as depot-bootstrap and wake any
  // blocked depot waits. Idempotent.
  void mark_depot_bootstrap(pkg *p);

  // Export phase configuration — set before resolve_graph() for pipeline export
  void set_export_config(export_phase_config cfg);
  export_phase_config const *export_config() const;

  // Thread-safe export result collection (called by phase handler)
  void record_export_result(pkg_key const &key, std::string output_line);
  std::string const *get_export_result(pkg_key const &key) const;

#ifdef ENVY_UNIT_TEST
  pkg_phase get_pkg_target_phase(pkg_key const &key) const;
#endif

 private:
  // Watermark mapping: pkg_phase `p` as a "run/wait through p inclusive" bound
  // is watermark int(p)+1 (task_engine watermark N = first N steps completed).
  static constexpr int watermark_through(pkg_phase p) { return static_cast<int>(p) + 1; }

  task_engine::task_config make_pkg_task_config(pkg *p);
  task_engine::observer make_trace_observer();
  std::string trace_display(std::string const &key) const;
  void process_fetch_dependencies(pkg *p);

  // Publish `p`'s PRODUCTS into the project-wide registry, called by p's own
  // worker the instant its spec is known. Eager (not barrier-batched) so a
  // consumer whose dependency edge forced `p` through pkg_export is guaranteed
  // to observe the entry — that edge is what makes the provider's payload
  // readable, so registry visibility must not lag behind it.
  void register_products(pkg *p);
  void validate_product_fallbacks();
  void validate_setup_selections();
  bool pkg_provides_product_transitively(pkg *p, std::string const &product_name) const;
  void extend_dependencies_recursive(pkg *p, std::unordered_set<pkg_key> &visited);
  void wait_for_resolution_phase();
  void on_spec_fetch_start();
  void on_spec_fetch_complete(std::string const &pkg_identity);

  enum class depot_state : int { NOT_READY, READY, FAILED };

  static constexpr char kDepotTaskKey[]{ "#depot" };

  void ensure_depot_task_started();
  std::vector<pkg *> spawn_depot_dependencies();  // #depot on_start (worker thread)
  void run_depot_step();                          // #depot step 0 (worker thread)

  cache &cache_;
  default_shell_cfg_t default_shell_;
  manifest const *manifest_{ nullptr };  // For bundle fetch function lookup

  // Depot state machine: importers block on the global condition until READY/
  // FAILED (or their own depot_bootstrap flag flips). depot_index_ is written
  // by the #depot worker (or set_depot_index) strictly before READY publishes.
  std::once_flag depot_task_once_;
  std::optional<package_depot_index> depot_index_;
  std::atomic<depot_state> depot_state_{ depot_state::NOT_READY };
  std::string depot_error_;  // written by #depot worker before FAILED publishes
  std::atomic_bool depot_pre_set_{ false };
  std::atomic_bool depot_ignored_{ false };

  // #depot worker-local: written in on_start, read by edges/step on the same
  // thread. lua_index → deps for each FETCH entry; flat list for edge queries.
  std::vector<std::pair<size_t, std::vector<pkg *>>> depot_fn_deps_;
  std::vector<pkg *> depot_edge_deps_;

  // Domain state (mutex_ guards the maps below; never held across core_ waits)
  std::unordered_map<pkg_key, std::unique_ptr<pkg>> packages_;
  mutable std::mutex mutex_;
  std::atomic_int pending_spec_fetches_{ 0 };

  // Product registry: maps product name → provider package (built during resolution)
  std::unordered_map<std::string, pkg *> product_registry_;

  // Bundle registry: maps bundle identity → bundle (populated during fetch)
  std::unordered_map<std::string, std::unique_ptr<bundle>> bundle_registry_;

  // Export phase state (set before resolve_graph, read by phase handler)
  std::optional<export_phase_config> export_config_;
  std::unordered_map<std::string, std::string> export_results_;  // guarded by mutex_

  // Declared last: workers capture pkg*/this, so the core (which joins them)
  // must be destroyed before the maps above.
  task_engine core_;
};

// Filter pkg_cfgs to those matching the current host platform.
// Packages with empty platforms match all hosts.
std::vector<pkg_cfg const *> engine_filter_host_platform(
    std::vector<pkg_cfg const *> const &cfgs);

// Resolve manifest packages against optional query list.
// Empty queries = all packages (unfiltered — callers handle platform filtering).
// Non-empty queries = match each query, throw if not found or wrong platform.
// cmd_name used in error messages (e.g. "deploy", "sync").
std::vector<pkg_cfg const *> engine_resolve_targets(
    std::vector<pkg_cfg *> const &packages,
    std::vector<std::string> const &queries,
    std::string const &cmd_name);

// Validate that adding candidate_identity as a dependency doesn't create a cycle
// Checks for self-loops and cycles in ancestor_chain, throws on detection
// dependency_type used for error messages (e.g., "Dependency" or "Fetch dependency")
void engine_validate_dependency_cycle(std::string const &candidate_identity,
                                      std::vector<std::string> const &ancestor_chain,
                                      std::string const &current_identity,
                                      std::string const &dependency_type);

}  // namespace envy
