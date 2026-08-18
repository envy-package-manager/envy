#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace CLI { class App; }

namespace envy {

// Retarget '@envy version'; `sums_hex` replaces or inserts the '@envy sha256sums' pin, nullopt
// drops it. Splices in place, preserving formatting. Throws with no '@envy version' to edit.
std::string use_rewrite_header(std::string_view content,
                               std::string_view version,
                               std::optional<std::string> const &sums_hex);

class cmd_use : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_use> {
    std::string version;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::string> mirror;
    bool subproject{ false };
    bool pin_sums{ false };
    bool no_pin_sums{ false };
    bool force{ false };
  };

  static void register_cli(CLI::App &app, std::function<void(cfg)> on_selected);

  cmd_use(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
