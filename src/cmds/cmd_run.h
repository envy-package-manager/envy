#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_run : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_run>, cmd_project_anchor {
    std::vector<std::string> command;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_run(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
