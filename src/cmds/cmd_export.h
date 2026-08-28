#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace CLI { class App; }

namespace envy {

class cmd_export : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_export>, cmd_project_anchor {
    std::vector<std::string> queries;  // Optional: if empty, export all manifest packages
    std::optional<std::filesystem::path> output_dir;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::string> depot_prefix;
    bool ignore_depot = false;
  };

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_export(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
