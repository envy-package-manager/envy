#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace CLI { class App; }

namespace envy {

class cmd_import : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_import>, cmd_project_anchor {
    std::filesystem::path archive_path;
    std::optional<std::filesystem::path> dir;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> checksums_path;
  };

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_import(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
