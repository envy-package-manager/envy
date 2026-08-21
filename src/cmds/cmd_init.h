#pragma once

#include "cmd.h"
#include "luarc.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace CLI { class App; }

namespace envy {

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

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_init(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
