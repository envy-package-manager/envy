#include "engine.h"

#include "lua_ctx/lua_phase_context.h"
#include "manifest.h"
#include "package_depot.h"
#include "phases/phase_build.h"
#include "phases/phase_check.h"
#include "phases/phase_completion.h"
#include "phases/phase_export.h"
#include "phases/phase_fetch.h"
#include "phases/phase_import.h"
#include "phases/phase_install.h"
#include "phases/phase_setup.h"
#include "phases/phase_spec_fetch.h"
#include "phases/phase_stage.h"
#include "pkg.h"
#include "pkg_key.h"
#include "pkg_phase.h"
#include "platform.h"
#include "tui.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace envy {

namespace {

// Watermark after which a dependency's artifacts and host-side SETUP state are
// available: setup completed (= export may begin). Dependencies are waited on
// to this point (not completion) so that export can overlap with dependents'
// builds — the edge ratchets the dependency through export without waiting for
// it. Setup must complete before dependents proceed: user-managed packages do
// all their work in SETUP pairs.
constexpr int kDependencySatisfiedWatermark{ static_cast<int>(pkg_phase::pkg_setup) + 1 };
constexpr int kDependencyRunThroughWatermark{ static_cast<int>(pkg_phase::pkg_export) +
                                              1 };
static_assert(kDependencySatisfiedWatermark == static_cast<int>(pkg_phase::pkg_export) &&
                  kDependencySatisfiedWatermark < static_cast<int>(pkg_phase::completion),
              "pkg_setup must be followed by pkg_export before completion");

using phase_func_t = void (*)(pkg *, engine &);

constexpr std::array<phase_func_t, pkg_phase_count> phase_dispatch_table{
  run_spec_fetch_phase,  // pkg_phase::spec_fetch
  run_check_phase,       // pkg_phase::pkg_check
  run_import_phase,      // pkg_phase::pkg_import
  run_fetch_phase,       // pkg_phase::pkg_fetch
  run_stage_phase,       // pkg_phase::pkg_stage
  run_build_phase,       // pkg_phase::pkg_build
  run_install_phase,     // pkg_phase::pkg_install
  run_setup_phase,       // pkg_phase::pkg_setup
  run_export_phase,      // pkg_phase::pkg_export
  run_completion_phase,  // pkg_phase::completion
};

// Trace mapping: watermark w = "first w steps completed"; the corresponding
// "current" phase in legacy trace terms is the last completed step, w - 1.
constexpr pkg_phase phase_from_watermark(int w) { return static_cast<pkg_phase>(w - 1); }

constexpr bool is_setup_pair_key(std::string_view key) {
  return key.find("#setup:") != std::string_view::npos;
}

// Engine-owned task keys ("#depot", "#default_shell"): not packages, and pkg_key
// throws on them. A SETUP pair key is "<canonical>#setup:<name>", so it never leads.
constexpr bool is_engine_task_key(std::string_view key) { return key.starts_with('#'); }

// The synthetic package a manifest-wide DEFAULT_SHELL function authorizes against.
constexpr char kDefaultShellConsumerIdentity[]{ "envy.DEFAULT_SHELL@v1" };

// Machine-stable kind for the trace: which built-in, or which custom form.
constexpr std::string_view shell_kind_name(resolved_shell const &shell) {
  return std::visit(
      match{ [](shell_choice c) -> std::string_view {
              switch (c) {
                case shell_choice::bash: return "bash";
                case shell_choice::sh: return "sh";
                case shell_choice::cmd: return "cmd";
                case shell_choice::powershell: return "powershell";
              }
              return "unknown";
            },
             [](custom_shell_file const &) -> std::string_view { return "file"; },
             [](custom_shell_inline const &) -> std::string_view { return "inline"; } },
      shell);
}

// Walk `root` and everything reachable through its dependencies, calling `visit`
// once per package. One worklist serves the whole traversal, so this does not copy a
// node's dependency list per node; a node's lock is held only long enough to append
// its dependencies, and `visit` runs outside it, so no two pkg locks ever nest.
// `kind`'s bit doubles as the visited set: subtrees already in this closure are
// skipped, which also makes this terminate on a dependency cycle and cheap to call
// repeatedly. A package may be in several closures at once, so the bits are
// independent — marking one never re-walks or clears another.
template <typename Fn>
void walk_closure(pkg *root, pkg_closure kind, Fn &&visit) {
  auto const bit{ static_cast<uint8_t>(kind) };
  std::vector<pkg *> work{ root };
  while (!work.empty()) {
    pkg *p{ work.back() };
    work.pop_back();
    if (p->closures.fetch_or(bit) & bit) { continue; }

    visit(p);

    std::lock_guard const deps_lock(p->deps_mutex);
    for (auto const &[_, dep_info] : p->dependencies) { work.push_back(dep_info.p); }
  }
}

// The edge path from → … → to, or empty when `to` is unreachable. Each node's lock
// is held only long enough to snapshot its edges, so no two pkg locks ever nest.
std::vector<pkg *> dependency_path(pkg *from, pkg *to) {
  if (from == to) { return { from }; }

  std::unordered_map<pkg *, pkg *> came_from{ { from, nullptr } };
  std::vector<pkg *> stack{ from };

  while (!stack.empty()) {
    pkg *const current{ stack.back() };
    stack.pop_back();

    auto const children{ [current] {
      std::lock_guard const deps_lock(current->deps_mutex);
      std::vector<pkg *> c;
      c.reserve(current->dependencies.size());
      for (auto const &[_, dep_info] : current->dependencies) { c.push_back(dep_info.p); }
      return c;
    }() };

    for (pkg *child : children) {
      if (!came_from.emplace(child, current).second) { continue; }
      if (child != to) {
        stack.push_back(child);
        continue;
      }
      std::vector<pkg *> path{ child };
      for (pkg *at{ current }; at; at = came_from.at(at)) { path.push_back(at); }
      std::ranges::reverse(path);
      return path;
    }
  }

  return {};
}

// Two cfgs on one pkg_key: the key is identity plus options, never the source, so the cfg
// that won the insert would silently decide what gets fetched. Refuse a provable
// disagreement; warn when two Lua closures make the pair incomparable.
void validate_source_redeclaration(pkg_cfg const *winner, pkg_cfg const *other) {
  auto const where{ [](pkg_cfg const *cfg) {
    return cfg->declaring_file_path.empty() ? std::string{ "<unknown>" }
                                            : cfg->declaring_file_path.string();
  } };

  auto const *a{ std::get_if<pkg_cfg::bundle_source>(&winner->source) };
  auto const *b{ std::get_if<pkg_cfg::bundle_source>(&other->source) };
  bool const bundles{ a && b };
  std::string const what{ bundles ? "bundle" : "spec" };
  std::string const subject{ bundles ? a->bundle_identity : winner->identity };

  switch (pkg_cfg_source_compare(winner->source, other->source)) {
    case pkg_source_match::SAME: return;

    case pkg_source_match::INCOMPARABLE:
      // Custom fetch closures: both were written to produce this payload, so run the
      // winner rather than failing, but say which declaration lost.
      tui::warn(
          "%s '%s' declares a custom fetch function in both %s and %s; the one in %s "
          "will run",
          what.c_str(),
          subject.c_str(),
          where(winner).c_str(),
          where(other).c_str(),
          where(winner).c_str());
      return;

    case pkg_source_match::DIFFERENT:
      if (bundles && a->bundle_identity != b->bundle_identity) {
        throw std::runtime_error(
            "spec '" + winner->identity + "' is requested from two different bundles: '" +
            a->bundle_identity + "' (" + where(winner) + ") and '" + b->bundle_identity +
            "' (" + where(other) + "); one declaration must be corrected");
      }
      throw std::runtime_error(what + " '" + subject +
                               "' is declared with conflicting sources in " +
                               where(winner) + " and " + where(other) + "; a " + what +
                               " identity must name one payload");
  }
}

// Merge a weak reference's SETUP selection into the package it resolved to
// (union with all other referrers). Weak resolution wires to an existing package
// without an ensure_pkg call, so the selection is merged here instead. A closure
// member is refused outright: its setup phase runs either before the weak pass
// (fetch) or after it (depot_bootstrap, default_shell), so the selection would land
// too late or not at all — refuse deterministically instead of dropping pairs.
void merge_setup_selection(pkg *dep, std::vector<std::string> const &names) {
  if (names.empty()) { return; }

  for (auto const kind : kAllClosures) {
    if (!dep->in_closure(kind)) { continue; }
    throw std::runtime_error(
        "SETUP selection for " + dep->cfg->identity +
        " cannot come from a weak dependency: it is in the " +
        std::string{ pkg_closure_name(kind) } +
        ", whose setup phase runs outside the window where weak references resolve");
  }

  std::lock_guard const deps_lock(dep->deps_mutex);
  size_t const before{ dep->setup_selected.size() };
  dep->setup_selected.insert(names.begin(), names.end());
  if (dep->setup_selection_consumed && dep->setup_selected.size() != before) {
    throw std::runtime_error(
        "SETUP selection for " + dep->cfg->identity +
        " arrived after its setup phase ran; a weak dependency selected a pair "
        "that resolved too late");
  }
}

// Record `provider` as the answer to `wr`, under `p`'s lock: the provider's key
// joins the hash input here rather than in a post-resolution sweep, so a closure
// member that hashes mid-resolution already sees it.
void record_resolution(pkg *p, pkg::weak_reference *wr, pkg *provider) {
  std::lock_guard const deps_lock(p->deps_mutex);
  wr->resolved = provider;

  auto const key{ provider->key.canonical() };
  auto &keys{ p->resolved_weak_dependency_keys };
  if (auto const at{ std::ranges::lower_bound(keys, key) };
      at == keys.end() || *at != key) {
    keys.insert(at, key);
  }

  if (!wr->is_product) { return; }
  auto const it{ p->product_dependencies.find(wr->query) };
  if (it == p->product_dependencies.end()) { return; }
  it->second.provider = provider;
  if (!wr->constraint_identity.empty()) {
    it->second.constraint_identity = wr->constraint_identity;
  }
}

// Log + trace a resolved product dependency. No-op for non-product references.
// `via` is one of "registry" | "fallback".
void trace_product_resolution(pkg const *consumer,
                              pkg::weak_reference const *wr,
                              pkg const *provider,
                              char const *via) {
  if (!wr->is_product) { return; }
  bool const fallback{ std::string_view{ via } == "fallback" };
  tui::debug("resolve: [%s] product '%s' → %s%s",
             consumer->cfg->identity.c_str(),
             wr->query.c_str(),
             provider->cfg->identity.c_str(),
             fallback ? " (fallback)" : "");
  ENVY_TRACE(product_resolved,
             consumer->cfg->identity,
             .product = wr->query,
             .provider = provider->cfg->identity,
             .via = via);
}

void resolve_identity_ref(pkg *p,
                          pkg::weak_reference *wr,
                          engine::weak_resolution_result &result,
                          std::vector<std::string> &ambiguity_messages,
                          engine &eng) {
  auto matches{ eng.find_matches(wr->query) };

  if (matches.size() == 1) {
    pkg *dep{ matches[0] };
    eng.wire_dependency(p, dep, wr->needed_by);
    merge_setup_selection(dep, wr->setup);
    // A match can be interned but unstarted (DEFAULT_SHELL.DEPENDS is), and an edge
    // only ratchets a target — it never spawns the worker that would satisfy it.
    eng.start_pkg_thread(dep, pkg_phase::spec_fetch);
    record_resolution(p, wr, dep);
    ++result.resolved;
    return;
  }

  if (matches.size() > 1) {
    // find_matches walks an unordered map; sort so one graph always reports one
    // message.
    std::ranges::sort(matches, {}, [](pkg const *m) { return m->key.canonical(); });
    std::ostringstream oss;
    oss << "Reference '" << wr->query << "' in spec '" << p->cfg->identity
        << "' is ambiguous: ";
    for (size_t i = 0; i < matches.size(); ++i) {
      if (i) { oss << ", "; }
      oss << matches[i]->key.canonical();
    }
    ambiguity_messages.push_back(oss.str());
    return;
  }

  if (wr->fallback) {
    wr->fallback->parent = p->cfg;

    pkg *dep{ eng.ensure_pkg(wr->fallback) };
    eng.wire_dependency(p, dep, wr->needed_by);
    merge_setup_selection(dep, wr->setup);
    eng.start_pkg_thread(dep, pkg_phase::spec_fetch);
    record_resolution(p, wr, dep);
    ++result.fallbacks_started;
  }
}

// `registry_provider` is the registry's answer for wr->query, already read under
// the engine's mutex_ by the caller — worker threads publish into that map
// concurrently, so it must never be traversed here.
void resolve_product_ref(pkg *p,
                         pkg::weak_reference *wr,
                         engine::weak_resolution_result &result,
                         pkg *registry_provider,
                         engine &eng) {
  if (registry_provider) {
    pkg *dep{ registry_provider };

    if (!wr->constraint_identity.empty() &&
        dep->cfg->identity != wr->constraint_identity) {
      throw std::runtime_error("Product '" + wr->query + "' in spec '" + p->cfg->identity +
                               "' must come from '" + wr->constraint_identity +
                               "', but provider is '" + dep->cfg->identity + "'");
    }

    eng.wire_dependency(p, dep, wr->needed_by);
    merge_setup_selection(dep, wr->setup);
    record_resolution(p, wr, dep);
    trace_product_resolution(p, wr, dep, "registry");
    ++result.resolved;
    return;
  }

  if (wr->fallback) {
    wr->fallback->parent = p->cfg;

    pkg *dep{ eng.ensure_pkg(wr->fallback) };
    eng.wire_dependency(p, dep, wr->needed_by);
    merge_setup_selection(dep, wr->setup);
    eng.start_pkg_thread(dep, pkg_phase::spec_fetch);
    record_resolution(p, wr, dep);
    trace_product_resolution(p, wr, dep, "fallback");
    ++result.fallbacks_started;
  }
}

bool pkg_provides_product(pkg *root, std::string const &product_name) {
  std::vector<pkg *> work{ root };
  std::unordered_set<pkg const *> visited{ root };

  while (!work.empty()) {
    pkg *p{ work.back() };
    work.pop_back();

    std::lock_guard const deps_lock(p->deps_mutex);
    if (p->products.contains(product_name)) { return true; }
    for (auto const &[_, dep_info] : p->dependencies) {
      if (visited.insert(dep_info.p).second) { work.push_back(dep_info.p); }
    }
  }

  return false;
}

}  // namespace

void engine::wire_dependency(pkg *parent,
                             pkg *dep,
                             pkg_phase needed_by,
                             std::string_view kind) {
  {
    // mutex_ spans check and insert (order mutex_ → deps_mutex, never reversed):
    // two workers must not both find no cycle and then both add their edge.
    std::lock_guard const lock(mutex_);

    // Reachability, not the spawn path: an edge closes a cycle exactly when its
    // target already reaches the parent, however it got there.
    if (auto const path{ dependency_path(dep, parent) }; !path.empty()) {
      std::string msg{ std::string{ kind } + " cycle detected: " + parent->cfg->identity };
      for (pkg const *node : path) { msg += " -> " + node->cfg->identity; }
      throw std::runtime_error(msg);
    }

    std::lock_guard const deps_lock(parent->deps_mutex);
    auto const [it, inserted]{ parent->dependencies.try_emplace(
        dep->cfg->identity,
        pkg::dependency_info{ .p = dep, .needed_by = needed_by }) };

    // Neither this map nor the Lua API that reads it can express two option
    // variants of one identity, so the second one is an error rather than a
    // silent overwrite.
    if (!inserted && it->second.p != dep) {
      throw std::runtime_error("spec '" + parent->cfg->identity + "' depends on '" +
                               dep->cfg->identity + "' twice with different options: '" +
                               it->second.p->key.canonical() + "' and '" +
                               dep->key.canonical() + "'");
    }

    bool const tightened{ !inserted && needed_by < it->second.needed_by };
    if (tightened) { it->second.needed_by = needed_by; }
    if (inserted || tightened) {
      ENVY_TRACE(dependency_added,
                 parent->cfg->identity,
                 .dependency = dep->cfg->identity,
                 .needed_by = it->second.needed_by);
    }
  }

  propagate_closures(parent, dep);
}

engine::engine(cache &cache, manifest const *manifest)
    : cache_(cache),
      manifest_(manifest),
      default_shell_decl_(manifest ? manifest->get_default_shell() : default_shell_decl{}),
      core_(make_trace_observer()) {}

engine::~engine() = default;  // core_ (declared last) fails + joins workers first

std::string engine::trace_display(std::string const &key) const {
  if (is_setup_pair_key(key) || is_engine_task_key(key)) { return key; }
  std::lock_guard const lock(mutex_);
  auto const it{ packages_.find(pkg_key{ key }) };
  return it != packages_.end() ? it->second->cfg->identity : key;
}

task_engine::observer engine::make_trace_observer() {
  task_engine::observer obs;

  obs.blocked =
      [this](std::string const &key, int step, std::string const &dep, int watermark) {
        if (!tui::g_trace_enabled) { return; }
        bool const pair{ is_setup_pair_key(key) };
        ENVY_TRACE(
            phase_blocked,
            trace_display(key),
            .blocked_at_phase = pair ? pkg_phase::pkg_setup : static_cast<pkg_phase>(step),
            .waiting_for = trace_display(dep),
            .target_phase =
                pair ? pkg_phase::completion : phase_from_watermark(watermark));
      };
  obs.unblocked = [this](std::string const &key, int step, std::string const &dep) {
    if (!tui::g_trace_enabled) { return; }
    ENVY_TRACE(phase_unblocked,
               trace_display(key),
               .unblocked_at_phase = is_setup_pair_key(key) ? pkg_phase::pkg_setup
                                                            : static_cast<pkg_phase>(step),
               .dependency = trace_display(dep));
  };
  obs.target_extended = [this](std::string const &key, int old_done, int new_target) {
    if (!tui::g_trace_enabled) { return; }
    ENVY_TRACE(target_extended,
               trace_display(key),
               .old_target = phase_from_watermark(old_done),
               .new_target = phase_from_watermark(new_target));
  };

  return obs;
}

task_engine::task_config engine::make_pkg_task_config(pkg *p) {
  task_engine::task_config cfg;
  cfg.key = p->key.canonical();
  cfg.step_count = pkg_phase_count;

  cfg.on_start = [this, p] {
    // Fetch/source dependencies must be wired before step 0's edge query so
    // spec loading blocks on the bundles it needs.
    if (!p->cfg->source_dependencies.empty()) { process_fetch_dependencies(p); }
  };

  cfg.edges = [p](int step) {
    // Snapshot under deps_mutex — the resolution loop may wire weak deps into
    // this map concurrently. Re-waiting satisfied edges on later steps is cheap.
    std::vector<task_engine::edge> edges;
    std::lock_guard const deps_lock(p->deps_mutex);
    for (auto const &[dep_identity, dep_info] : p->dependencies) {
      if (step >= static_cast<int>(dep_info.needed_by)) {
        edges.push_back({ dep_info.p->key.canonical(),
                          kDependencySatisfiedWatermark,
                          kDependencyRunThroughWatermark });
      }
    }
    return edges;
  };

  cfg.step = [this, p](int step) {
    // Ambient log context for this whole step: every debug/info/warn/error line
    // (including those emitted from cache/extract called by phase code) is
    // auto-prefixed "[identity]". Deleted hand-rolled [%s] prefixes rely on this.
    tui::log_ctx_scope const log_ctx{ p->cfg->identity };
    p->current_phase.store(static_cast<pkg_phase>(step));

    if (static_cast<pkg_phase>(step) == pkg_phase::spec_fetch) {
      p->build_start = std::chrono::steady_clock::now();
    }

    phase_dispatch_table[step](p, *this);

    if (static_cast<pkg_phase>(step) == pkg_phase::spec_fetch) {
      // Publish before the completion flag: a consumer's edge waits on
      // pkg_export, which is strictly later, so the entry is always visible by
      // the time anyone can legally read this package's products.
      register_products(p);
      p->spec_fetch_completed = true;
      on_spec_fetch_complete();

      // BUNDLE_ONLY packages stop after spec_fetch — no lua state to execute — but
      // they still report through the completion phase, so a bundle gets the same
      // outcome row, trace event, and off-TTY line as any other package.
      if (p->type == pkg_type::BUNDLE_ONLY) {
        run_completion_phase(p, *this);
        return true;
      }
    }
    return false;
  };

  cfg.on_failed = [this, p] {
    if (!p->spec_fetch_completed) { on_spec_fetch_complete(); }
  };

  return cfg;
}

std::unique_ptr<pkg> engine::make_pkg(pkg_cfg const *cfg,
                                      tui::section_handle section,
                                      std::string canonical_identity_hash) {
  return std::unique_ptr<pkg>(
      new pkg{ .key = pkg_key{ *cfg },
               .cfg = cfg,
               .cache_ptr = &cache_,
               .eng = this,
               .tui_section = section,
               .lua = nullptr,
               .lock = nullptr,
               .canonical_identity_hash = std::move(canonical_identity_hash),
               .pkg_path = std::filesystem::path{},
               .result_hash = {},
               .type = pkg_type::UNKNOWN,
               .owned_dependency_cfgs = {},
               .dependencies = {},
               .product_dependencies = {},
               .weak_references = {} });
}

pkg *engine::ensure_pkg(pkg_cfg const *cfg) {
  pkg_key const key(*cfg);
  pkg *result{ nullptr };
  bool inserted{ false };

  {
    std::lock_guard const lock(mutex_);

    // Look before constructing: a duplicate call would otherwise build a whole
    // package and a TUI row only to throw them away.
    auto it{ packages_.find(key) };
    if (it == packages_.end()) {
      it = packages_.emplace(key, make_pkg(cfg, tui::section_create(), key.canonical()))
               .first;
      inserted = true;
    }
    result = it->second.get();

    if (inserted) { ENVY_TRACE(spec_registered, cfg->identity, .key = key.canonical()); }

    // A second cfg for an existing key: the source is not part of the key, so make
    // sure the two declarations agree about what to fetch.
    if (!inserted && result->cfg != cfg) {
      validate_source_redeclaration(result->cfg, cfg);
    }

    if (cfg->setup.has_value() && !cfg->setup->empty()) {
      // Merge explicit SETUP selection across referrers (union). Selection is
      // explicit-only: a referrer that omits `setup` requests nothing.
      std::lock_guard const deps_lock(result->deps_mutex);
      size_t const before{ result->setup_selected.size() };
      result->setup_selected.insert(cfg->setup->begin(), cfg->setup->end());
      if (result->setup_selection_consumed && result->setup_selected.size() != before) {
        throw std::runtime_error(
            "SETUP selection for " + cfg->identity +
            " arrived after its setup phase ran; select pairs from entries that "
            "resolve before the package executes");
      }
    }

    // Task creation must be atomic with the packages_ insert (still under
    // mutex_; engine mutex -> core mutex ordering, never reversed): a second
    // thread that loses the try_emplace returns immediately and may start or
    // wait on the task before this thread would otherwise have created it.
    if (inserted && !core_.ensure_task(make_pkg_task_config(result))) {
      // A fresh package key must never collide with an existing task (e.g. a
      // pathological identity matching a pair-task key). Fail loudly now
      // instead of hanging later on a task with someone else's config.
      throw std::runtime_error("Package task key collides with existing task: " +
                               key.canonical());
    }
  }

  return result;
}

pkg *engine::find_exact(pkg_key const &key) const {
  std::lock_guard const lock(mutex_);
  auto const it{ packages_.find(key) };
  return (it != packages_.end()) ? it->second.get() : nullptr;
}

pkg *engine::find_product_provider(std::string const &product_name) const {
  std::lock_guard const lock(mutex_);
  auto const it{ product_registry_.find(product_name) };
  return it == product_registry_.end() ? nullptr : it->second;
}

std::vector<product_info> engine::collect_all_products() const {
  std::vector<product_info> infos;

  {
    std::lock_guard const lock(mutex_);

    for (auto const &[key, package] : packages_) {
      // type is written during spec_fetch and pkg_path across check/import/install:
      // read each only once its own worker has published the step that writes it
      // (the atomic completed count is the happens-before), or a still-running
      // closure member is a data race.
      int const done{ core_.completed(key.canonical()) };
      pkg_type const type{ done >= 1 ? package->type : pkg_type::UNKNOWN };
      auto const pkg_path{ done >= kDependencySatisfiedWatermark
                               ? package->pkg_path
                               : std::filesystem::path{} };

      // products/resolved_platforms are deps_mutex-guarded; mutex_ → deps_mutex
      // matches the resolution loop's order.
      std::lock_guard const deps_lock(package->deps_mutex);
      for (auto const &[prod_name, prod_entry] : package->products) {
        auto plats{ util_platform_intersect(prod_entry.platforms,
                                            package->resolved_platforms) };
        if (plats.empty() && !prod_entry.platforms.empty() &&
            !package->resolved_platforms.empty()) {
          plats.emplace_back(kPlatformNone);
        }
        infos.push_back({
            .product_name = prod_name,
            .value = prod_entry.value,
            .provider_canonical = package->key.canonical(),
            .type = type,
            .pkg_path = pkg_path,
            .script = prod_entry.script,
            .platforms = std::move(plats),
        });
      }
    }
  }

  std::ranges::sort(infos, {}, &product_info::product_name);

  return infos;
}

std::vector<pkg *> engine::find_matches(std::string_view query) const {
  std::lock_guard const lock(mutex_);

  std::vector<pkg *> matches;
  for (auto const &[key, p] : packages_) {
    if (key.matches(query)) { matches.push_back(p.get()); }
  }

  return matches;
}

void engine::start_pkg_thread(pkg *p, pkg_phase run_through) {
  // before_spawn runs exactly once, before the worker exists: the spec-fetch
  // counter must rise before the worker can decrement it.
  core_.start_task(p->key.canonical(), watermark_through(run_through), [this] {
    on_spec_fetch_start();
  });
}

void engine::extend_to_completion(pkg_key const &key) {
  core_.extend_to_done(key.canonical());
}

void engine::wait_for_completion(pkg_key const &key) {
  std::string const canonical{ key.canonical() };
  core_.wait_at(canonical, core_.step_count(canonical));
}

void engine::run_setup_pairs_for(pkg *parent, std::vector<std::string> const &pair_names) {
  // Pair name → task key; the selection closure guarantees every DEPENDS
  // target of a selected pair is itself selected.
  std::unordered_map<std::string, std::string> key_of;
  for (auto const &name : pair_names) {
    key_of.emplace(name, parent->key.canonical() + "#setup:" + name);
  }

  for (auto const &name : pair_names) {
    std::string const &key{ key_of.at(name) };

    std::vector<task_engine::edge> sibling_edges;
    for (auto const &dep : parent->setup_pairs.at(name).depends) {
      sibling_edges.push_back({ key_of.at(dep), 1 });
    }

    tui::section_handle const section{ tui::section_create() };

    task_engine::task_config cfg;
    cfg.key = key;
    cfg.step_count = 1;
    cfg.edges = [edges = std::move(sibling_edges)](int) { return edges; };
    cfg.step = [this, parent, name, section, key](int) {
      tui::log_ctx_scope const log_ctx{ parent->cfg->identity };
      run_setup_pair(parent, *this, name, section, key);
      if (section && tui::section_has_content(section)) {
        tui::section_set_content(
            section,
            tui::section_frame{ .label = "[" + key + "]",
                                .content = tui::static_text_data{ .text = "done" } });
        tui::section_set_complete(section);
      }
      return false;
    };

    if (!core_.ensure_task(std::move(cfg))) {
      throw std::runtime_error("SETUP pair task key collides with existing task: " + key);
    }
  }

  // Edges are baked into each config, so start order is irrelevant.
  for (auto const &name : pair_names) { core_.start_task(key_of.at(name), 1); }

  // Wait for every pair so one bad pair doesn't mask the others, but re-throw only the
  // first verbatim: joining them would store a third string nothing can dedup against.
  std::string first_error;
  for (auto const &name : pair_names) {
    try {
      core_.wait_at(key_of.at(name), 1);
    } catch (std::exception const &e) {
      if (first_error.empty()) { first_error = e.what(); }
    }
  }
  if (!first_error.empty()) { throw std::runtime_error(first_error); }
}

void engine::extend_dependencies_to_completion(pkg *p) {
  std::unordered_set<pkg_key> visited;
  extend_dependencies_recursive(p, visited);
}

std::filesystem::path const &engine::cache_root() const { return cache_.root(); }

manifest const *engine::get_manifest() const { return manifest_; }

void engine::set_depot_index(package_depot_index idx) {
  depot_index_ = std::move(idx);
  depot_pre_set_ = true;
}

void engine::set_ignore_depot(bool ignore) { depot_ignored_ = ignore; }

void engine::set_export_config(export_phase_config cfg) {
  export_config_ = std::move(cfg);
}

export_phase_config const *engine::export_config() const {
  return export_config_ ? &*export_config_ : nullptr;
}

void engine::record_export_result(pkg_key const &key, std::string output_line) {
  std::lock_guard const lock{ mutex_ };
  export_results_[key.canonical()] = std::move(output_line);
}

std::string const *engine::get_export_result(pkg_key const &key) const {
  std::lock_guard const lock{ mutex_ };
  auto it{ export_results_.find(key.canonical()) };
  return it != export_results_.end() ? &it->second : nullptr;
}

package_depot_index const *engine::depot_index_for(pkg *p) {
  // Pre-set index (set before thread creation, safe to read without synchronization)
  if (depot_pre_set_) { return &*depot_index_; }

  if (depot_ignored_) { return nullptr; }

  if (!manifest_ || manifest_->package_depots.empty()) { return nullptr; }

  // Bootstrap: never consult the depot
  if (p->in_closure(pkg_closure::depot_bootstrap)) { return nullptr; }

  ensure_depot_task_started();

  auto const wait_start{ std::chrono::steady_clock::now() };

  // The #depot worker broadcasts the global condition on completion/failure;
  // mark_closure broadcasts when this package's exemption flips late
  // (it was wired into the depot's DEPENDS closure after blocking here).
  core_.wait_global([this, p] {
    return depot_state_ != task_state::NOT_READY ||
           p->in_closure(pkg_closure::depot_bootstrap);
  });

  bool const bootstrap{ p->in_closure(pkg_closure::depot_bootstrap) };
  bool const failed{ !bootstrap && depot_state_ == task_state::FAILED };
  ENVY_TRACE(depot_wait,
             p->cfg->identity,
             .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - wait_start)
                                .count(),
             .result = bootstrap ? "bootstrap" : (failed ? "failed" : "ready"));

  if (bootstrap) { return nullptr; }
  if (failed) { throw std::runtime_error(depot_error_); }
  return depot_index_ ? &*depot_index_ : nullptr;
}

void engine::ensure_depot_task_started() {
  std::call_once(depot_task_once_, [this] {
    task_engine::task_config cfg;
    cfg.key = kDepotTaskKey;
    cfg.step_count = 1;
    cfg.on_start = [this] {
      try {
        depot_edge_deps_ = spawn_depot_dependencies();
      } catch (std::exception const &e) {
        depot_error_ = std::string{ "package depot: " } + e.what();
        throw std::runtime_error(depot_error_);  // task and waiters store one message
      }
    };
    cfg.edges = [this](int) {
      std::vector<task_engine::edge> edges;
      edges.reserve(depot_edge_deps_.size());
      for (pkg *dep : depot_edge_deps_) {
        edges.push_back({ dep->key.canonical(),
                          kDependencySatisfiedWatermark,
                          kDependencyRunThroughWatermark });
      }
      return edges;
    };
    cfg.step = [this](int) {
      try {
        run_depot_step();
      } catch (std::exception const &e) {
        // Throw what the waiters get: a bare rethrow would store the unprefixed
        // message here and defeat aggregated_failures' dedup.
        depot_error_ = std::string{ "package depot: " } + e.what();
        throw std::runtime_error(depot_error_);
      }
      return false;
    };
    cfg.on_failed = [this] {
      if (depot_error_.empty()) { depot_error_ = "package depot: dependency failed"; }
      depot_state_ = task_state::FAILED;
    };

    if (!core_.ensure_task(std::move(cfg))) {
      throw std::runtime_error("depot task key collides with existing task");
    }
    core_.start_task(kDepotTaskKey, 1);
  });
}

std::vector<pkg *> engine::spawn_depot_dependencies() {
  std::vector<pkg *> edge_deps;
  std::unordered_set<pkg *> seen;

  for (auto const &src : manifest_->package_depots) {
    auto const *fn{ std::get_if<manifest::depot_fetch_fn>(&src) };
    if (!fn) { continue; }

    std::vector<pkg *> fn_deps;
    if (!fn->depends.empty()) {
      auto const cfgs{
        engine_resolve_targets(manifest_->packages, fn->depends, "package-depot")
      };
      fn_deps.reserve(cfgs.size());
      for (auto const *cfg : cfgs) {
        pkg *dep{ ensure_pkg(cfg) };
        mark_closure(dep, pkg_closure::depot_bootstrap);
        fn_deps.push_back(dep);
        if (seen.insert(dep).second) {
          edge_deps.push_back(dep);
          // Run to full completion: depot deps may spawn after the resolution
          // loop, so nothing else ratchets them.
          start_pkg_thread(dep, pkg_phase::completion);
        }
      }
    }
    depot_fn_deps_.emplace_back(fn->lua_index, std::move(fn_deps));
  }

  return edge_deps;
}

void engine::run_depot_step() {
  namespace fs = std::filesystem;

  auto const depot_tmp{ fs::temp_directory_path() /
                        ("envy-depot-" + std::to_string(platform::get_process_id())) };
  std::error_code ec;
  fs::create_directories(depot_tmp, ec);

  try {
    package_depot_index merged;

    std::vector<std::string> urls;
    for (auto const &src : manifest_->package_depots) {
      if (auto const *uri{ std::get_if<manifest::depot_uri>(&src) }) {
        urls.push_back(uri->url);
      }
    }
    merged.merge(package_depot_index::build(urls, depot_tmp));

    for (auto const &[lua_index, deps] : depot_fn_deps_) {
      std::vector<std::pair<std::string, std::string>> dep_paths;
      dep_paths.reserve(deps.size());
      for (pkg *dep : deps) {
        dep_paths.emplace_back(dep->cfg->identity, dep->pkg_path.string());
      }

      phase_context ctx{ .eng = this,
                         .p = nullptr,
                         .run_dir = depot_tmp,
                         .lock = nullptr,
                         .builtin_shell = true };
      auto const result{
        manifest_->run_depot_fetch(lua_index, &ctx, depot_tmp, dep_paths)
      };

      if (auto const *text{ std::get_if<std::string>(&result) }) {
        // A newline-free string naming an existing file is a path to depot
        // manifest text; anything else is the text itself. FETCH output is
        // author-trusted, so SHA256 is not required (local paths importable).
        std::string content{ *text };
        if (text->find('\n') == std::string::npos && fs::exists(fs::path{ *text }, ec)) {
          auto const data{ util_load_file(*text) };
          content.assign(reinterpret_cast<char const *>(data.data()), data.size());
        }
        merged.merge(package_depot_index::build_from_text(content, false));
      } else {
        merged.merge(package_depot_index::build_from_entries(
            std::get<std::vector<depot_entry>>(result)));
      }
    }

    depot_index_ = std::move(merged);
    depot_state_ = task_state::READY;
  } catch (...) {
    fs::remove_all(depot_tmp, ec);
    throw;
  }
  fs::remove_all(depot_tmp, ec);
}

void engine::mark_closure(pkg *p, pkg_closure kind) {
  walk_closure(p, kind, [this, kind](pkg *member) {
    // Checked per member here as well as at declaration time, because a package can
    // complete spec_fetch as a root — registering its weak references and passing that
    // check — before anything pulls it into a closure. Only *unresolved* references
    // are a violation; one already resolved at an earlier barrier is wired and
    // ordered.
    auto const unresolved{ [member] {
      std::lock_guard const deps_lock(member->deps_mutex);
      auto const it{ std::ranges::find_if(member->weak_references,
                                          [](auto const &wr) { return !wr.resolved; }) };
      return it == member->weak_references.end() ? std::string{} : it->query;
    }() };
    if (!unresolved.empty()) {
      throw std::runtime_error(std::string{ pkg_closure_name(kind) } +
                               " must use strong dependencies: '" + member->cfg->identity +
                               "' holds a weak reference to '" + unresolved +
                               "' but runs outside the window where weak references "
                               "resolve");
    }

    // The bit is what releases a member already parked in a bootstrap wait, so
    // every kind wakes them; kAllClosures grows without another special case here.
    core_.notify_global();
  });
}

void engine::propagate_closures(pkg *from, pkg *to) {
  for (auto const kind : kAllClosures) {
    if (from->in_closure(kind)) { mark_closure(to, kind); }
  }
}

bundle *engine::register_bundle(std::string const &identity,
                                std::unordered_map<std::string, std::string> specs,
                                std::filesystem::path cache_path) {
  std::lock_guard const lock{ mutex_ };

  // Check if already registered
  auto it{ bundle_registry_.find(identity) };
  if (it != bundle_registry_.end()) { return it->second.get(); }

  // Create new bundle and register
  auto b{ std::make_unique<bundle>() };
  b->identity = identity;
  b->specs = std::move(specs);
  b->cache_path = std::move(cache_path);

  auto [insert_it, inserted]{ bundle_registry_.emplace(identity, std::move(b)) };
  return insert_it->second.get();
}

bundle *engine::find_bundle(std::string const &identity) const {
  std::lock_guard const lock{ mutex_ };
  auto it{ bundle_registry_.find(identity) };
  return it != bundle_registry_.end() ? it->second.get() : nullptr;
}

void engine::extend_dependencies_recursive(pkg *p, std::unordered_set<pkg_key> &visited) {
  if (!visited.insert(p->key).second) { return; }  // Already visited (cycle detection)

  // The observer traces the extension; doing it here too would double-report it.
  core_.extend_to_done(p->key.canonical());

  // Recursively extend all dependencies (snapshot: no nested pkg locks)
  auto const deps{ [&] {
    std::lock_guard const deps_lock(p->deps_mutex);
    std::vector<pkg *> snapshot;
    snapshot.reserve(p->dependencies.size());
    for (auto const &[_, dep_info] : p->dependencies) { snapshot.push_back(dep_info.p); }
    return snapshot;
  }() };
  for (auto *dep : deps) { extend_dependencies_recursive(dep, visited); }
}

#ifdef ENVY_UNIT_TEST
pkg_phase engine::get_pkg_target_phase(pkg_key const &key) const {
  return phase_from_watermark(core_.target(key.canonical()));
}
#endif

void engine::wait_for_resolution_phase() {
  core_.wait_global([this] { return pending_spec_fetches_ == 0; });
}

void engine::on_spec_fetch_start() { pending_spec_fetches_.fetch_add(1); }

void engine::on_spec_fetch_complete() {
  if (pending_spec_fetches_.fetch_sub(1) - 1 == 0) { core_.notify_global(); }
}

bool engine::builtin_shell_only(pkg const *p) const {
  if (!p) { return true; }
  for (auto const kind : kAllClosures) {
    if (p->in_closure(kind)) { return true; }
  }
  return false;
}

resolved_shell engine::default_shell(pkg *p) {
  if (!default_shell_decl_.is_function) {
    return shell_resolve_default(&default_shell_decl_.value);
  }

  // Bootstrap work runs before the manifest shell can exist — every closure member
  // is either supplying that shell or running outside the window where it could —
  // and the manifest Lua state is one non-recursive mutex, so evaluating the shell
  // can never nest inside itself either (phase_context::builtin_shell covers that).
  if (builtin_shell_only(p)) { return shell_resolve_default(nullptr); }

  ensure_default_shell_task_started();

  // The #default_shell worker broadcasts on publish/failure; mark_closure broadcasts
  // when this package's exemption flips late (it was wired into a bootstrap closure
  // after blocking here). Atomic reads only: the watchdog evaluates this too.
  core_.wait_global([this, p] {
    return default_shell_state_.load() != task_state::NOT_READY || builtin_shell_only(p);
  });

  if (builtin_shell_only(p)) { return shell_resolve_default(nullptr); }
  if (default_shell_state_ == task_state::FAILED) {
    throw std::runtime_error(default_shell_error_);
  }
  return shell_resolve_default(&default_shell_);
}

void engine::intern_default_shell_deps() {
  if (!default_shell_decl_.is_function || default_shell_decl_.depends.empty()) { return; }
  if (!default_shell_deps_.empty()) { return; }  // the identities never change

  // DEPENDS names packages the manifest already declares, as PACKAGE_DEPOTS does.
  auto const cfgs{ engine_resolve_targets(manifest_->packages,
                                          default_shell_decl_.depends,
                                          "DEFAULT_SHELL") };

  std::vector<pkg *> deps;
  deps.reserve(cfgs.size());
  for (auto const *cfg : cfgs) {
    pkg *dep{ ensure_pkg(cfg) };
    mark_closure(dep, pkg_closure::default_shell);
    deps.push_back(dep);
  }
  default_shell_deps_ = std::move(deps);
}

void engine::ensure_default_shell_task_started() {
  std::call_once(default_shell_task_once_, [this] {
    task_engine::task_config cfg;
    cfg.key = kDefaultShellTaskKey;
    cfg.step_count = 1;
    // Started here, not in resolve_graph, so a run that never asks for a shell never
    // builds the interpreter; to completion, since nothing else ratchets these.
    cfg.on_start = [this] {
      try {
        intern_default_shell_deps();  // no-op after resolve_graph, which does it first
        ENVY_TRACE(default_shell_resolving,
                   kDefaultShellConsumerIdentity,
                   .depends = static_cast<std::int64_t>(default_shell_deps_.size()));
        for (pkg *dep : default_shell_deps_) {
          start_pkg_thread(dep, pkg_phase::completion);
        }
      } catch (std::exception const &e) {
        default_shell_error_ = std::string{ "DEFAULT_SHELL: " } + e.what();
        throw std::runtime_error(default_shell_error_);  // one message, task and waiters
      }
    };
    cfg.edges = [this](int) {
      std::vector<task_engine::edge> edges;
      edges.reserve(default_shell_deps_.size());
      for (pkg *dep : default_shell_deps_) {
        edges.push_back({ dep->key.canonical(),
                          kDependencySatisfiedWatermark,
                          kDependencyRunThroughWatermark });
      }
      return edges;
    };
    cfg.step = [this](int) {
      try {
        run_default_shell_step();
      } catch (std::exception const &e) {
        // Throw what the waiters get, so task and waiter store one string (see #depot).
        default_shell_error_ = std::string{ "DEFAULT_SHELL: " } + e.what();
        throw std::runtime_error(default_shell_error_);
      }
      return false;
    };
    cfg.on_failed = [this] {
      if (default_shell_error_.empty()) {
        default_shell_error_ = "DEFAULT_SHELL: dependency failed";
      }
      default_shell_state_ = task_state::FAILED;
    };

    if (!core_.ensure_task(std::move(cfg))) {
      throw std::runtime_error("DEFAULT_SHELL task key collides with existing task");
    }
    core_.start_task(kDefaultShellTaskKey, 1);
  });
}

void engine::run_default_shell_step() {
  // A manifest-wide shell has no package of its own to authorize against, so it gets
  // one: a consumer holding an edge to each DEPENDS entry, wired through the same
  // path as everyone else's. It is deliberately absent from packages_ — never
  // scheduled, matched, or reported — and sits at completion because this task's own
  // edges have already carried every DEPENDS entry through setup.
  pkg_cfg const *const consumer_cfg{ pkg_cfg::pool()->emplace(
      kDefaultShellConsumerIdentity,
      pkg_cfg::source_t{ pkg_cfg::weak_ref{} },
      std::string{},
      std::optional<pkg_phase>{},
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{},
      std::optional<std::string>{},
      manifest_->manifest_path) };

  default_shell_consumer_ = make_pkg(consumer_cfg, tui::kInvalidSection, {});
  default_shell_consumer_->current_phase = pkg_phase::completion;

  for (pkg *dep : default_shell_deps_) {
    wire_dependency(default_shell_consumer_.get(), dep, pkg_phase::spec_fetch);
  }

  // The manifest's directory, not the CWD: DEFAULT_SHELL belongs to the project, and a
  // command anchored elsewhere (--project, --manifest) would otherwise run it in the
  // caller's tree -- the same cwd SETUP verbs get from compute_project_root.
  phase_context ctx{ .eng = this,
                     .p = default_shell_consumer_.get(),
                     .run_dir = manifest_->manifest_path.parent_path(),
                     .lock = nullptr,
                     .builtin_shell = true };
  default_shell_ = manifest_->run_default_shell_fn(&ctx);
  default_shell_state_ = task_state::READY;

  ENVY_TRACE(
      default_shell_resolved,
      kDefaultShellConsumerIdentity,
      .shell = std::string{ shell_kind_name(shell_resolve_default(&default_shell_)) });
}

resolved_shell pkg_default_shell(pkg *p) {
  return p && p->eng ? p->eng->default_shell(p) : shell_resolve_default(nullptr);
}

void engine::process_fetch_dependencies(pkg *p) {
  // Process fetch dependencies - added to dependencies map with needed_by=spec_fetch
  // The per-step edge query handles blocking automatically
  for (auto *fetch_dep_cfg : p->cfg->source_dependencies) {
    // A bundle package's parent is fixed where the bundle is declared: the declaring
    // spec, whose Lua state holds the custom fetch function, or null for a
    // manifest-declared bundle, whose fetch function lives in the manifest. Never
    // re-parent it to a consumer — every spec pulled from the bundle is blocked on it
    // here, so its Lua state is by definition not loaded yet, and the bundle's own
    // worker may be reading parent concurrently.
    bool const is_bundle_pkg{ fetch_dep_cfg->bundle_identity.has_value() &&
                              fetch_dep_cfg->identity == *fetch_dep_cfg->bundle_identity };
    if (!is_bundle_pkg) {
      fetch_dep_cfg->parent = p->cfg;  // Set parent pointer for custom fetch lookup
    }

    // pkg_cfg::parse_fetch_dependency rejects weak fetch prerequisites, so every
    // entry here has a source. Programmatic construction bypasses that parse, so
    // assert it rather than fall through to ensure_pkg with no source.
    if (fetch_dep_cfg->is_weak_reference()) {
      throw std::runtime_error("source.dependencies entry '" + fetch_dep_cfg->identity +
                               "' in spec '" + p->cfg->identity +
                               "' must be a strong reference");
    }

    pkg *fetch_dep{ ensure_pkg(fetch_dep_cfg) };

    // This package runs its whole ladder during graph resolution, so nothing in its
    // closure may hold a weak reference. Mark before starting its worker, so its own
    // wire_dependency_graph sees the flag.
    mark_closure(fetch_dep, pkg_closure::fetch);

    // The edge query will block spec_fetch on it.
    wire_dependency(p, fetch_dep, pkg_phase::spec_fetch, "Fetch dependency");

    // An explicit `product` on the entry additionally pins the name to this
    // provider, so envy.product reports a mismatch instead of silently reading
    // whichever package the registry happens to hold.
    if (fetch_dep_cfg->product.has_value()) {
      std::lock_guard const deps_lock(p->deps_mutex);
      auto const [it, inserted]{ p->product_dependencies.emplace(
          *fetch_dep_cfg->product,
          pkg::product_dependency{ .name = *fetch_dep_cfg->product,
                                   .needed_by = pkg_phase::spec_fetch,
                                   .provider = fetch_dep,
                                   .constraint_identity = fetch_dep_cfg->identity }) };
      if (!inserted && it->second.provider != fetch_dep) {
        throw std::runtime_error("Duplicate product dependency '" +
                                 *fetch_dep_cfg->product + "' in spec '" +
                                 p->cfg->identity + "'");
      }
    }

    start_pkg_thread(fetch_dep, pkg_phase::completion);
  }
}

std::vector<pkg_cfg const *> engine_filter_host_platform(
    std::vector<pkg_cfg const *> const &cfgs) {
  std::vector<pkg_cfg const *> result;
  result.reserve(cfgs.size());
  for (auto const *cfg : cfgs) {
    if (util_platform_matches(cfg->platforms,
                              platform::os_name(),
                              platform::arch_name())) {
      result.push_back(cfg);
    }
  }
  return result;
}

std::vector<pkg_cfg const *> engine_resolve_targets(
    std::vector<pkg_cfg *> const &packages,
    std::vector<std::string> const &queries,
    std::string const &cmd_name) {
  if (queries.empty()) { return { packages.begin(), packages.end() }; }

  std::vector<pkg_cfg const *> targets;
  for (auto const &query : queries) {
    // Distinct packages, not entries: envy.import can splice two declarations of one
    // key, which ensure_pkg treats as one package. Sorted so the message is reproducible.
    std::map<std::string, pkg_cfg const *> matches;
    for (auto const *pkg : packages) {
      if (pkg_key const key{ *pkg }; key.matches(query)) {
        matches.try_emplace(key.canonical(), pkg);
      }
    }

    if (matches.empty()) {
      throw std::runtime_error(cmd_name + ": query '" + query + "' not found in manifest");
    }

    // First-wins would pick whichever entry the manifest happened to list first;
    // make the author say which one.
    if (matches.size() > 1) {
      std::ostringstream oss;
      oss << cmd_name << ": query '" << query << "' is ambiguous: ";
      for (auto it{ matches.begin() }; it != matches.end(); ++it) {
        if (it != matches.begin()) { oss << ", "; }
        oss << it->first;
      }
      throw std::runtime_error(oss.str());
    }

    pkg_cfg const *const target{ matches.begin()->second };
    if (!util_platform_matches(target->platforms,
                               platform::os_name(),
                               platform::arch_name())) {
      throw std::runtime_error(cmd_name + ": '" + query +
                               "' is not available on this platform");
    }
    targets.push_back(target);
  }

  return targets;
}

std::string engine::aggregated_failures() const {
  std::vector<std::string> msgs;
  for (auto const &[key, msg] : core_.collect_failures()) {
    msgs.push_back(msg.empty() ? "Package failed: " + key : msg);
  }
  std::ranges::sort(msgs);
  msgs.erase(std::ranges::unique(msgs).begin(), msgs.end());

  std::string out;
  for (auto const &msg : msgs) {
    if (!out.empty()) { out += '\n'; }
    out += msg;
  }
  return out;
}

void engine::run_full(std::vector<pkg_cfg const *> const &roots) {
  auto const filtered{ engine_filter_host_platform(roots) };

  try {
    resolve_graph(filtered);
  } catch (...) {
    core_.fail_all();
    core_.join_all();  // Best-effort join to avoid leaks
    throw;
  }

  core_.extend_all_to_done();  // Launch all tasks running to completion
  core_.join_all();            // Tolerates pair tasks spawned while joining

  if (auto const failures{ aggregated_failures() }; !failures.empty()) {
    throw std::runtime_error(failures);
  }
}

void engine::register_products(pkg *p) {
  // Snapshot names under deps_mutex, publish under mutex_ — sequential, never
  // nested, so this never inverts the resolution loop's mutex_ → deps_mutex
  // order. Sorted so a package declaring several colliding products always
  // reports the same one first.
  auto const names{ [p] {
    std::vector<std::string> n;
    std::lock_guard const deps_lock(p->deps_mutex);
    n.reserve(p->products.size());
    for (auto const &[product_name, _] : p->products) { n.push_back(product_name); }
    std::ranges::sort(n);
    return n;
  }() };

  if (names.empty()) { return; }

  std::lock_guard const lock(mutex_);
  for (auto const &product_name : names) {
    auto const [it, inserted]{ product_registry_.emplace(product_name, p) };
    if (inserted || it->second == p) { continue; }

    // Whichever provider registered first is scheduling-dependent; name them in
    // sorted order so the message is reproducible.
    auto const &a{ it->second->cfg->identity };
    auto const &b{ p->cfg->identity };
    throw std::runtime_error(
        "Product '" + product_name +
        "' provided by multiple specs: " + (a < b ? a + ", " + b : b + ", " + a));
  }
}

void engine::validate_product_fallbacks() {
  std::vector<std::pair<pkg *, pkg::weak_reference *>> to_validate;

  {
    std::lock_guard const lock(mutex_);
    for (auto &[_, package] : packages_) {
      // weak_references is deps_mutex-guarded; hold it while iterating, like the
      // sibling validators do.
      std::lock_guard const deps_lock(package->deps_mutex);
      for (auto &wr : package->weak_references) {
        if (wr.is_product && wr.fallback && wr.resolved) {
          to_validate.emplace_back(package.get(), &wr);
        }
      }
    }
  }

  std::vector<std::string> errors;

  for (auto const &[p, wr] : to_validate) {
    if (!pkg_provides_product(wr->resolved, wr->query)) {
      errors.push_back("Fallback for product '" + wr->query + "' in spec '" +
                       p->cfg->identity + "' resolved to '" + wr->resolved->cfg->identity +
                       "', which does not provide product transitively");
    }
  }

  if (!errors.empty()) {
    std::ostringstream oss;
    for (size_t i{ 0 }; i < errors.size(); ++i) {
      if (i) { oss << "\n"; }
      oss << errors[i];
    }
    throw std::runtime_error(oss.str());
  }
}

void engine::validate_setup_selections() {
  // A weak reference may select SETUP pairs on whatever package it resolves to.
  // A selection only makes sense if the resolved package runs a setup phase and
  // declares the named pair. Validate once, after the graph is fully resolved
  // (all spec_fetches complete, so type/setup_pairs are populated), so the
  // author sees a precise error before any fetch/build work begins.
  std::vector<std::pair<pkg *, pkg::weak_reference *>> to_validate;
  {
    std::lock_guard const lock(mutex_);
    for (auto &[_, package] : packages_) {
      // weak_references is deps_mutex-guarded; take it while iterating to match
      // resolve_weak_references (no mutators run at this point, but keep the
      // discipline consistent).
      std::lock_guard const deps_lock(package->deps_mutex);
      for (auto &wr : package->weak_references) {
        if (!wr.setup.empty() && wr.resolved) {
          to_validate.emplace_back(package.get(), &wr);
        }
      }
    }
  }

  std::vector<std::string> errors;

  for (auto const &[requester, wr] : to_validate) {
    pkg *const target{ wr->resolved };
    std::string const context{ "spec '" + requester->cfg->identity +
                               "' weak-depends on '" + wr->query + "' (resolved to '" +
                               target->cfg->identity + "')" };

    // (a) target must actually run a setup phase (see run_setup_phase).
    if (target->type != pkg_type::CACHE_MANAGED &&
        target->type != pkg_type::USER_MANAGED) {
      for (auto const &name : wr->setup) {
        errors.push_back(context + " selects SETUP pair '" + name +
                         "', but that package runs no setup phase");
      }
      continue;
    }

    // (b) every selected pair must be declared by the resolved package.
    for (auto const &name : wr->setup) {
      if (target->setup_pairs.contains(name)) { continue; }
      std::string detail{ target->setup_pairs.empty() ? "it declares no SETUP pairs"
                                                      : "declared pairs:" };
      for (auto const &[pair_name, _] : target->setup_pairs) { detail += " " + pair_name; }
      errors.push_back(context + " selects SETUP pair '" + name + "', but " + detail);
    }
  }

  if (!errors.empty()) {
    std::ostringstream oss;
    for (size_t i{ 0 }; i < errors.size(); ++i) {
      if (i) { oss << "\n"; }
      oss << errors[i];
    }
    throw std::runtime_error(oss.str());
  }
}

engine::weak_resolution_result engine::resolve_weak_references() {
  weak_resolution_result result{};

  // One scan: anything spawned by this pass belongs to the next wave anyway, and
  // resolve_graph only consults the counts below when this pass changed nothing.
  auto const unresolved{ [this] {
    std::vector<std::pair<pkg *, pkg::weak_reference *>> out;
    std::lock_guard const lock(mutex_);
    for (auto &[key, package] : packages_) {
      std::lock_guard const deps_lock(package->deps_mutex);
      for (auto &wr : package->weak_references) {
        if (!wr.resolved) { out.emplace_back(package.get(), &wr); }
      }
    }
    return out;
  }() };

  std::vector<std::string> ambiguity_messages;

  for (auto [p, wr] : unresolved) {
    if (wr->is_product) {
      resolve_product_ref(p, wr, result, find_product_provider(wr->query), *this);
    } else {
      resolve_identity_ref(p, wr, result, ambiguity_messages, *this);
    }
  }

  // Wait for the fallbacks' spec_fetch, then re-check. The snapshot's raw pointers hold:
  // only wire_dependency_graph appends, once per package, inside a completed spec_fetch.
  if (result.fallbacks_started > 0) { wait_for_resolution_phase(); }

  for (auto [p, wr] : unresolved) {
    if (wr->resolved) { continue; }
    ++result.unresolved;
    if (wr->fallback) { continue; }
    result.missing_without_fallback.push_back(
        (wr->is_product ? "Product '" : "Reference '") + wr->query + "' in spec '" +
        p->cfg->identity + "' was not found");
  }

  if (!ambiguity_messages.empty()) {
    core_.fail_all();
    std::ostringstream oss;
    for (size_t i{ 0 }; i < ambiguity_messages.size(); ++i) {
      if (i) { oss << "\n"; }
      oss << ambiguity_messages[i];
    }
    throw std::runtime_error(oss.str());
  }

  return result;
}

void engine::resolve_graph(std::vector<pkg_cfg const *> const &roots) {
  // Register all roots before starting any thread so every manifest cfg's
  // SETUP selection is merged before a dependency thread can race ensure_pkg.
  std::vector<pkg *> root_pkgs;
  root_pkgs.reserve(roots.size());
  for (auto const *cfg : roots) { root_pkgs.push_back(ensure_pkg(cfg)); }

  // After root registration (its ensure_pkg calls may name the same packages) but
  // before any worker exists, so no string verb can outrun the shell's carve-out.
  // Interning only: the #default_shell task is what starts them, so a command that
  // never runs a string verb never builds the interpreter.
  intern_default_shell_deps();

  for (size_t i{ 0 }; i < root_pkgs.size(); ++i) {
    start_pkg_thread(root_pkgs[i], pkg_phase::spec_fetch);
  }

  size_t iteration{ 0 };
  while (true) {
    ++iteration;
    wait_for_resolution_phase();

    if (auto const errors{ aggregated_failures() }; !errors.empty()) {
      core_.fail_all();
      throw std::runtime_error(errors);
    }

    // No registry sweep here: register_products publishes at each package's own
    // spec_fetch completion, so by this barrier the registry already holds
    // everything a sweep would have found. Nor a resolved-weak-key sweep:
    // record_resolution appends each key the moment the reference resolves, which
    // is what a closure member hashing mid-resolution needs.
    weak_resolution_result const resolution{ resolve_weak_references() };

    if (resolution.resolved || resolution.fallbacks_started) { continue; }
    if (resolution.unresolved == 0) { break; }

    core_.fail_all();
    std::ostringstream oss;
    for (auto const &msg : resolution.missing_without_fallback) { oss << msg << "\n"; }
    oss << "Dependency resolution made no progress at iteration " << iteration << " with "
        << resolution.unresolved << " unresolved references";
    throw std::runtime_error(oss.str());
  }

  validate_product_fallbacks();
  validate_setup_selections();
}

}  // namespace envy
