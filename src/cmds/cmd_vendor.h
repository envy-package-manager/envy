#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_vendor : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_vendor>, cmd_project_anchor {
    std::vector<std::string> queries;  // Which vendored packages; --all takes every one
    bool all = false;
    bool force = false;    // Repair even where vendor.auto_sync = false
    bool dry_run = false;  // Report the decision, write nothing
    int threads = 0;       // Copy workers; 0 = the performance-core count
    std::optional<std::filesystem::path> manifest_path;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_vendor(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
