#include "cmd_package.h"

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

#include "CLI11.hpp"

#include <memory>
#include <set>

namespace envy {

void cmd_package::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("package",
                                "Query and install package, print package path") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("identity",
                  cfg_ptr->identity,
                  "Package identity (partial matching supported)")
      ->required();
  sub->add_option("--manifest", cfg_ptr->manifest_path, "Path to envy.lua manifest");
  sub->add_flag("--ignore-depot",
                cfg_ptr->ignore_depot,
                "Ignore package depot; rebuild from source")
      ->envname("ENVY_IGNORE_DEPOT");
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_package::cmd_package(cfg cfg,
                         std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_package::execute() {
  auto const [m, c]{
    cmd_startup_load("package", cfg_.manifest_path, cli_cache_root_, false,
                     cfg_.project_dir)
  };

  // Collect all packages that match the query (supports partial matching)
  std::vector<pkg_cfg const *> matches;
  for (auto const *pkg : m->packages) {
    pkg_key const key{ *pkg };
    if (key.matches(cfg_.identity)) {
      if (!util_platform_matches(pkg->platforms,
                                 platform::os_name(),
                                 platform::arch_name())) {
        throw std::runtime_error("package: '" + cfg_.identity +
                                 "' is not available on this platform");
      }
      matches.push_back(pkg);
    }
  }

  if (matches.empty()) {
    throw std::runtime_error("package: no package matching '" + cfg_.identity + "'");
  }

  // Check for ambiguous matches (different identities)
  auto const unique_identities{ [&] {
    std::set<std::string> ids;
    for (auto const *pkg : matches) { ids.insert(pkg->identity); }
    return ids;
  }() };

  if (unique_identities.size() > 1) {
    std::string msg{ "package: '" + cfg_.identity + "' is ambiguous, matches:\n" };
    for (auto const &id : unique_identities) { msg += "  " + id + "\n"; }
    throw std::runtime_error(msg);
  }

  // All matches have the same identity; verify they have the same options
  if (matches.size() > 1) {
    std::string first_key{ pkg_cfg::format_key(matches[0]->identity,
                                               matches[0]->serialized_options) };
    for (size_t i{ 1 }; i < matches.size(); ++i) {
      std::string key{ pkg_cfg::format_key(matches[i]->identity,
                                           matches[i]->serialized_options) };
      if (key != first_key) {
        throw std::runtime_error("package: '" + cfg_.identity +
                                 "' appears multiple times with different options");
      }
    }
  }

  engine eng{ *c, m.get() };
  if (cfg_.ignore_depot) { eng.set_ignore_depot(true); }

  eng.resolve_graph({ m->packages.begin(), m->packages.end() });

  pkg_key const target_key{ *matches[0] };
  pkg *p{ eng.find_exact(target_key) };
  if (!p) { throw std::runtime_error("package: spec not found in graph"); }

  eng.wait_for_completion(p->key);

  if (p->type != pkg_type::CACHE_MANAGED) {
    throw std::runtime_error("package: package is not cache-managed");
  }

  tui::print_stdout("%s\n", p->pkg_path.string().c_str());
}

}  // namespace envy
