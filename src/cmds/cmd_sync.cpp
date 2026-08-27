#include "cmd_sync.h"

#include "deploy.h"
#include "engine.h"
#include "luarc.h"
#include "manifest.h"
#include "reexec.h"
#include "self_deploy.h"
#include "util.h"

#include "CLI11.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace envy {

namespace fs = std::filesystem;

void cmd_sync::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("sync", "Install packages and deploy product scripts") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("queries",
                  cfg_ptr->queries,
                  "Package queries to sync (sync all if omitted)");
  auto *manifest_opt{
    sub->add_option("--manifest", cfg_ptr->manifest_path, "Path to envy.lua manifest")
  };
  sub->add_flag("--strict",
                cfg_ptr->strict,
                "Error on non-envy-managed product script conflicts");
  sub->add_flag("--subproject",
                cfg_ptr->subproject,
                "Use nearest manifest instead of walking to root")
      ->excludes(manifest_opt);
  sub->add_option("--platform",
                  cfg_ptr->platform_flag,
                  "Script platform: posix, windows, or all (default: current OS)")
      ->check(CLI::IsMember({ "posix", "windows", "all" }));
  sub->add_flag("--ignore-depot",
                cfg_ptr->ignore_depot,
                "Ignore package depot; rebuild from source")
      ->envname("ENVY_IGNORE_DEPOT");
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_sync::cmd_sync(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_sync::execute() {
  auto const [m, c]{
    cmd_startup_load("sync",
                     cfg_.manifest_path,
                     cli_cache_root_,
                     cfg_.subproject,
                     subproject_anchor(cfg_.subproject, cfg_.project_dir))
  };

  if (!m->meta.bin) {
    throw std::runtime_error(
        "sync: manifest missing '@envy bin' directive (required for sync)");
  }

  auto const platforms{ util_parse_platform_flag(cfg_.platform_flag) };

  fs::path const manifest_dir{ m->manifest_path.parent_path() };
  update_luarc_types_path(manifest_dir, m->meta);

  fs::path const bin_dir{ manifest_dir / *m->meta.bin };

  if (!fs::exists(bin_dir)) {
    std::error_code ec;
    fs::create_directories(bin_dir, ec);
    if (ec) {
      throw std::runtime_error("sync: failed to create bin directory " + bin_dir.string() +
                               ": " + ec.message());
    }
  }

  auto const targets{ engine_resolve_targets(m->packages, cfg_.queries, "sync") };

  if (targets.empty()) { return; }

  // Install packages (full build pipeline — filters to host platform internally)
  engine eng{ *c, m.get() };
  if (cfg_.ignore_depot) { eng.set_ignore_depot(true); }
  auto result{ eng.run_full(targets) };

  size_t failed{ 0 };
  for (auto const &[key, outcome] : result) {
    if (outcome.type == pkg_type::UNKNOWN) { ++failed; }
  }

  if (failed > 0) {
    throw std::runtime_error("sync: " + std::to_string(failed) + " package(s) failed");
  }

  // Resolve non-host targets so deploy knows about their products for script generation.
  // run_full only resolves host-platform packages; without this, deploy cleanup would
  // remove scripts for valid non-host packages (e.g. linux-only on macOS).
  auto const host_targets{ engine_filter_host_platform(targets) };
  auto const non_host_targets{ [&] {
    std::vector<pkg_cfg const *> nh;
    for (auto const *t : targets) {
      if (std::find(host_targets.begin(), host_targets.end(), t) == host_targets.end()) {
        nh.push_back(t);
      }
    }
    return nh;
  }() };
  if (!non_host_targets.empty()) { eng.resolve_graph(non_host_targets); }

  // Deploy product scripts
  auto const products{ eng.collect_all_products() };

  deploy_finalize(bin_dir,
                  products,
                  platforms,
                  cfg_.strict,
                  m->manifest_path,
                  m->meta);
}

}  // namespace envy
