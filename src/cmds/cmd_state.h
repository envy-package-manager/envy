#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

// `envy state`: each entry under the project's .envy/state, its size, and whether a
// package uses it.
class cmd_state : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_state>, cmd_project_anchor {};

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_state(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

// `envy state clean [query...]`: delete state dirs whatever their permissions. Queries
// resolve the manifest; bare `clean` reads the '@envy' header and empties .envy/state.
class cmd_state_clean : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_state_clean>, cmd_project_anchor {
    std::vector<std::string> queries;  // package, product or state entry names
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_state_clean(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  void clean_all() const;

  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
