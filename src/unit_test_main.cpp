#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "tui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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

// A pwsh test pays for a cold .NET start, and on an arm64 Windows runner that start is
// emulated. `fails on nonexistent command` is the worst of them: PowerShell answers an
// unresolved name by searching the whole PATH for suggestions. Observed exceeding 30s
// there while passing in seconds on x64, so the bound is per-class rather than global --
// a genuine hang in a pwsh test still aborts, just later.
constexpr std::chrono::seconds kPowershellTimeout{ 120 };

bool is_powershell_test(std::string_view name) {
  return name.find("powershell") != std::string_view::npos ||
         name.find("pwsh") != std::string_view::npos;
}

std::chrono::seconds timeout_for(std::string_view name, std::chrono::seconds fallback) {
  if (is_stress_test(name)) { return std::max(fallback, kStressTimeout); }
  if (is_powershell_test(name)) { return std::max(fallback, kPowershellTimeout); }
  return fallback;
}

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
    g_test_start = std::chrono::steady_clock::now();
    g_effective_timeout = timeout_for(g_current_test, g_default_timeout);
    g_test_running = true;
  }

  void test_case_end(doctest::CurrentTestCaseStats const &) override {
    g_test_running = false;
  }

  void report_query(doctest::QueryData const &) override {}
  void test_run_start() override {}
  void test_run_end(doctest::TestRunStats const &) override {}
  void test_case_reenter(doctest::TestCaseData const &) override {}
  void test_case_exception(doctest::TestCaseException const &) override {}
  void subcase_start(doctest::SubcaseSignature const &) override {}
  void subcase_end() override {}
  void log_assert(doctest::AssertData const &) override {}
  void log_message(doctest::MessageData const &) override {}
  void test_case_skipped(doctest::TestCaseData const &) override {}
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
