#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace CLI { class App; }

namespace envy {

class cmd_extract : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_extract> {
    std::filesystem::path archive_path;
    std::filesystem::path destination;
    std::vector<std::string> only;  // Archive-relative paths/globs; empty = everything
  };

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_extract(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
