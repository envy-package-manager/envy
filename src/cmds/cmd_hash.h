#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_hash : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_hash> {
    std::vector<std::filesystem::path> paths;
    std::optional<std::string> prefix;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_hash(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
