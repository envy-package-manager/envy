#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>

namespace envy {

class cli_cmd;

class cmd_git_resolve : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_git_resolve> {
    std::string repo;
    std::string ref;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_git_resolve(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
