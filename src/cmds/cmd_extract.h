#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_extract : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_extract> {
    std::filesystem::path archive_path;
    std::filesystem::path destination;
    std::vector<std::string> only;  // Archive-relative paths/globs; empty = everything
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_extract(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
