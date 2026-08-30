#include "cmd_deploy.h"

#include "deploy.h"
#include "engine.h"
#include "luarc.h"
#include "manifest.h"
#include "reexec.h"
#include "self_deploy.h"
#include "util.h"

#include "cli_parse.h"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace envy {

namespace fs = std::filesystem;

cli_cmd &cmd_deploy::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("deploy", "Deploy product scripts") };
  sub.pos("identities", c.identities, "Spec identities to deploy (deploy all if omitted)");
  auto const manifest_opt{
    sub.opt("--manifest", c.manifest_path, "Path to envy.lua manifest")
  };
  sub.flag("--strict", c.strict, "Error on non-envy-managed product script conflicts");
  sub.flag("--subproject", c.subproject, "Use nearest manifest instead of walking to root")
      .excludes(manifest_opt);
  sub.opt("--platform",
          c.platform_flag,
          "Script platform: posix, windows, or all (default: current OS)")
      .one_of("posix,windows,all");
  return sub;
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
