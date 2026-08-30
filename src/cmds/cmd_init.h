#pragma once

#include "cmd.h"
#include "luarc.h"

#include <filesystem>
#include <optional>
#include <string>

namespace envy {

class cli_cmd;

class cmd_init : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_init> {
    std::filesystem::path project_dir;
    std::filesystem::path bin_dir;
    std::optional<std::string> mirror;
    std::optional<std::string> envy_version;  // re-exec into this envy to run the init
    std::optional<bool> deploy{ true };       // @envy deploy directive value
    std::optional<bool> root{ true };         // @envy root directive value
    bool pin_sums{ false };     // fetch this release's SHA256SUMS and pin its hash
    std::string platform_flag;  // "posix", "windows", "all", or empty (current OS)
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_init(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
