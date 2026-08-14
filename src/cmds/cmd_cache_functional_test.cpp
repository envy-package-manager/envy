#include "cmd_cache_functional_test.h"

#include "cache.h"
#include "platform.h"

#include "CLI11.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace envy {

namespace {

// File-based rendezvous between cooperating test processes.
class test_barrier {
 public:
  explicit test_barrier(std::filesystem::path const &barrier_dir)
      : barrier_dir_{ barrier_dir } {
    std::filesystem::create_directories(barrier_dir_);
  }

  void signal(std::string const &name) {
    if (name.empty()) { return; }
    platform::touch_file(barrier_dir_ / name);
  }

  void wait(std::string const &name) {
    if (name.empty()) { return; }
    std::filesystem::path const marker{ barrier_dir_ / name };
    while (!std::filesystem::exists(marker)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

 private:
  std::filesystem::path barrier_dir_;
};

void add_cache_test_options(CLI::App &sub, cache_test_cfg &cfg) {
  sub.add_option("--test-id", cfg.test_id, "Test ID for barrier isolation");
  sub.add_option("--barrier-dir", cfg.barrier_dir, "Barrier directory");
  sub.add_option("--barrier-signal", cfg.barrier_signal, "Barrier to signal before lock");
  sub.add_option("--barrier-wait", cfg.barrier_wait, "Barrier to wait for before lock");
  sub.add_option("--barrier-signal-after",
                 cfg.barrier_signal_after,
                 "Barrier to signal after lock");
  sub.add_option("--barrier-wait-after",
                 cfg.barrier_wait_after,
                 "Barrier to wait for after lock");
  sub.add_option("--crash-after", cfg.crash_after_ms, "Crash after N milliseconds");
  sub.add_flag("--fail-before-complete",
               cfg.fail_before_complete,
               "Exit without marking complete");
}

// The two ensure_* variants differ only in which entry they take, so the
// barrier / crash / completion choreography lives here once.
template <typename EnsureFn>
void run_ensure(cache_test_cfg const &cfg, EnsureFn &&ensure) {
  auto const default_dir{ std::filesystem::temp_directory_path() /
                          ("envy-barrier-" + cfg.test_id) };
  test_barrier barrier{ cfg.barrier_dir.empty() ? default_dir : cfg.barrier_dir };

  barrier.signal(cfg.barrier_signal);
  barrier.wait(cfg.barrier_wait);

  auto result{ ensure() };

  barrier.signal(cfg.barrier_signal_after);
  barrier.wait(cfg.barrier_wait_after);

  if (cfg.crash_after_ms >= 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.crash_after_ms));
    platform::terminate_process();
  }

  // Unwinding still runs the lock destructor, which traces the entry as
  // cleaned_failure / kept_partial rather than completed.
  if (cfg.fail_before_complete) {
    throw std::runtime_error("cache-test: fail_before_complete requested");
  }

  if (result.lock) { result.lock->mark_install_complete(); }
}

}  // namespace

void cmd_cache_ensure_package::register_cli(CLI::App &parent,
                                            std::function<void(cfg)> on_selected) {
  auto *sub{ parent.add_subcommand("ensure-package", "Test package cache entry") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("identity", cfg_ptr->identity, "Package identity")->required();
  sub->add_option("platform", cfg_ptr->platform, "Platform (darwin/linux/windows)")
      ->required();
  sub->add_option("arch", cfg_ptr->arch, "Architecture (arm64/x86_64)")->required();
  sub->add_option("hash_prefix", cfg_ptr->hash_prefix, "Hash prefix")->required();
  add_cache_test_options(*sub, *cfg_ptr);
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

void cmd_cache_ensure_spec::register_cli(CLI::App &parent,
                                         std::function<void(cfg)> on_selected) {
  auto *sub{ parent.add_subcommand("ensure-spec", "Test spec cache entry") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("identity", cfg_ptr->identity, "Spec identity")->required();
  sub->add_option("--source", cfg_ptr->source, "Source key (default: the identity)");
  add_cache_test_options(*sub, *cfg_ptr);
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_cache_ensure_package::cmd_cache_ensure_package(
    cfg const &config,
    std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ config }, cli_cache_root_{ cli_cache_root } {}

void cmd_cache_ensure_package::execute() {
  cache c{ cli_cache_root_ };
  run_ensure(cfg_, [&] {
    return c.ensure_pkg(cfg_.identity, cfg_.platform, cfg_.arch, cfg_.hash_prefix);
  });
}

cmd_cache_ensure_spec::cmd_cache_ensure_spec(
    cfg const &config,
    std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ config }, cli_cache_root_{ cli_cache_root } {}

void cmd_cache_ensure_spec::execute() {
  cache c{ cli_cache_root_ };
  run_ensure(cfg_, [&] {
    return c.ensure_spec(cfg_.identity,
                         cfg_.source.empty() ? cfg_.identity : cfg_.source);
  });
}

}  // namespace envy
