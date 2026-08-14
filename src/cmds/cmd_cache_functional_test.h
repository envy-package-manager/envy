#pragma once

#include "cmd.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace CLI { class App; }

namespace envy {

// Functional-tester-only: drive cache::ensure_pkg / ensure_spec directly so tests
// can choreograph two processes around a single lock. What happened is reported
// through the normal trace stream (cache_hit, cache_miss, lock_acquired,
// cache_entry_finalized); these commands print nothing.
struct cache_test_cfg {
  std::string test_id;
  std::filesystem::path barrier_dir;  // empty = temp_dir/envy-barrier-<test_id>
  std::string barrier_signal;         // empty = no barrier
  std::string barrier_wait;           // empty = no barrier
  std::string barrier_signal_after;   // signal after lock acquired
  std::string barrier_wait_after;     // wait after lock acquired
  int crash_after_ms = -1;            // -1 = no crash
  bool fail_before_complete = false;
};

class cmd_cache_ensure_package : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_cache_ensure_package>, cache_test_cfg {
    std::string identity;
    std::string platform;
    std::string arch;
    std::string hash_prefix;
  };

  static void register_cli(CLI::App &parent, std::function<void(cfg)> on_selected);

  cmd_cache_ensure_package(cfg const &config,
                           std::optional<std::filesystem::path> const &cli_cache_root);
  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

class cmd_cache_ensure_spec : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_cache_ensure_spec>, cache_test_cfg {
    std::string identity;
    std::string source;  // empty = key on the identity (one entry per identity)
  };

  static void register_cli(CLI::App &parent, std::function<void(cfg)> on_selected);

  cmd_cache_ensure_spec(cfg const &config,
                        std::optional<std::filesystem::path> const &cli_cache_root);
  void execute() override;

 private:
  cfg cfg_;
  std::optional<std::filesystem::path> cli_cache_root_;
};

}  // namespace envy
