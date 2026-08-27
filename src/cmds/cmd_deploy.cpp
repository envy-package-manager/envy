#include "cmd_deploy.h"

#include "deploy.h"
#include "engine.h"
#include "luarc.h"
#include "manifest.h"
#include "reexec.h"
#include "self_deploy.h"
#include "util.h"

#include "CLI11.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace envy {

namespace fs = std::filesystem;

void cmd_deploy::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("deploy", "Deploy product scripts") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("identities",
                  cfg_ptr->identities,
                  "Spec identities to deploy (deploy all if omitted)");
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
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_deploy::cmd_deploy(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_deploy::execute() {
  auto const [m,
              c]{ cmd_startup_load("deploy",
                                   cfg_.manifest_path,
                                   cli_cache_root_,
                                   cfg_.subproject,
                                   subproject_anchor(cfg_.subproject, cfg_.project_dir)) };

  if (!m->meta.bin) {
    throw std::runtime_error(
        "deploy: manifest missing '@envy bin' directive (required for deploy)");
  }

  auto const platforms{ util_parse_platform_flag(cfg_.platform_flag) };

  fs::path const manifest_dir{ m->manifest_path.parent_path() };
  update_luarc_types_path(manifest_dir, m->meta);

  fs::path const bin_dir{ manifest_dir / *m->meta.bin };

  if (!fs::exists(bin_dir)) {
    std::error_code ec;
    fs::create_directories(bin_dir, ec);
    if (ec) {
      throw std::runtime_error("deploy: failed to create bin directory " +
                               bin_dir.string() + ": " + ec.message());
    }
  }

  auto const targets{ engine_resolve_targets(m->packages, cfg_.identities, "deploy") };

  if (targets.empty()) { return; }

  engine eng{ *c, m.get() };
  eng.resolve_graph(targets);

  auto const products{ eng.collect_all_products() };

  // Check deploy directive: absent or false means deployment disabled

  deploy_finalize(bin_dir, products, platforms, cfg_.strict, m->manifest_path, m->meta);
}

}  // namespace envy
