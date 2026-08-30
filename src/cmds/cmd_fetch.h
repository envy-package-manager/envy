#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>

namespace envy {

class cli_cmd;

class cmd_fetch : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_fetch> {
    std::string source;
    std::filesystem::path destination;
    std::optional<std::filesystem::path> manifest_root;
    std::optional<std::string> ref;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_fetch(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
