#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace CLI { class App; }

namespace envy {

class cmd_cache : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_cache>, cmd_project_anchor {};

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_cache(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
