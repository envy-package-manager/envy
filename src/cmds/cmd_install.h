#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_install : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_install>, cmd_project_anchor {
    std::vector<std::string> queries;  // Optional: if empty, install all manifest packages
    std::optional<std::filesystem::path> manifest_path;
    bool ignore_depot = false;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_install(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
