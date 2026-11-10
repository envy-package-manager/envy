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

// Depth-first from the manifest roots, each package before its dependencies and children
// in key order: the order activation's "earlier wins" precedence is defined over.
std::vector<pkg *> walk_graph(engine const &eng, std::vector<pkg_cfg const *> const &roots) {
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
  for (auto const *cfg : roots) { visit(visit, eng.find_exact(pkg_key{ *cfg })); }
  return order;
}

struct keyed_layer {
  std::string key;
  env_ops ops;
};

// The graph under `roots`, resolved by one engine and walked into ENV layers in priority
// order; `uninstalled` counts ENV-bearing packages not in the cache. Throws if unresolvable.
std::vector<keyed_layer> resolve_layers(cmd_startup const &startup,
                                        std::vector<pkg_cfg const *> const &roots,
                                        std::filesystem::path const &root,
                                        size_t &uninstalled) {
  engine eng{ *startup.c, startup.m.get() };
  eng.resolve_graph(roots);

  std::vector<keyed_layer> layers;
  for (pkg *p : walk_graph(eng, roots)) {
    if (!p->has_env) { continue; }
    std::optional<std::filesystem::path> pkg_path;
    if (p->type == pkg_type::CACHE_MANAGED) {
      pkg_path = pkg_predicted_path(p);
      if (!cache::is_entry_complete(pkg_path->parent_path())) {
        ++uninstalled;
        continue;
      }
    } else if (p->type != pkg_type::USER_MANAGED) {
      continue;
    }

    try {
      if (auto ops{ pkg_env_eval(p, pkg_path, root) }) {
        layers.push_back({ .key = p->key.canonical(), .ops = std::move(*ops) });
      }
    } catch (std::exception const &e) {
      tui::warn("activate: skipping ENV of %s: %s", p->cfg->identity.c_str(), e.what());
    }
  }
  return layers;
}

// Every installed package's ENV with root = the project root, highest priority first.
// Returns the count of packages declaring ENV that are not installed yet.
size_t package_layers(cmd_startup const &startup,
                      std::filesystem::path const &root,
                      std::vector<env_ops> &layers) {
  auto const roots{ engine_filter_host_platform(
      { startup.m->packages.begin(), startup.m->packages.end() }) };
  size_t uninstalled{ 0 };
  std::unordered_set<std::string> seen;  // a dependency shared by two roots applies once
  auto const keep{ [&](std::vector<keyed_layer> found) {
    for (auto &l : found) {
      if (seen.insert(l.key).second) { layers.push_back(std::move(l.ops)); }
    }
  } };

  try {
    keep(resolve_layers(startup, roots, root, uninstalled));
    return uninstalled;
  } catch (std::exception const &) {}

  // One unresolvable root (an uncached spec, a Lua error) must not cost the others their
  // ENV: resolve each alone, in order, keeping what resolves.
  for (auto const *cfg : roots) {
    try {
      keep(resolve_layers(startup, { cfg }, root, uninstalled));
    } catch (std::exception const &e) {
      if (std::string_view{ e.what() }.find(kCacheReadOnlyMissText) != std::string_view::npos) {
        ++uninstalled;
      } else {
        tui::warn("activate: skipping %s: %s", cfg->identity.c_str(), e.what());
      }
    }
  }
  return uninstalled;
}

}  // namespace

activation activation_compute(std::optional<std::filesystem::path> const &project_dir,
                              std::optional<std::filesystem::path> const &cli_cache_root,
                              shell_env_t const &live) {
  // Re-exec into the pinned envy before anything reads the state: that envy owns its format.
  // A manifest that fails to load still activates its bin dir and root.
  auto const found{ manifest::find_and_discover(std::nullopt, false, project_dir) };
  std::filesystem::path const root{ util_canonical_path(found.path).parent_path() };

  // Read before any package is inspected: an install finishing mid-activation then leaves
  // the recorded generation stale, and the next prompt refreshes.
  auto const genfile{ activation_genfile(
      resolve_cache_root(found.meta.cache_request(cli_cache_root, found.path.parent_path()))
          .root,
      root) };
  std::string const gen{ activation_gen_read(genfile) };

  std::optional<cmd_startup> startup;
  try {
    startup.emplace(
        cmd_startup_load("activate", found.path, cli_cache_root, false, std::nullopt, true));
  } catch (std::exception const &e) {
    tui::warn("activate: %s; only the bin dir is active", e.what());
  }

  shell_env_t const base{ env_restore(live) };

  std::vector<env_ops> layers{ { { .name = "ENVY_PROJECT_ROOT",
                                   .op = env_set{ root.string() } } } };  // highest first
  if (found.meta.bin) {
    std::error_code ec;
    if (auto const bin{ std::filesystem::canonical(root / *found.meta.bin, ec) }; !ec) {
      layers.front().push_back({ .name = "PATH",
                                 .op = env_edit{ .prepend = { bin.string() },
                                                 .sep = std::string(1, env_path_sep()) } });
    }
  }

  if (startup) {
    try {
      if (size_t const n{ package_layers(*startup, root, layers) }) {
        tui::warn("activate: %zu package%s not installed; run 'envy install'",
                  n,
                  n == 1 ? "" : "s");
      }
    } catch (std::exception const &e) {
      tui::warn("activate: package ENV skipped: %s", e.what());
    }
  }

  shell_env_t target{ base };
  for (auto it{ layers.rbegin() }; it != layers.rend(); ++it) { env_ops_apply(target, *it); }

  size_t const vars{ env_changed_names(base, target).size() };
  return { .target = env_with_state(base,
                                    std::move(target),
                                    { .root = root.string(),
                                      .exe = platform::get_exe_path().string(),
                                      .genfile = genfile.string(),
                                      .gen = gen }),
           .root = root.string(),
           .vars = vars };
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

activation_refresh_guard::activation_refresh_guard(
    std::filesystem::path const &cache_root,
    std::filesystem::path const &manifest_path)
    : genfile_{ activation_genfile(cache_root,
                                   util_canonical_path(manifest_path).parent_path()) } {}

activation_refresh_guard::~activation_refresh_guard() {
  try {  // a fresh token, never a counter: two racing installs still both change it
    auto const now{ std::chrono::system_clock::now().time_since_epoch().count() };
    std::filesystem::create_directories(genfile_.parent_path());
    util_write_file(genfile_, std::to_string(now) + "\n");
  } catch (std::exception const &e) {
    tui::debug("activate: could not bump %s: %s", genfile_.string().c_str(), e.what());
  }
}

env_shell activation_shell(std::string_view name, std::string_view cmd) {
  auto const sh{ env_shell_parse(name) };
  if (!sh) { throw_runtime_error({ cmd, ": unsupported shell '", name, "'" }); }
#ifdef _WIN32
  if (*sh != env_shell::POWERSHELL) {
    throw_runtime_error({ cmd, ": only powershell is supported on Windows" });
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
  return "eval \"$(" + call + ")\"";
}

void activation_announce(char const *verb, std::string const &root, std::string const &tail) {
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
