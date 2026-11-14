#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "tui.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Terminate the process if any single test case runs longer than this - a hung
// test (deadlock, missed wakeup) must fail the build, not stall it forever.
// ENVY_TEST_TIMEOUT (seconds) overrides. The Windows default is looser: shell
// tests spawn pwsh, whose cold .NET start on CI runners can exceed 5 seconds.
std::chrono::seconds resolve_test_timeout() {
  if (char const *env{ std::getenv("ENVY_TEST_TIMEOUT") }) {
    if (int const v{ std::atoi(env) }; v > 0) { return std::chrono::seconds{ v }; }
  }
#ifdef _WIN32
  return std::chrono::seconds{ 30 };
#else
  return std::chrono::seconds{ 5 };
#endif
}

std::chrono::seconds g_default_timeout{ 5 };  // set in main before the watchdog starts

// Concurrency stress tests (named "... race - ..." / "... stress - ...") spawn
// hundreds of short-lived threads and hammer a shared mutex. Their wall-clock
// time balloons on slow / oversubscribed CI runners (measured ~4s at 6x
// oversubscription on a fast host) and trips the tight default. Give the whole
// class generous headroom; the watchdog still aborts a genuine hang, just at a
// higher bound. Matching by name keeps future stress tests covered for free.
constexpr std::chrono::seconds kStressTimeout{ 45 };

bool is_stress_test(std::string_view name) {
  return name.find("race - ") != std::string_view::npos ||
         name.find("stress - ") != std::string_view::npos;
}

std::chrono::seconds timeout_for(std::string_view name, std::chrono::seconds fallback) {
  return is_stress_test(name) ? std::max(fallback, kStressTimeout) : fallback;
}

// Cases at or over this are listed after the run: the suite is sub-second on POSIX, so a
// platform-only slowdown (pwsh's cold start on Windows) would otherwise hide in the total.
constexpr double kSlowTestSeconds{ 0.5 };
constexpr std::size_t kSlowTestsShown{ 12 };

std::mutex g_watchdog_mutex;
std::string g_current_test;                          // guarded by g_watchdog_mutex
std::chrono::steady_clock::time_point g_test_start;  // guarded by g_watchdog_mutex
std::chrono::seconds g_effective_timeout{ 5 };       // guarded by g_watchdog_mutex
std::atomic_bool g_test_running{ false };
std::atomic_bool g_watchdog_shutdown{ false };

struct watchdog_listener : doctest::IReporter {
  explicit watchdog_listener(doctest::ContextOptions const &) {}

  void test_case_start(doctest::TestCaseData const &data) override {
    std::lock_guard const lock(g_watchdog_mutex);
    g_current_test = data.m_name ? data.m_name : "<unnamed>";
    name_ = g_current_test;
    g_test_start = std::chrono::steady_clock::now();
    g_effective_timeout = timeout_for(g_current_test, g_default_timeout);
    g_test_running = true;
  }

  void test_case_end(doctest::CurrentTestCaseStats const &stats) override {
    g_test_running = false;
    if (stats.seconds >= kSlowTestSeconds) { slow_.emplace_back(stats.seconds, name_); }
  }

  void report_query(doctest::QueryData const &) override {}
  void test_run_start() override {}
  void test_run_end(doctest::TestRunStats const &) override {
    if (slow_.empty()) { return; }
    envy::util_sort(slow_, [](auto const &a, auto const &b) { return a.first > b.first; });
    std::printf("unit tests taking %.1fs or more (%zu):\n",
                kSlowTestSeconds,
                slow_.size());
    for (std::size_t i{ 0 }; i < std::min(slow_.size(), kSlowTestsShown); ++i) {
      std::printf("  %7.2fs  %s\n", slow_[i].first, slow_[i].second.c_str());
    }
    std::fflush(stdout);
  }
  void test_case_reenter(doctest::TestCaseData const &) override {}
  void test_case_exception(doctest::TestCaseException const &) override {}
  void subcase_start(doctest::SubcaseSignature const &) override {}
  void subcase_end() override {}
  void log_assert(doctest::AssertData const &) override {}
  void log_message(doctest::MessageData const &) override {}
  void test_case_skipped(doctest::TestCaseData const &) override {}

 private:
  std::string name_;
  std::vector<std::pair<double, std::string>> slow_;
};

void watchdog_thread_main() {
  while (!g_watchdog_shutdown) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!g_test_running) { continue; }
    std::lock_guard const lock(g_watchdog_mutex);
    if (g_test_running &&
        std::chrono::steady_clock::now() - g_test_start > g_effective_timeout) {
      std::fprintf(stderr,
                   "\nwatchdog: test case '%s' exceeded %lld seconds; aborting\n",
                   g_current_test.c_str(),
                   static_cast<long long>(g_effective_timeout.count()));
      std::fflush(stderr);
      std::abort();
    }
  }
}

}  // namespace

REGISTER_LISTENER("watchdog", 0, watchdog_listener);

int main(int argc, char **argv) {
  doctest::Context context;
  context.applyCommandLine(argc, argv);

  envy::tui::init();
  envy::tui::set_output_handler([](std::string_view) {});
  envy::tui::test::g_isatty = false;

  // The watchdog runs as a background thread. Skip it when requested (set by the
  // valgrind CI step): a running thread makes fork() in the shell tests misbehave
  // under valgrind, and a 5s wall-clock limit is meaningless under its ~30x slowdown.
  bool const watchdog_enabled{ std::getenv("ENVY_TEST_NO_WATCHDOG") == nullptr };

  std::thread watchdog;
  if (watchdog_enabled) {
    g_default_timeout = resolve_test_timeout();
    watchdog = std::thread{ watchdog_thread_main };
  }
  int const rc{ context.run() };
  if (watchdog.joinable()) {
    g_watchdog_shutdown = true;
    watchdog.join();
  }
  return rc;
}
