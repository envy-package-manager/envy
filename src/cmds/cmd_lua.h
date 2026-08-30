#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>

namespace envy {

class cli_cmd;

class cmd_lua : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_lua> {
    std::filesystem::path script_path;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_lua(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
