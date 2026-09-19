#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

class cli_cmd;

class cmd_hash : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_hash> {
    std::vector<std::filesystem::path> paths;
    std::optional<std::string> prefix;

    // BLAKE3 over a whole directory: the digest vendoring stamps. `only` is the VENDOR
    // selector language; `json` reports the hash's own duration, not the process's.
    bool tree{ false };
    std::vector<std::string> only;
    int threads{ 0 };
    bool json{ false };

    // Per-stage timings and per-worker balance. Off by default: two clock reads per
    // file is free next to a syscall, not next to a tight hashing loop.
    bool stats{ false };
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_hash(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
