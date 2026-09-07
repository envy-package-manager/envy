#pragma once

#include "bundle.h"
#include "cache.h"
#include "package_depot.h"
#include "pkg_cfg.h"
#include "pkg_key.h"
#include "pkg_phase.h"
#include "shell.h"
#include "task_engine.h"
#include "tui.h"
#include "util.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace envy {

struct manifest;
struct pkg;
enum class pkg_closure : uint8_t;

enum class pkg_type {
  UNKNOWN,        // Not yet determined or failed
  CACHE_MANAGED,  // Package produces cached artifacts (has fetch)
  USER_MANAGED,   // Package managed by user (has check/install, no cache artifacts)
  BUNDLE_ONLY     // Pure bundle dependency (no spec, just bundle for envy.loadenv_spec())
};

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

  // The only place pkg::dependencies gains an entry. Under mutex_ (so two workers
  // cannot both pass the check and then both insert): refuses an edge whose target
  // already reaches `parent`, naming the cycle path; refuses an identity already
  // bound to a different package, since the map and the Lua API are identity-keyed;
  // otherwise inserts, or merges `needed_by` downward. Then carries `parent`'s
  // closure memberships to `dep`. `kind` prefixes the cycle message.
  void wire_dependency(pkg *parent,
                       pkg *dep,
                       pkg_phase needed_by,
                       std::string_view kind = "Dependency");

  // Start the package's worker (idempotent) and ratchet its target so it runs
  // through `run_through` inclusive.
  void start_pkg_thread(pkg *p, pkg_phase run_through);

  // Ratchet a package's target to full completion (no wait).
  void extend_to_completion(pkg_key const &key);

  // Block until the package has fully completed; throws its failure message.
  void wait_for_completion(pkg_key const &key);

  // Spawn one single-step task per selected SETUP pair of `parent` (sibling
  // DEPENDS become edges), wait for all of them, and re-throw the first failure
  // verbatim (each pair stores its own). Called by the parent's setup phase.
  void run_setup_pairs_for(pkg *parent, std::vector<std::string> const &pair_names);

  // Shell for `p`'s string verbs. A DEFAULT_SHELL function is evaluated on first
  // use — never at construction, where no package exists to own the interpreter
  // dependency — by the single-step "#default_shell" task, whose edges hold it
  // until DEPENDS is installed; the result is published once for the run. A null
  // `p`, or one in ANY bootstrap closure, gets the platform built-in instead:
  // that work runs before the manifest shell can exist. Throws the evaluation
  // failure to every caller.
  resolved_shell default_shell(pkg *p);

  // High-level execution: resolve, run every package to completion, throw the
  // aggregated failures. The engine's own state is the record of what happened.
  void run_full(std::vector<pkg_cfg const *> const &roots);

  void resolve_graph(std::vector<pkg_cfg const *> const &roots);

  struct weak_resolution_result {
    size_t resolved{ 0 };
    size_t fallbacks_started{ 0 };
    size_t unresolved{ 0 };
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
  // depot dependency failed. Mirrors default_shell()'s handshake exactly.
  package_depot_index const *depot_index_for(pkg *p);

  // Flag `p` and its dependency closure as a member of `kind`. Idempotent. Throws
  // if any member already holds an unresolved weak reference: no closure overlaps
  // the resolution barrier, so such a reference could only resolve after the phase
  // that needed it. Every newly flagged member wakes the global condition: the bit
  // is what releases a package already parked in a depot or default-shell wait.
  void mark_closure(pkg *p, pkg_closure kind);

  // Carry every closure membership from a package to a dependency just wired to it.
  // Every wiring site calls this, so a new closure kind is one edit in kAllClosures.
  void propagate_closures(pkg *from, pkg *to);

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

  std::unique_ptr<pkg> make_pkg(pkg_cfg const *cfg,
                                tui::section_handle section,
                                std::string canonical_identity_hash);
  task_engine::task_config make_pkg_task_config(pkg *p);
  task_engine::observer make_trace_observer();
  std::string trace_display(std::string const &key) const;
  void process_fetch_dependencies(pkg *p);

  // Every failed task's message, deduplicated and sorted, newline-joined. A
  // dependent re-stores its dependency's message verbatim, so the raw list repeats.
  std::string aggregated_failures() const;

  // Intern DEFAULT_SHELL.DEPENDS and flag the closure. resolve_graph calls it before
  // any worker exists, so a member's own string verbs see the carve-out without
  // racing; the #default_shell task repeats it (a no-op) and is what starts them.
  void intern_default_shell_deps();

  // Whether `p` is exempt from the manifest shell: no package at all, or one in a
  // bootstrap closure. Pure atomic reads — a wait predicate calls it.
  bool builtin_shell_only(pkg const *p) const;

  // Publish `p`'s PRODUCTS into the project-wide registry, called by p's own
  // worker the instant its spec is known. Eager (not barrier-batched) so a
  // consumer whose dependency edge forced `p` through pkg_export is guaranteed
  // to observe the entry — that edge is what makes the provider's payload
  // readable, so registry visibility must not lag behind it.
  void register_products(pkg *p);
  void validate_product_fallbacks();
  void validate_setup_selections();
  void extend_dependencies_recursive(pkg *p, std::unordered_set<pkg_key> &visited);
  void wait_for_resolution_phase();
  void on_spec_fetch_start();
  void on_spec_fetch_complete();

  // Both bootstrap tasks publish through the same three-state handshake: the value
  // is written before the atomic flips, and waiters read it after.
  enum class task_state : int { NOT_READY, READY, FAILED };

  static constexpr char kDepotTaskKey[]{ "#depot" };
  static constexpr char kDefaultShellTaskKey[]{ "#default_shell" };

  void ensure_depot_task_started();
  std::vector<pkg *> spawn_depot_dependencies();  // #depot on_start (worker thread)
  void run_depot_step();                          // #depot step 0 (worker thread)

  void ensure_default_shell_task_started();
  void run_default_shell_step();  // #default_shell step 0 (worker thread)

  cache &cache_;
  manifest const *manifest_{ nullptr };  // For bundle fetch function lookup

  // DEFAULT_SHELL: the declaration is parsed at construction (so a malformed one
  // fails the run early), the function form evaluated by the #default_shell task.
  // Callers block on the global condition until READY/FAILED (or their own closure
  // membership lands). default_shell_deps_ is written once, before any worker can
  // read it; the value and the error are written before the state flips.
  default_shell_decl default_shell_decl_;
  std::vector<pkg *> default_shell_deps_;
  std::once_flag default_shell_task_once_;
  std::atomic<task_state> default_shell_state_{ task_state::NOT_READY };
  default_shell_cfg_t default_shell_;
  std::string default_shell_error_;
  std::unique_ptr<pkg> default_shell_consumer_;

  // Depot state machine: importers block on the global condition until READY/
  // FAILED (or their own depot_bootstrap membership lands). depot_index_ is written
  // by the #depot worker (or set_depot_index) strictly before READY publishes.
  std::once_flag depot_task_once_;
  std::optional<package_depot_index> depot_index_;
  std::atomic<task_state> depot_state_{ task_state::NOT_READY };
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

// engine::default_shell for `p`, or the platform built-in when `p` has no engine
// (unit-test packages). Phase code reaches the manifest shell through here.
resolved_shell pkg_default_shell(pkg *p);

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

}  // namespace envy
