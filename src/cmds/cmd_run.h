#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace CLI { class App; }

namespace envy {

class cmd_run : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_run>, cmd_project_anchor {
    std::vector<std::string> command;
  };

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_run(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
