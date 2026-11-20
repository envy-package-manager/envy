#include "activation.h"

#include "blake3_util.h"
#include "cache.h"
#include "cmd.h"
#include "engine.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_env.h"
#include "pkg_key.h"
#include "platform.h"
#include "tui.h"
#include "util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace envy {

namespace {

// Depth-first from the roots, each package before its dependencies and children in key
// order: the order activation's "earlier wins" precedence is defined over.
std::vector<pkg *> walk_graph(std::vector<pkg *> const &roots) {
  std::vector<pkg *> order;
  std::unordered_set<pkg *> seen;
  auto const visit{ [&](auto const &self, pkg *p) -> void {
    if (!p || !seen.insert(p).second) { return; }
    order.push_back(p);
    std::vector<pkg *> deps{ [&] {
      std::vector<pkg *> out;
      std::lock_guard const deps_lock(p->deps_mutex);
      for (auto const &[_, dep] : p->dependencies) { out.push_back(dep.p); }
      return out;
    }() };
    util_sort(deps, [](pkg const *a, pkg const *b) {
      return a->key.canonical() < b->key.canonical();
    });
    for (pkg *d : deps) { self(self, d); }
  } };
  for (pkg *p : roots) { visit(visit, p); }
  return order;
}

std::vector<pkg *> walk_graph(engine const &eng,
                              std::vector<pkg_cfg const *> const &roots) {
  std::vector<pkg *> pkgs;
  for (auto const *cfg : roots) { pkgs.push_back(eng.find_exact(pkg_key{ *cfg })); }
  return walk_graph(pkgs);
}

struct keyed_layer {
  std::string key;
  env_ops ops;
};

// The ENV layers of the graph `eng` resolved, walked from `roots` in priority order.
// Skips packages not in the cache. An ENV that throws inside `strict` propagates;
// anywhere else it warns and is skipped, so one broken spec costs only its own ENV.
std::vector<keyed_layer> engine_layers(engine const &eng,
                                       std::vector<pkg_cfg const *> const &roots,
                                       std::filesystem::path const &root,
                                       std::unordered_set<pkg *> const &strict) {
  std::vector<keyed_layer> layers;
  for (pkg *p : walk_graph(eng, roots)) {
    if (!p->has_env) { continue; }
    std::optional<std::filesystem::path> pkg_path;
    if (p->type == pkg_type::CACHE_MANAGED) {
      pkg_path = pkg_predicted_path(p);
      if (!cache::is_entry_complete(pkg_path->parent_path())) { continue; }
    } else if (p->type != pkg_type::USER_MANAGED) {
      continue;
    }

    try {
      if (auto ops{ pkg_env_eval(p, pkg_path, root) }) {
        layers.push_back({ .key = p->key.canonical(), .ops = std::move(*ops) });
      }
    } catch (std::exception const &e) {
      if (strict.contains(p)) {
        throw_runtime_error({ "ENV of ", p->cfg->identity, ": ", e.what() });
      }
      tui::warn("activate: skipping ENV of %s: %s", p->cfg->identity.c_str(), e.what());
    }
  }
  return layers;
}

// The graph under `roots` -- plus `providers`, which may satisfy their weak references --
// resolved by one engine and walked from `roots` into ENV layers in priority order. Skips
// packages not in the cache. Throws if unresolvable.
std::vector<keyed_layer> resolve_layers(cmd_startup const &startup,
                                        std::vector<pkg_cfg const *> const &roots,
                                        std::vector<pkg_cfg const *> const &providers,
                                        std::filesystem::path const &root) {
  engine eng{ *startup.c, startup.m.get() };
  eng.set_quiet_idle_outcomes(true);  // a local bundle read in place is no news per prompt
  eng.resolve_graph([&] {
    auto all{ providers };
    all.insert(all.end(), roots.begin(), roots.end());
    return all;
  }());

  return engine_layers(eng, roots, root, {});
}

// Keeps the first layer per package key: a dependency shared by two roots applies once.
void keep_layers(std::vector<keyed_layer> found,
                 std::unordered_set<std::string> &seen,
                 std::vector<env_ops> &layers) {
  for (auto &l : found) {
    if (seen.insert(l.key).second) { layers.push_back(std::move(l.ops)); }
  }
}

// Every installed package's ENV with root = the project root, highest priority first.
// Uninstalled packages and uncached specs are skipped silently: shims install lazily.
void package_layers(cmd_startup const &startup,
                    std::filesystem::path const &root,
                    std::vector<env_ops> &layers) {
  auto const roots{ engine_filter_host_platform(
      { startup.m->packages.begin(), startup.m->packages.end() }) };
  std::unordered_set<std::string> seen;
  auto const keep{ [&](std::vector<keyed_layer> found) {
    keep_layers(std::move(found), seen, layers);
  } };

  try {
    keep(resolve_layers(startup, roots, {}, root));
    return;
  } catch (std::exception const &) {}

  // One unresolvable root (an uncached spec, a Lua error) must not cost the others their
  // ENV: resolve each alone, keeping what resolves, in root order.
  std::vector<std::vector<keyed_layer>> found(roots.size());
  std::vector<pkg_cfg const *> resolved;  // roots that resolved: candidate providers
  std::vector<size_t> deferred;           // roots whose weak references name another root
  std::vector<std::string> why(roots.size());  // a deferred root's latest failure
  bool any_uncached{ false };
  auto const skip{ [&](size_t i, char const *what) {
    tui::warn("activate: skipping %s: %s", roots[i]->identity.c_str(), what);
  } };
  for (size_t i{ 0 }; i < roots.size(); ++i) {
    try {
      found[i] = resolve_layers(startup, { roots[i] }, {}, root);
      resolved.push_back(roots[i]);
    } catch (unresolved_reference_error const &) {
      deferred.push_back(i);
    } catch (std::exception const &e) {
      if (std::string_view{ e.what() }.find(kCacheReadOnlyMissText) !=
          std::string_view::npos) {
        any_uncached = true;
      } else {
        skip(i, e.what());
      }
    }
  }

  // Retry those with every root that resolved, each success joining the providers, until a
  // pass resolves none: a chain of product dependencies resolves link by link.
  for (size_t n{ 0 }; n != deferred.size();) {
    n = deferred.size();
    std::erase_if(deferred, [&](size_t i) {
      try {
        found[i] = resolve_layers(startup, { roots[i] }, resolved, root);
        resolved.push_back(roots[i]);
      } catch (unresolved_reference_error const &e) {
        why[i] = e.what();
        return false;
      } catch (std::exception const &e) { skip(i, e.what()); }
      return true;
    });
  }

  // Still unresolved while a root is uncached, the provider may be that root: not
  // installed, not a failure.
  if (!any_uncached) {
    for (size_t const i : deferred) { skip(i, why[i].c_str()); }
  }

  for (auto &f : found) { keep(std::move(f)); }
}

// Built-ins outrank every package: ENVY_PROJECT_ROOT, and the bin dir ahead on PATH.
env_ops builtin_layer(std::filesystem::path const &root, envy_meta const &meta) {
  env_ops out{ { .name = "ENVY_PROJECT_ROOT", .op = env_set{ root.string() } } };
  if (meta.bin) {
    std::error_code ec;
    if (auto const bin{ std::filesystem::canonical(root / *meta.bin, ec) }; !ec) {
      out.push_back({ .name = "PATH",
                      .op = env_edit{ .prepend = { bin.string() },
                                      .sep = std::string(1, env_path_sep()) } });
    }
  }
  return out;
}

// `layers` (highest first) over `live` with any prior activation undone, plus the state
// that records it.
activation finish(shell_env_t const &live,
                  activation_anchor const &anchor,
                  std::vector<env_ops> const &layers) {
  shell_env_t const base{ env_restore(live) };
  shell_env_t target{ base };
  for (auto it{ layers.rbegin() }; it != layers.rend(); ++it) {
    env_ops_apply(target, *it);
  }

  size_t const vars{ env_changed_names(base, target).size() };
  return { .target = env_with_state(base,
                                    std::move(target),
                                    { .root = anchor.root.string(),
                                      .exe = platform::get_exe_path().string(),
                                      .genfile = anchor.genfile.string(),
                                      .gen = anchor.gen }),
           .root = anchor.root.string(),
           .vars = vars };
}

}  // namespace

activation_anchor activation_anchor_read(std::filesystem::path const &cache_root,
                                         std::filesystem::path const &manifest_path) {
  std::filesystem::path root{ util_canonical_path(manifest_path).parent_path() };
  auto genfile{ activation_genfile(cache_root, root) };
  std::string gen{ activation_gen_read(genfile) };
  return { .root = std::move(root), .genfile = std::move(genfile), .gen = std::move(gen) };
}

activation activation_compute(std::optional<std::filesystem::path> const &project_dir,
                              std::optional<std::filesystem::path> const &cli_cache_root,
                              shell_env_t const &live,
                              bool reuse_current) {
  // Re-exec into the pinned envy before anything reads the state: that envy owns its
  // format. A manifest that fails to load still activates its bin dir and root.
  auto const found{ manifest::find_and_discover(std::nullopt, false, project_dir) };

  // Read before any package is inspected: an install finishing mid-activation then leaves
  // the recorded generation stale, and the next prompt refreshes.
  auto const anchor{ activation_anchor_read(
      resolve_cache_root(
          found.meta.cache_request(cli_cache_root, found.path.parent_path()))
          .root,
      found.path) };
  if (reuse_current && env_activation_current(live, anchor.root.string(), anchor.gen)) {
    return { .target = live, .root = anchor.root.string(), .vars = 0 };
  }

  std::optional<cmd_startup> startup;
  try {
    startup.emplace(cmd_startup_load("activate",
                                     found.path,
                                     cli_cache_root,
                                     false,
                                     std::nullopt,
                                     true));
  } catch (std::exception const &e) {
    tui::warn("activate: %s; only the bin dir is active", e.what());
  }

  std::vector<env_ops> layers{ builtin_layer(anchor.root, found.meta) };  // highest first
  if (startup) {
    try {
      package_layers(*startup, anchor.root, layers);
    } catch (std::exception const &e) {
      tui::warn("activate: package ENV skipped: %s", e.what());
    }
  }
  return finish(live, anchor, layers);
}

activation activation_from_engine(engine const &eng,
                                  manifest const &m,
                                  activation_anchor const &anchor,
                                  shell_env_t const &live,
                                  pkg *strict) {
  auto const closure{ walk_graph({ strict }) };
  std::vector<env_ops> layers{ builtin_layer(anchor.root, m.meta) };
  std::unordered_set<std::string> seen;
  keep_layers(
      engine_layers(eng,
                    engine_filter_host_platform({ m.packages.begin(), m.packages.end() }),
                    anchor.root,
                    { closure.begin(), closure.end() }),
      seen,
      layers);
  return finish(live, anchor, layers);
}

std::filesystem::path activation_genfile(std::filesystem::path const &cache_root,
                                         std::filesystem::path const &project_root) {
  std::string const key{ util_normalized_path(project_root) };
  auto const digest{ blake3_hash(key.data(), key.size()) };
  return cache_root / "activate" / util_bytes_to_hex(digest.data(), 8);
}

std::string activation_gen_read(std::filesystem::path const &genfile) {
  std::error_code ec;
  if (!std::filesystem::exists(genfile, ec)) { return "0"; }
  try {
    auto const bytes{ util_load_file(genfile) };
    std::string gen{ bytes.begin(), bytes.end() };
    while (!gen.empty() && (gen.back() == '\n' || gen.back() == '\r')) { gen.pop_back(); }
    return gen.empty() ? "0" : gen;
  } catch (std::exception const &) { return "0"; }
}

void activation_gen_bump(std::filesystem::path const &genfile) {
  try {  // a fresh token, never a counter: two racing installs still both change it
    auto const now{ std::chrono::system_clock::now().time_since_epoch().count() };
    std::filesystem::create_directories(genfile.parent_path());
    util_write_file(genfile, std::to_string(now) + "\n");
  } catch (std::exception const &e) {
    tui::debug("activate: could not bump %s: %s", genfile.string().c_str(), e.what());
  }
}

activation_refresh_guard::activation_refresh_guard(
    std::filesystem::path const &cache_root,
    std::filesystem::path const &manifest_path)
    : genfile_{ activation_genfile(cache_root,
                                   util_canonical_path(manifest_path).parent_path()) } {}

activation_refresh_guard::~activation_refresh_guard() { activation_gen_bump(genfile_); }

env_shell activation_shell(std::string_view name, std::string_view cmd) {
  auto const sh{ env_shell_parse(name) };
  if (!sh) { throw_runtime_error({ cmd, ": unsupported shell '", name, "'" }); }
#ifdef _WIN32
  if (*sh != env_shell::POWERSHELL && *sh != env_shell::CMD) {
    throw_runtime_error({ cmd, ": only powershell and cmd are supported on Windows" });
  }
#else
  if (*sh == env_shell::CMD) {
    throw_runtime_error({ cmd, ": cmd is supported only on Windows" });
  }
#endif
  return *sh;
}

bool activation_stdout_is_tty() {
#ifdef _WIN32
  return ::_isatty(::_fileno(stdout)) != 0;
#else
  return ::isatty(::fileno(stdout)) != 0;
#endif
}

std::string activation_eval_hint(std::string_view cmd, std::string_view shell) {
  std::string const call{ "envy " + std::string{ cmd } + " " + std::string{ shell } };
  if (shell == "fish") { return call + " | source"; }
  if (shell == "powershell") { return call + " | Out-String | Invoke-Expression"; }
  if (shell == "cmd") {  // the launcher runs the `set` lines in the caller's session
    return call + ", through the project's envy.bat";
  }
  return "eval \"$(" + call + ")\"";
}

void activation_announce(char const *verb,
                         std::string const &root,
                         std::string const &tail) {
  if (char const *quiet{ std::getenv("ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE") };
      quiet && std::string_view{ quiet } == "1") {
    return;
  }
  std::string_view const locale{ [] {
    for (char const *v : { "LC_ALL", "LC_CTYPE", "LANG" }) {
      if (char const *s{ std::getenv(v) }; s && *s) { return std::string_view{ s }; }
    }
    return std::string_view{};
  }() };
  bool const utf8{ locale.find("UTF-8") != std::string_view::npos ||
                   locale.find("utf-8") != std::string_view::npos ||
                   locale.find("UTF8") != std::string_view::npos ||
                   locale.find("utf8") != std::string_view::npos };
  std::string const name{ std::filesystem::path{ root }.filename().string() };
  tui::info("envy: %s %s %s %s",
            verb,
            name.c_str(),
            utf8 ? "\xE2\x80\x94" : "--",
            tail.c_str());
}

}  // namespace envy
