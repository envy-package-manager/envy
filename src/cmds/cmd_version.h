#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>

namespace envy {

class cli_cmd;

class cmd_version : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_version> {
    bool show_licenses{ false };
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_version(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
