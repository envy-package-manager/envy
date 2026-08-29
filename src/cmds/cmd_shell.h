#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace CLI { class App; }

namespace envy {

class cmd_shell : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_shell>, cmd_project_anchor {
    std::string shell;  // "bash", "zsh", "fish", "powershell"
  };

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_shell(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
