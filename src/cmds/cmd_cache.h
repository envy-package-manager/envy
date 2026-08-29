#pragma once

#include "cache.h"
#include "cmd.h"
#include "manifest.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace CLI { class App; }

namespace envy {

class cmd_cache : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_cache>, cmd_project_anchor {
    // REPORT is the usage table; PRINT_ROOT is the resolved root alone, with no scan, so
    // tests can compare it against what a launcher computed without paying for a walk.
    // PRINT_USER_WIDE_ROOT is the same oracle for the *other* root the launchers compute:
    // a local project looks there for an envy binary before downloading one, and a second
    // root resolved by four readers with nothing comparing them is how this whole area
    // went wrong the first time.
    enum class action { REPORT, PRINT_ROOT, PRINT_USER_WIDE_ROOT, SET_LOCAL, SET_SHARED };
    action act{ action::REPORT };
  };

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

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
