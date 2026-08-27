#include "cmd_install.h"

#include "engine.h"
#include "manifest.h"
#include "pkg_key.h"
#include "platform.h"
#include "reexec.h"
#include "self_deploy.h"
#include "util.h"

#include "CLI11.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace envy {

namespace fs = std::filesystem;

void cmd_install::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("install", "Install packages from manifest") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("queries",
                  cfg_ptr->queries,
                  "Package queries to install (install all if omitted)");
  sub->add_option("--manifest", cfg_ptr->manifest_path, "Path to envy.lua manifest");
  sub->add_flag("--ignore-depot",
                cfg_ptr->ignore_depot,
                "Ignore package depot; rebuild from source")
      ->envname("ENVY_IGNORE_DEPOT");
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_install::cmd_install(cfg cfg, std::optional<fs::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_install::execute() {
  auto const [m, c]{
    cmd_startup_load("install",
                     cfg_.manifest_path,
                     cli_cache_root_,
                     false,
                     cfg_.project_dir)
  };

  auto const targets{ [&] {
    std::vector<pkg_cfg const *> t;
    if (cfg_.queries.empty()) {
      for (auto const *pkg : m->packages) { t.push_back(pkg); }
    } else {
      for (auto const &query : cfg_.queries) {
        bool found{ false };
        for (auto const *pkg : m->packages) {
          if (pkg_key const key{ *pkg }; key.matches(query)) {
            if (!util_platform_matches(pkg->platforms,
                                       platform::os_name(),
                                       platform::arch_name())) {
              throw std::runtime_error("install: '" + query +
                                       "' is not available on this platform");
            }
            t.push_back(pkg);
            found = true;
            break;
          }
        }
        if (!found) {
          throw std::runtime_error("install: query '" + query + "' not found in manifest");
        }
      }
    }
    return t;
  }() };

  if (targets.empty()) { return; }

  engine eng{ *c, m.get() };
  if (cfg_.ignore_depot) { eng.set_ignore_depot(true); }
  auto result{ eng.run_full(targets) };

  size_t failed{ 0 };
  for (auto const &[key, outcome] : result) {
    if (outcome.type == pkg_type::UNKNOWN) { ++failed; }
  }

  if (failed > 0) {
    throw std::runtime_error("install: " + std::to_string(failed) + " package(s) failed");
  }
}

}  // namespace envy
