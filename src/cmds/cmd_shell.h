#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>

namespace envy {

class cli_cmd;

class cmd_shell : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_shell>, cmd_project_anchor {
    std::string shell;  // "bash", "zsh", "fish", "powershell"
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_shell(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
