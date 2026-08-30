#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>

namespace envy {

class cli_cmd;

class cmd_package : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_package>, cmd_project_anchor {
    std::string identity;  // Required: "namespace.name@version"
    std::optional<std::filesystem::path> manifest_path;
    bool ignore_depot = false;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_package(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
