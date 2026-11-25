#include "cmd_state.h"

#include "cache.h"
#include "engine.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_state.h"
#include "reexec.h"
#include "tree_hash.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace envy {

namespace {

namespace fs = std::filesystem;

// Runs `fn` on this host's graph resolved to spec level, the packages install resolves.
void with_resolved_graph(char const *cmd_name,
                         cmd_project_anchor const &anchor,
                         std::optional<fs::path> const &cache_root,
                         function_ref<void(manifest const &, engine const &)> fn) {
  auto const [m, c]{
    cmd_startup_load(cmd_name, std::nullopt, cache_root, false, anchor.project_dir)
  };
  engine eng{ *c, m.get() };
  eng.set_quiet_idle_outcomes(true);  // cache hits log at DEBUG
  std::vector<pkg_cfg const *> const roots{ m->packages.begin(), m->packages.end() };
  eng.resolve_graph(engine_filter_host_platform(roots));
  fn(*m, eng);
}

// Whether `path` exists, as a link or otherwise; throws when its parent hides the answer.
bool entry_exists(fs::path const &path) {
  std::error_code ec;
  auto const st{ fs::symlink_status(path, ec) };
  if (ec && st.type() != fs::file_type::not_found) {
    throw_runtime_error(
        { "state clean: cannot read ", util_path_to_utf8(path), ": ", ec.message() });
  }
  return fs::exists(st);
}

// Both clean forms delete beneath a real .envy/state holding neither cache nor markers.
void require_cleanable(envy_meta const &meta,
                       fs::path const &manifest_dir,
                       fs::path const &project_root) {
  cache_validate_project_dirs(meta.cache_request(std::nullopt, manifest_dir));
  for (auto const &dir : { project_root / ".envy", pkg_state_root(project_root) }) {
    if (!entry_exists(dir)) { return; }
    std::error_code ec;  // lstat reads a link or junction as itself, in any case spelling
    if (fs::symlink_status(dir, ec).type() != fs::file_type::directory) {
      throw_runtime_error({ "state clean: ",
                            util_path_to_utf8(dir),
                            " is a link or file; clean works beneath real directories" });
    }
  }
}

// Deletes `path` and its tree whatever their permissions, unlinking links in place.
void remove_entry(fs::path const &path) {
  if (auto const ec{ tree_remove_insisting(path) }) {
    throw_runtime_error(
        { "state clean: could not remove ", util_path_to_utf8(path), ": ", ec.message() });
  }
}

// The key `query` names: a package's (any revision or options), a product provider's, or
// a listed entry's own name. Each candidate counts, so a name naming two is ambiguous.
std::string state_key_for(engine const &eng,
                          std::vector<pkg_state_entry> const &entries,
                          std::string const &query) {
  auto const keys{ [&] {
    std::set<std::string> k;
    auto const add_package{ [&](pkg const *p) {
      std::string key{ pkg_state_key(p->cfg->identity) };
      if (!util_is_safe_path_component(key)) {  // a bundle spec's IDENTITY is unchecked
        throw_runtime_error(
            { "state clean: package key '", key, "' names a path outside .envy/state" });
      }
      k.insert(std::move(key));
    } };
    for (pkg const *p : eng.find_matches(query)) { add_package(p); }
    if (pkg const *p{ eng.find_product_provider(query) }) { add_package(p); }
    if (std::ranges::any_of(entries,
                            [&](pkg_state_entry const &e) { return e.name == query; })) {
      k.insert(query);
    }
    return k;
  }() };
  if (keys.size() > 1) {
    throw_runtime_error({ "state clean: query '",
                          query,
                          "' is ambiguous: ",
                          util_join({ keys.begin(), keys.end() }) });
  }
  if (keys.empty()) {
    throw_runtime_error(
        { "state clean: query '", query, "' matches no package, product or state entry" });
  }
  return *keys.begin();
}

}  // namespace

cli_cmd &cmd_state::register_cli(cli_cmd &app, cfg &) {
  return app.sub("state", "Show package state dirs and their disk usage");
}

cmd_state::cmd_state(cfg cfg, std::optional<fs::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_state::execute() {
  auto const list{ [](manifest const &m, engine const &eng) {
    auto const root{ pkg_state_root(m.project_root) };
    auto const entries{ pkg_state_entries(m.project_root) };
    auto const unused{ pkg_state_unused(eng, m.project_root, entries) };
    std::vector<cmd_size_row> rows;
    for (auto const &e : entries) {
      bool const dir{ e.type == fs::file_type::directory };
      bool const file{ e.type == fs::file_type::regular };
      auto const file_bytes{ [&]() -> std::uint64_t {
        std::error_code ec;  // a file gone since the listing sizes as zero
        auto const n{ file ? fs::file_size(e.path, ec) : 0 };
        return ec ? 0 : n;
      }() };
      bool const is_unused{ std::ranges::find(unused, e.name) != unused.end() };
      rows.push_back({ e.name,
                       dir ? e.path : fs::path{},  // a link lists by its own size
                       file_bytes,
                       std::string{ dir    ? ""
                                    : file ? "  (file)"
                                           : "  (link)" } +
                           (is_unused ? "  (unused)" : "") });
    }
    tui::print_stdout("State: %s\n", util_path_to_utf8(root).c_str());
    cmd_print_sizes("[state]", { { nullptr, std::move(rows) } });
  } };
  with_resolved_graph("state", cfg_, cli_cache_root_, list);
}

cli_cmd &cmd_state_clean::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("clean", "Remove package state dirs: the named ones, or all") };
  sub.pos("queries",
          c.queries,
          "Package, product or state dir names (remove all if omitted)");
  return sub;
}

cmd_state_clean::cmd_state_clean(cfg cfg, std::optional<fs::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_state_clean::clean_all() const {
  // The '@envy' header alone re-execs into the pinned envy, so a broken manifest resets.
  auto const found{ manifest::find_and_discover(std::nullopt, false, cfg_.project_dir) };
  auto const manifest_dir{ found.path.parent_path() };
  reexec_if_needed(
      found.meta,
      resolve_cache_root(found.meta.cache_request(cli_cache_root_, manifest_dir)),
      manifest_dir);

  auto const project_root{ util_canonical_path(found.path).parent_path() };
  require_cleanable(found.meta, manifest_dir, project_root);
  auto const root{ pkg_state_root(project_root) };
  if (!entry_exists(root)) {
    tui::info("No package state in %s", util_path_to_utf8(root).c_str());
    return;
  }
  std::error_code ec;  // an unreadable root lists none; its removal takes all of it
  auto const entries{ pkg_state_entries(project_root, ec) };
  remove_entry(root);
  for (auto const &e : entries) {
    tui::info("Removed %s", util_path_to_utf8(e.path).c_str());
  }
  if (entries.empty()) { tui::info("Removed %s", util_path_to_utf8(root).c_str()); }
}

void cmd_state_clean::execute() {
  if (cfg_.queries.empty()) {
    clean_all();
    return;
  }

  auto const clean{ [&](manifest const &m, engine const &eng) {
    require_cleanable(m.meta, m.manifest_path.parent_path(), m.project_root);
    auto const root{ pkg_state_root(m.project_root) };
    std::error_code ec;  // a read-only or unsearchable root lists and deletes too
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ec);
    auto const entries{ pkg_state_entries(m.project_root) };
    auto const keys{ [&] {  // every query resolves before the first deletion
      std::set<std::string> k;
      for (auto const &q : cfg_.queries) { k.insert(state_key_for(eng, entries, q)); }
      return k;
    }() };

    auto const packages{ eng.packages() };
    for (auto const &key : keys) {
      auto const dir{ root / util_path_from_utf8(key) };
      if (!entry_exists(dir)) {
        tui::info("%s has no state", key.c_str());
        continue;
      }
      remove_entry(dir);
      auto const shared{ [&] {  // one dir serves every revision and option set of a spec
        std::vector<std::string> users;
        for (pkg const *p : packages) {
          if (pkg_state_key(p->cfg->identity) == key) {
            users.push_back(p->key.canonical());
          }
        }
        util_sort(users);
        return users.size() > 1 ? " (shared by " + util_join(users) + ")" : std::string{};
      }() };
      tui::info("Removed %s%s", util_path_to_utf8(dir).c_str(), shared.c_str());
    }
  } };
  with_resolved_graph("state clean", cfg_, cli_cache_root_, clean);
}

}  // namespace envy
