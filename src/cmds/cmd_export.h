#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_export : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_export>, cmd_project_anchor {
    std::vector<std::string> queries;  // Optional: if empty, export all manifest packages
    std::optional<std::filesystem::path> output_dir;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::string> depot_prefix;
    bool ignore_depot = false;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_export(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
