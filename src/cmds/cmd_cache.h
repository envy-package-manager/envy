#pragma once

#include "cache.h"
#include "cmd.h"
#include "manifest.h"

#include <filesystem>
#include <optional>

namespace envy {

class cli_cmd;

class cmd_cache : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_cache>, cmd_project_anchor {
    // REPORT is the usage table; PRINT_ROOT and PRINT_USER_WIDE_ROOT are launcher-parity
    // oracles for the two roots, printed alone so a test pays for no disk walk.
    enum class action { REPORT, PRINT_ROOT, PRINT_USER_WIDE_ROOT, SET_LOCAL, SET_SHARED };
    action act{ action::REPORT };
    // Parse scratch, one per mutually exclusive flag; folded into `act` once argv parses.
    bool want_root{}, want_user_wide{}, want_local{}, want_shared{};
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_cache(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  // Records the user's mode choice as a marker under the state dir, or clears both markers
  // when the choice already matches what the manifest declares -- a redundant marker would
  // be one more thing to explain and one more thing to go stale.
  void set_mode(envy_meta const &meta,
                std::filesystem::path const &manifest_dir,
                cache_mode requested);

  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
