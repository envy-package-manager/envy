#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_run : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_run>, cmd_project_anchor {
    std::vector<std::string> command;
    bool activate{ false };  // the project's activation env, as `envy activate` loads it
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_run(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  void activate_process_env(std::filesystem::path const &start_dir) const;
  void prepend_bin_dir(std::filesystem::path const &start_dir, bool hint_sentinel) const;

  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
