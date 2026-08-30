#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_sync : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_sync>, cmd_project_anchor {
    std::vector<std::string> queries;  // Package filter (install all if empty)
    std::optional<std::filesystem::path> manifest_path;
    bool strict = false;  // If true, error on non-envy-managed product script conflicts
    bool subproject = false;    // If true, use nearest manifest instead of root
    std::string platform_flag;  // "posix", "windows", "all", or empty (current OS)
    bool ignore_depot = false;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_sync(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
