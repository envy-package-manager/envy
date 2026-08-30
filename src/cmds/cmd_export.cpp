#include "cmd_export.h"

#include "engine.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_key.h"
#include "platform.h"
#include "reexec.h"
#include "self_deploy.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace envy {

cli_cmd &cmd_export::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("export", "Export cached packages as tar.zst archives") };
  sub.pos("queries", c.queries, "Package queries to export (export all if omitted)");
  sub.opt("-o,--output-dir", c.output_dir, "Output directory for archives");
  sub.opt("--manifest", c.manifest_path, "Path to envy.lua manifest");
  sub.opt("--depot-prefix", c.depot_prefix, "URL prefix for depot manifest output");
  sub.flag("--ignore-depot", c.ignore_depot, "Ignore package depot; rebuild from source")
      .envname("ENVY_IGNORE_DEPOT");
  return sub;
}

cmd_export::cmd_export(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_export::execute() {
  auto const [m, c]{ cmd_startup_load("export",
                                      cfg_.manifest_path,
                                      cli_cache_root_,
                                      false,
                                      cfg_.project_dir) };

  // Collect target packages: all if no queries, matched subset otherwise
  auto const targets{ [&] {
    std::vector<pkg_cfg const *> t;
    if (cfg_.queries.empty()) {
      for (auto const *pkg : m->packages) { t.push_back(pkg); }
      t = engine_filter_host_platform(t);
    } else {
      for (auto const &query : cfg_.queries) {
        bool found{ false };
        for (auto const *pkg : m->packages) {
          if (pkg_key const key{ *pkg }; key.matches(query)) {
            if (!util_platform_matches(pkg->platforms,
                                       platform::os_name(),
                                       platform::arch_name())) {
              throw std::runtime_error("export: '" + query +
                                       "' is not available on this platform");
            }
            t.push_back(pkg);
            found = true;
            break;
          }
        }
        if (!found) {
          throw std::runtime_error("export: no package matching '" + query + "'");
        }
      }
    }
    return t;
  }() };

  if (targets.empty()) {
    tui::info("No packages to export on this platform");
    return;
  }

  auto const target_keys{ [&] {
    std::vector<pkg_key> keys;
    keys.reserve(targets.size());
    for (auto const *cfg : targets) { keys.emplace_back(*cfg); }
    return keys;
  }() };

  // Create output directory before engine work (export phase needs it)
  std::filesystem::path const output_dir{ cfg_.output_dir
                                              ? *cfg_.output_dir
                                              : std::filesystem::current_path() };
  {
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
      throw std::runtime_error("export: failed to create output directory " +
                               output_dir.string() + ": " + ec.message());
    }
  }

  engine eng{ *c, m.get() };
  if (cfg_.ignore_depot) { eng.set_ignore_depot(true); }

  // Configure export phase — each package exports as soon as it finishes install
  eng.set_export_config(export_phase_config{
      .output_dir = output_dir,
      .depot_prefix = cfg_.depot_prefix,
      .explicitly_requested = !cfg_.queries.empty(),
      .export_targets =
          [&] {
            std::unordered_set<pkg_key> s;
            for (auto const *cfg : targets) { s.emplace(*cfg); }
            return s;
          }(),
  });

  eng.resolve_graph(targets);

  // Extend all targets to completion and wait
  for (auto const &key : target_keys) { eng.extend_to_completion(key); }
  for (auto const &key : target_keys) { eng.wait_for_completion(key); }

  // Delete TUI sections, then print results in deterministic target order
  for (auto const &key : target_keys) {
    pkg *p{ eng.find_exact(key) };
    if (p && p->tui_section) { tui::section_delete(p->tui_section); }
  }
  for (auto const &key : target_keys) {
    auto const *line{ eng.get_export_result(key) };
    if (line && !line->empty()) { tui::print_stdout("%s", line->c_str()); }
  }
}

}  // namespace envy
