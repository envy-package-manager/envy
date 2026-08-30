#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>

namespace envy {

class cli_cmd;

class cmd_product : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_product>, cmd_project_anchor {
    std::string product_name;  // Optional: if empty, list all products
    std::optional<std::filesystem::path> manifest_path;
    bool json{ false };  // JSON output mode
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_product(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
