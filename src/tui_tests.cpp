#include "tui.h"

#include "doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

TEST_CASE("tui init can only run once") {
  CHECK_THROWS_AS(envy::tui::init(), std::logic_error);
}

TEST_CASE("tui allows handler changes while idle") {
  CHECK_NOTHROW(envy::tui::set_output_handler([](std::string_view) {}));
  CHECK_NOTHROW(envy::tui::set_output_handler([](std::string_view) {}));
}

TEST_CASE("tui enforces run/shutdown sequencing") {
  auto const handler{ [](std::string_view) {} };
  CHECK_NOTHROW(envy::tui::set_output_handler(handler));
  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_INFO));
  CHECK_NOTHROW(envy::tui::shutdown());

  CHECK_NOTHROW(envy::tui::run(std::nullopt));
  CHECK_THROWS_AS(envy::tui::set_output_handler(handler), std::logic_error);
  CHECK_THROWS_AS(envy::tui::run(std::nullopt), std::logic_error);

  CHECK_NOTHROW(envy::tui::shutdown());
  CHECK_THROWS_AS(envy::tui::shutdown(), std::logic_error);

  CHECK_NOTHROW(envy::tui::set_output_handler(handler));
}

namespace {

struct captured_output {
  std::vector<std::string> messages;

  captured_output() {
    envy::tui::set_output_handler(
        [this](std::string_view value) { messages.emplace_back(value); });
  }

  ~captured_output() {
    try {
      envy::tui::set_output_handler([](std::string_view) {});
    } catch (std::logic_error const &error) {
      FAIL("set_output_handler should not throw during teardown: " << error.what());
    }
  }
};

}  // namespace

TEST_CASE_FIXTURE(captured_output,
                  "tui undecorated logs: info/debug raw, warn/error tagged") {
  REQUIRE(messages.empty());

  CHECK_NOTHROW(envy::tui::run(std::nullopt));

  envy::tui::debug("hello %s", "world");
  envy::tui::info("value %d", 42);
  envy::tui::warn("three %d", 3);
  envy::tui::error("boom");

  CHECK_NOTHROW(envy::tui::shutdown());

  REQUIRE(messages.size() == 4);
  CHECK(messages[0] == "hello world\n");
  CHECK(messages[1] == "value 42\n");
  // Undecorated warn/error are marked compiler-style so they stand out.
  CHECK(messages[2] == "warning: three 3\n");
  CHECK(messages[3] == "error: boom\n");
}

TEST_CASE_FIXTURE(captured_output, "tui structured logs include prefix") {
  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_DEBUG, true));
  envy::tui::info("structured %d", 7);
  CHECK_NOTHROW(envy::tui::shutdown());

  REQUIRE(messages.size() == 1);
  auto const &line{ messages[0] };
  CHECK(line.find("[INF") != std::string::npos);
  CHECK(line.rfind("structured 7\n") ==
        line.size() - std::string("structured 7\n").size());
}

TEST_CASE_FIXTURE(captured_output, "tui severity filtering honors threshold") {
  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_WARN, true));
  envy::tui::debug("debug");
  envy::tui::info("info");
  envy::tui::warn("warn");
  envy::tui::error("error");
  CHECK_NOTHROW(envy::tui::shutdown());

  REQUIRE(messages.size() == 2);
  CHECK(messages[0].find("WRN") != std::string::npos);
  CHECK(messages[0].find("warn") != std::string::npos);
  CHECK(messages[1].find("ERR") != std::string::npos);
  CHECK(messages[1].find("error") != std::string::npos);

  messages.clear();
  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_INFO, true));
  envy::tui::debug("debug");
  envy::tui::info("info");
  CHECK_NOTHROW(envy::tui::shutdown());
  REQUIRE(messages.size() == 1);
  CHECK(messages[0].find("INF") != std::string::npos);
  CHECK(messages[0].find("info") != std::string::npos);
}

TEST_CASE_FIXTURE(captured_output, "tui trace events reach handler") {
  envy::tui::configure_trace_outputs(
      { { envy::tui::trace_output_type::std_err, std::nullopt } });
  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_DEBUG, false));

  envy::tui::trace(
      "demo.spec@v1",
      envy::trace_events::phase_start{ .phase = envy::pkg_phase::spec_fetch });

  CHECK_NOTHROW(envy::tui::shutdown());
  REQUIRE_FALSE(messages.empty());
  // configure_trace_outputs emits the trace_start header record first.
  CHECK(messages[0].find("trace_start") != std::string::npos);
  bool found{ false };
  for (auto const &msg : messages) {
    if (msg.find("phase_start") != std::string::npos &&
        msg.find("spec=demo.spec@v1") != std::string::npos) {
      found = true;
    }
  }
  CHECK(found);

  envy::tui::configure_trace_outputs({});
}

TEST_CASE("g_trace_enabled controls trace event processing") {
  // Initially disabled (no trace outputs configured)
  CHECK_FALSE(envy::tui::g_trace_enabled);

  // Enable stderr trace
  envy::tui::configure_trace_outputs(
      { { envy::tui::trace_output_type::std_err, std::nullopt } });
  CHECK(envy::tui::g_trace_enabled);

  // Disable trace
  envy::tui::configure_trace_outputs({});
  CHECK_FALSE(envy::tui::g_trace_enabled);

  // Enable file trace
  envy::tui::configure_trace_outputs(
      { { envy::tui::trace_output_type::file,
          std::filesystem::temp_directory_path() / "test_trace.jsonl" } });
  CHECK(envy::tui::g_trace_enabled);

  // Disable again
  envy::tui::configure_trace_outputs({});
  CHECK_FALSE(envy::tui::g_trace_enabled);

  // Enable both
  envy::tui::configure_trace_outputs(
      { { envy::tui::trace_output_type::std_err, std::nullopt },
        { envy::tui::trace_output_type::file,
          std::filesystem::temp_directory_path() / "test_trace2.jsonl" } });
  CHECK(envy::tui::g_trace_enabled);

  // Cleanup
  envy::tui::configure_trace_outputs({});
}

TEST_CASE("ENVY_TRACE respects g_trace_enabled") {
  envy::tui::configure_trace_outputs({});
  CHECK_FALSE(envy::tui::g_trace_enabled);

  // These should not crash even when trace is disabled
  ENVY_TRACE(phase_blocked,
             "r1",
             .blocked_at_phase = envy::pkg_phase::pkg_check,
             .waiting_for = "dep",
             .target_phase = envy::pkg_phase::completion);
  ENVY_TRACE(dependency_added,
             "parent",
             .dependency = "child",
             .needed_by = envy::pkg_phase::pkg_fetch);

  // Enable trace and verify events can be emitted
  envy::tui::configure_trace_outputs(
      { { envy::tui::trace_output_type::std_err, std::nullopt } });
  CHECK(envy::tui::g_trace_enabled);

  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_DEBUG, false));
  ENVY_TRACE(phase_start, "test", .phase = envy::pkg_phase::spec_fetch);
  CHECK_NOTHROW(envy::tui::shutdown());

  envy::tui::configure_trace_outputs({});
}

TEST_CASE("trace file output writes JSONL format") {
  auto const trace_path{ std::filesystem::temp_directory_path() /
                         "envy_test_trace.jsonl" };

  // Clean up any existing file
  std::error_code ec;
  std::filesystem::remove(trace_path, ec);

  envy::tui::configure_trace_outputs(
      { { envy::tui::trace_output_type::file, trace_path } });
  CHECK(envy::tui::g_trace_enabled);

  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_DEBUG, false));

  // Emit various trace events
  envy::tui::trace(
      "test@v1",
      envy::trace_events::phase_start{ .phase = envy::pkg_phase::spec_fetch });
  envy::tui::trace(
      "parent@v1",
      envy::trace_events::dependency_added{ .dependency = "child@v2",
                                            .needed_by = envy::pkg_phase::pkg_fetch });
  envy::tui::trace(
      "test@v1",
      envy::trace_events::cache_hit{ .cache_key = "test-key", .pkg_path = "/cache/test" });

  CHECK_NOTHROW(envy::tui::shutdown());

  // Verify file exists and contains JSON
  REQUIRE(std::filesystem::exists(trace_path));

  std::ifstream file{ trace_path };
  REQUIRE(file.is_open());

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty()) { lines.push_back(line); }
  }
  file.close();

  // Header record + 3 events
  REQUIRE(lines.size() >= 4);
  CHECK(lines[0].find("\"event\":\"trace_start\"") != std::string::npos);
  CHECK(lines[0].find("\"schema\":") != std::string::npos);

  // Each line is a JSON object with envelope keys; seq strictly increasing
  long last_seq{ -1 };
  for (auto const &json_line : lines) {
    CHECK(json_line.find("\"seq\":") != std::string::npos);
    CHECK(json_line.find("\"ts\":") != std::string::npos);
    CHECK(json_line.find("\"tid\":") != std::string::npos);
    CHECK(json_line.find("\"event\":") != std::string::npos);
    CHECK(json_line[0] == '{');
    CHECK(json_line[json_line.size() - 1] == '}');
    auto const seq_pos{ json_line.find("\"seq\":") + 6 };
    long const seq{ std::stol(json_line.substr(seq_pos)) };
    CHECK(seq > last_seq);
    last_seq = seq;
  }

  // Verify specific events
  bool found_phase_start{ false };
  bool found_dependency_added{ false };
  bool found_cache_hit{ false };

  for (auto const &json_line : lines) {
    if (json_line.find("\"event\":\"phase_start\"") != std::string::npos &&
        json_line.find("\"spec\":\"test@v1\"") != std::string::npos) {
      found_phase_start = true;
    }
    if (json_line.find("\"event\":\"dependency_added\"") != std::string::npos &&
        json_line.find("\"spec\":\"parent@v1\"") != std::string::npos) {
      found_dependency_added = true;
    }
    if (json_line.find("\"event\":\"cache_hit\"") != std::string::npos &&
        json_line.find("\"cache_key\":\"test-key\"") != std::string::npos) {
      found_cache_hit = true;
    }
  }

  CHECK(found_phase_start);
  CHECK(found_dependency_added);
  CHECK(found_cache_hit);

  // Cleanup
  std::filesystem::remove(trace_path, ec);
  envy::tui::configure_trace_outputs({});
}

TEST_CASE_FIXTURE(captured_output, "trace multiple outputs simultaneously") {
  auto const trace_path{ std::filesystem::temp_directory_path() /
                         "envy_test_multi_trace.jsonl" };

  // Clean up any existing file
  std::error_code ec;
  std::filesystem::remove(trace_path, ec);

  // Configure both stderr and file output
  envy::tui::configure_trace_outputs(
      { { envy::tui::trace_output_type::std_err, std::nullopt },
        { envy::tui::trace_output_type::file, trace_path } });
  CHECK(envy::tui::g_trace_enabled);

  CHECK_NOTHROW(envy::tui::run(envy::tui::level::TUI_DEBUG, false));

  // Emit trace event
  envy::tui::trace("multi@v1",
                   envy::trace_events::phase_complete{ .phase = envy::pkg_phase::pkg_build,
                                                       .duration_ms = 123 });

  CHECK_NOTHROW(envy::tui::shutdown());

  // Verify stderr output (human-readable)
  REQUIRE_FALSE(messages.empty());
  bool found_stderr{ false };
  for (auto const &msg : messages) {
    if (msg.find("phase_complete") != std::string::npos &&
        msg.find("spec=multi@v1") != std::string::npos) {
      found_stderr = true;
      break;
    }
  }
  CHECK(found_stderr);

  // Verify file output (JSON)
  REQUIRE(std::filesystem::exists(trace_path));

  std::ifstream file{ trace_path };
  REQUIRE(file.is_open());

  bool found_file{ false };
  std::string line;
  while (std::getline(file, line)) {
    if (line.find("\"event\":\"phase_complete\"") != std::string::npos &&
        line.find("\"spec\":\"multi@v1\"") != std::string::npos &&
        line.find("\"duration_ms\":123") != std::string::npos) {
      found_file = true;
      break;
    }
  }
  file.close();

  CHECK(found_file);

  // Cleanup
  std::filesystem::remove(trace_path, ec);
  envy::tui::configure_trace_outputs({});
}

TEST_CASE("configure_trace_outputs rejects multiple file outputs") {
  auto const path1{ std::filesystem::temp_directory_path() / "trace1.jsonl" };
  auto const path2{ std::filesystem::temp_directory_path() / "trace2.jsonl" };

  CHECK_THROWS_AS(envy::tui::configure_trace_outputs(
                      { { envy::tui::trace_output_type::file, path1 },
                        { envy::tui::trace_output_type::file, path2 } }),
                  std::logic_error);

  // Ensure cleanup in case of test failure
  envy::tui::configure_trace_outputs({});
}

// Progress section tests

#ifdef ENVY_UNIT_TEST

TEST_CASE("progress section line counting with text stream") {
  envy::tui::test::g_terminal_width = 80;
  envy::tui::test::g_isatty = true;
  auto const now{ std::chrono::steady_clock::now() };
  envy::tui::test::g_now = now;

  envy::tui::section_frame const frame{ .label = "pkg@v1",
                                        .content = envy::tui::text_stream_data{
                                            .lines = { "line1", "line2", "line3" },
                                            .start_time = now } };

  std::string const output{ envy::tui::test::render_section_frame(frame) };

  // Should have: 1 header line + 3 content lines = 4 lines total
  int const line_count{ static_cast<int>(std::count(output.begin(), output.end(), '\n')) };
  CHECK(line_count == 4);
}

TEST_CASE("grouped render ansi") {
  envy::tui::test::g_terminal_width = 80;
  envy::tui::test::g_isatty = true;
  auto const now{ std::chrono::steady_clock::now() };
  envy::tui::test::g_now = now;

  envy::tui::section_frame parent{
    .label = "pkg",
    .content = envy::tui::progress_data{ .percent = 50.0, .status = "fetch" },
    .phase_label = ""
  };
  parent.children.push_back(envy::tui::section_frame{
      .label = "ninja.git",
      .content = envy::tui::progress_data{ .percent = 20.0, .status = "20%" } });
  parent.children.push_back(envy::tui::section_frame{
      .label = "googletest.git",
      .content = envy::tui::progress_data{ .percent = 80.0, .status = "80%" } });

  std::string const output{ envy::tui::test::render_section_frame(parent) };

  CHECK(output.find("pkg") != std::string::npos);
  CHECK(output.find("fetch") != std::string::npos);
  CHECK(output.find("  ninja.git") != std::string::npos);
  CHECK(output.find("  googletest.git") != std::string::npos);
}

TEST_CASE("grouped render fallback") {
  envy::tui::test::g_terminal_width = 80;
  envy::tui::test::g_isatty = false;
  auto const now{ std::chrono::steady_clock::now() };
  envy::tui::test::g_now = now;

  envy::tui::section_frame parent{
    .label = "pkg",
    .content = envy::tui::progress_data{ .percent = 50.0, .status = "fetch" },
    .phase_label = ""
  };
  parent.children.push_back(envy::tui::section_frame{
      .label = "ninja.git",
      .content = envy::tui::progress_data{ .percent = 20.0, .status = "20%" } });

  std::string const output{ envy::tui::test::render_section_frame(parent) };

  CHECK(output.find("pkg") != std::string::npos);
  CHECK(output.find("fetch") != std::string::npos);
  CHECK(output.find("  ninja.git") != std::string::npos);
}

TEST_CASE("interactive_mode_guard RAII") {
  {
    envy::tui::interactive_mode_guard guard;
    // Mutex is locked, rendering paused
  }
  // Mutex is unlocked, rendering resumed (destructor ran)

  // If we can create another guard without deadlock, RAII worked
  { envy::tui::interactive_mode_guard guard2; }
}

TEST_CASE("interactive_mode_guard exception safety") {
  bool exception_thrown{ false };

  try {
    envy::tui::interactive_mode_guard guard;
    exception_thrown = true;
    throw std::runtime_error{ "test exception" };
  } catch (std::runtime_error const &) {}

  CHECK(exception_thrown);

  // If we can acquire the guard again, the mutex was properly released
  { envy::tui::interactive_mode_guard guard2; }
}

TEST_CASE("serialized interactive mode") {
  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  std::atomic<int> counter{ 0 };
  bool t1_acquired{ false };
  bool t2_attempting{ false };

  std::thread t1{ [&]() {
    envy::tui::interactive_mode_guard guard;
    counter++;

    {  // Signal t2 that we've acquired
      std::lock_guard lock{ sync_mutex };
      t1_acquired = true;
    }
    sync_cv.notify_one();

    {  // Wait for t2 to signal it's attempting to acquire
      std::unique_lock lock{ sync_mutex };
      sync_cv.wait(lock, [&] { return t2_attempting; });
    }

    // t2 is now blocked on acquire - verify counter hasn't incremented
    CHECK(counter == 1);

    // Release guard (t2 will unblock)
  } };

  std::thread t2{ [&]() {
    {  // Wait for t1 to acquire
      std::unique_lock lock{ sync_mutex };
      sync_cv.wait(lock, [&] { return t1_acquired; });
    }

    {  // Signal that we're about to attempt acquire (will block)
      std::lock_guard lock{ sync_mutex };
      t2_attempting = true;
    }
    sync_cv.notify_one();

    // This blocks until t1 releases
    envy::tui::interactive_mode_guard guard;
    counter++;
    CHECK(counter == 2);
  } };

  t1.join();
  t2.join();

  CHECK(counter == 2);
}

// ============================================================================
// ANSI-aware visible length and padding tests
// ============================================================================

TEST_CASE("calculate_visible_length - plain text") {
  CHECK(envy::tui::test::calculate_visible_length("") == 0);
  CHECK(envy::tui::test::calculate_visible_length("a") == 1);
  CHECK(envy::tui::test::calculate_visible_length("hello") == 5);
  CHECK(envy::tui::test::calculate_visible_length("hello world") == 11);
  CHECK(envy::tui::test::calculate_visible_length("123456789") == 9);
}

TEST_CASE("calculate_visible_length - single ANSI escape") {
  // Red text: ESC[31m
  CHECK(envy::tui::test::calculate_visible_length("\x1b[31m") == 0);
  CHECK(envy::tui::test::calculate_visible_length("\x1b[31mred") == 3);
  CHECK(envy::tui::test::calculate_visible_length("text\x1b[0m") == 4);
  CHECK(envy::tui::test::calculate_visible_length("\x1b[31mred\x1b[0m") == 3);
}

TEST_CASE("calculate_visible_length - multiple ANSI escapes") {
  // Bold red: ESC[1;31m
  CHECK(envy::tui::test::calculate_visible_length("\x1b[1;31mbold red\x1b[0m") == 8);

  // Multiple colors
  CHECK(envy::tui::test::calculate_visible_length("\x1b[31mred\x1b[32mgreen\x1b[0m") == 8);

  // Complex formatting
  std::string complex = "\x1b[1m\x1b[31mBold Red\x1b[0m Normal \x1b[32mGreen\x1b[0m";
  CHECK(envy::tui::test::calculate_visible_length(complex) ==
        21);  // "Bold Red Normal Green"
}

TEST_CASE("calculate_visible_length - only ANSI codes") {
  CHECK(envy::tui::test::calculate_visible_length("\x1b[31m\x1b[0m") == 0);
  CHECK(envy::tui::test::calculate_visible_length("\x1b[1;31;42m") == 0);
}

TEST_CASE("calculate_visible_length - mixed content") {
  // Progress bar with colors: [[package]] 50%
  std::string colored = "\x1b[1m[[package]]\x1b[0m \x1b[32m50%\x1b[0m";
  CHECK(envy::tui::test::calculate_visible_length(colored) == 15);  // "[[package]] 50%"

  // Real-world TUI output
  std::string progress = "[[arm.gcc@v2]] \x1b[32mBuilding...\x1b[0m [=====>    ] 50.0%";
  CHECK(envy::tui::test::calculate_visible_length(progress) == 45);
}

TEST_CASE("calculate_visible_length - incomplete ANSI sequence") {
  // Incomplete sequences stay in escape mode (waiting for 'm' terminator)
  CHECK(envy::tui::test::calculate_visible_length("\x1b[") ==
        0);  // Started but not finished
  CHECK(envy::tui::test::calculate_visible_length("\x1b[31") ==
        0);  // Still in escape, no 'm' yet
  CHECK(envy::tui::test::calculate_visible_length("text\x1b[") ==
        4);  // "text" visible, then incomplete escape
}

TEST_CASE("calculate_visible_length - ESC without bracket") {
  // ESC character without [ should count ESC as visible
  CHECK(envy::tui::test::calculate_visible_length("\x1b"
                                                  "text") == 5);  // ESC + "text"
  CHECK(envy::tui::test::calculate_visible_length("a\x1b"
                                                  "b") == 3);  // "a" + ESC + "b"
}

TEST_CASE("calculate_visible_length - unicode and special chars") {
  // Regular ASCII punctuation and symbols
  CHECK(envy::tui::test::calculate_visible_length("[](){}<>") == 8);
  CHECK(envy::tui::test::calculate_visible_length("!@#$%^&*") == 8);
  CHECK(envy::tui::test::calculate_visible_length("  spaces  ") == 10);
  CHECK(envy::tui::test::calculate_visible_length("\t\ttabs\t") ==
        28);  // tabs count as 8 columns: 8+8+4+8=28
}

TEST_CASE("pad_to_width - plain text shorter than width") {
  CHECK(envy::tui::test::pad_to_width("hello", 10) == "hello     ");
  CHECK(envy::tui::test::pad_to_width("a", 5) == "a    ");
  CHECK(envy::tui::test::pad_to_width("", 3) == "   ");
}

TEST_CASE("pad_to_width - plain text equal to width") {
  CHECK(envy::tui::test::pad_to_width("hello", 5) == "hello");
  CHECK(envy::tui::test::pad_to_width("exact", 5) == "exact");
}

TEST_CASE("pad_to_width - plain text longer than width") {
  // Now truncates to fit within width
  CHECK(envy::tui::test::pad_to_width("hello world", 5) == "hello");
  CHECK(envy::tui::test::pad_to_width("toolong", 3) == "too");
}

TEST_CASE("pad_to_width - width zero and negative") {
  // Zero or negative width truncates to empty (nothing fits)
  CHECK(envy::tui::test::pad_to_width("text", 0) == "");
  CHECK(envy::tui::test::pad_to_width("text", -5) == "");
  CHECK(envy::tui::test::pad_to_width("", 0) == "");
}

TEST_CASE("pad_to_width - ANSI colored text") {
  std::string red = "\x1b[31mred\x1b[0m";
  std::string padded = envy::tui::test::pad_to_width(red, 10);

  // Should be: "\x1b[31mred\x1b[0m       " (3 visible chars + 7 spaces)
  CHECK(padded.size() == red.size() + 7);
  CHECK(envy::tui::test::calculate_visible_length(padded) == 10);
  CHECK(padded.starts_with("\x1b[31mred\x1b[0m"));
  CHECK(padded.ends_with("       "));
}

TEST_CASE("pad_to_width - multiple ANSI sequences") {
  std::string multicolor = "\x1b[31mred\x1b[32mgreen\x1b[0m";  // visible: "redgreen" = 8
  std::string padded = envy::tui::test::pad_to_width(multicolor, 12);

  CHECK(envy::tui::test::calculate_visible_length(padded) == 12);
  CHECK(padded.starts_with(multicolor));
  CHECK(padded.ends_with("    "));  // 4 spaces
}

TEST_CASE("pad_to_width - complex formatting") {
  // Bold red text: ESC[1;31m ... ESC[0m
  std::string formatted = "\x1b[1;31mBold\x1b[0m";  // visible: "Bold" = 4
  std::string padded = envy::tui::test::pad_to_width(formatted, 10);

  CHECK(envy::tui::test::calculate_visible_length(padded) == 10);
  CHECK(padded.starts_with(formatted));
}

TEST_CASE("pad_to_width - only ANSI codes") {
  std::string only_ansi = "\x1b[31m\x1b[0m";  // visible: 0
  std::string padded = envy::tui::test::pad_to_width(only_ansi, 5);

  CHECK(envy::tui::test::calculate_visible_length(padded) == 5);
  CHECK(padded == only_ansi + "     ");
}

TEST_CASE("pad_to_width - real-world progress bar") {
  // Simulate: [[package]] Building... [=====>    ] 50.0%
  std::string bar = "\x1b[1m[[pkg]]\x1b[0m Build [==>  ] \x1b[32m50%\x1b[0m";
  // Visible: "[[pkg]] Build [==>  ] 50%" = 25 chars (7 + 15 + 3)

  int visible = envy::tui::test::calculate_visible_length(bar);
  CHECK(visible == 25);

  std::string padded = envy::tui::test::pad_to_width(bar, 80);
  CHECK(envy::tui::test::calculate_visible_length(padded) == 80);
  CHECK(padded.starts_with(bar));
  CHECK(padded.size() == bar.size() + (80 - 25));
}

TEST_CASE("pad_to_width - nested ANSI sequences") {
  // Some terminals support nested formatting
  std::string nested = "\x1b[1m\x1b[31mbold red\x1b[0m\x1b[0m";  // visible: "bold red" = 8
  std::string padded = envy::tui::test::pad_to_width(nested, 15);

  CHECK(envy::tui::test::calculate_visible_length(padded) == 15);
  CHECK(padded.starts_with(nested));
  CHECK(padded.ends_with("       "));  // 7 spaces
}

TEST_CASE("pad_to_width - interleaved text and ANSI") {
  std::string interleaved = "a\x1b[31mb\x1b[0mc\x1b[32md\x1b[0me";  // visible: "abcde" = 5
  std::string padded = envy::tui::test::pad_to_width(interleaved, 10);

  CHECK(envy::tui::test::calculate_visible_length(padded) == 10);
  CHECK(padded.starts_with(interleaved));
  CHECK(padded.ends_with("     "));
}

TEST_CASE("pad_to_width - edge case empty with width") {
  std::string padded = envy::tui::test::pad_to_width("", 5);
  CHECK(padded == "     ");
  CHECK(envy::tui::test::calculate_visible_length(padded) == 5);
}

TEST_CASE("pad_to_width - preserves exact ANSI codes") {
  // Verify that padding doesn't modify the original ANSI content
  std::string original = "\x1b[38;5;214mOrange\x1b[0m";  // 256-color orange
  std::string padded = envy::tui::test::pad_to_width(original, 15);

  CHECK(padded.substr(0, original.size()) == original);
  CHECK(envy::tui::test::calculate_visible_length(padded) == 15);
}

TEST_CASE("calculate_visible_length - stress test long string") {
  // Build a long string with many ANSI sequences
  std::string long_str;
  for (int i = 0; i < 100; ++i) {
    long_str += "\x1b[31m" + std::string(10, 'a' + (i % 26)) + "\x1b[0m";
  }
  // Expected: 100 * 10 = 1000 visible chars
  CHECK(envy::tui::test::calculate_visible_length(long_str) == 1000);

  std::string padded = envy::tui::test::pad_to_width(long_str, 1200);
  CHECK(envy::tui::test::calculate_visible_length(padded) == 1200);
}

TEST_CASE("pad_to_width - idempotent when already at width") {
  std::string text = "exactly ten!";  // 12 chars
  std::string first_pad = envy::tui::test::pad_to_width(text, 12);
  std::string second_pad = envy::tui::test::pad_to_width(first_pad, 12);

  CHECK(first_pad == text);
  CHECK(second_pad == text);
  CHECK(first_pad == second_pad);
}

TEST_CASE("calculate_visible_length - all escape sequences end with 'm'") {
  // Verify we correctly identify 'm' as terminator for all SGR sequences
  std::vector<std::string> sequences = {
    "\x1b[0m",               // Reset
    "\x1b[1m",               // Bold
    "\x1b[2m",               // Dim
    "\x1b[3m",               // Italic
    "\x1b[4m",               // Underline
    "\x1b[31m",              // Red
    "\x1b[1;31m",            // Bold red
    "\x1b[38;5;214m",        // 256-color
    "\x1b[38;2;255;128;0m",  // RGB color
  };

  for (auto const &seq : sequences) {
    CHECK(envy::tui::test::calculate_visible_length(seq) == 0);
    CHECK(envy::tui::test::calculate_visible_length(seq + "text") == 4);
  }
}

// ============================================================================
// ANSI-aware truncation tests
// ============================================================================

TEST_CASE("truncate_to_width_ansi_aware - plain text shorter than width") {
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("hello", 10) == "hello");
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("ab", 5) == "ab");
}

TEST_CASE("truncate_to_width_ansi_aware - plain text exact width") {
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("hello", 5) == "hello");
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("12345", 5) == "12345");
}

TEST_CASE("truncate_to_width_ansi_aware - plain text longer than width") {
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("hello world", 5) == "hello");
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("abcdefghij", 3) == "abc");
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("1234567890", 7) == "1234567");
}

TEST_CASE("truncate_to_width_ansi_aware - width zero") {
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("hello", 0) == "");
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("", 0) == "");
}

TEST_CASE("truncate_to_width_ansi_aware - empty string") {
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("", 10) == "");
}

TEST_CASE("truncate_to_width_ansi_aware - ANSI at end preserved") {
  // "hello" (5 chars) + reset code
  std::string s = "hello\x1b[0m";
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 5) == s);
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 10) == s);
}

TEST_CASE("truncate_to_width_ansi_aware - ANSI at start preserved") {
  std::string s = "\x1b[31mhello";  // Red "hello"
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 5) == s);
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 3) == "\x1b[31mhel");
}

TEST_CASE("truncate_to_width_ansi_aware - ANSI in middle preserved") {
  std::string s = "hel\x1b[31mlo world";  // "hel" + red + "lo world"
  std::string result = envy::tui::test::truncate_to_width_ansi_aware(s, 5);
  CHECK(result == "hel\x1b[31mlo");
  CHECK(envy::tui::test::calculate_visible_length(result) == 5);
}

TEST_CASE("truncate_to_width_ansi_aware - multiple ANSI codes") {
  std::string s = "\x1b[1m\x1b[31mBold Red Text\x1b[0m";  // Bold red "Bold Red Text"
  std::string result = envy::tui::test::truncate_to_width_ansi_aware(s, 8);
  CHECK(result == "\x1b[1m\x1b[31mBold Red");
  CHECK(envy::tui::test::calculate_visible_length(result) == 8);
}

TEST_CASE("truncate_to_width_ansi_aware - truncate with ANSI code") {
  std::string s = "hello\x1b[31m world";  // "hello" + red + " world"
  // Truncate to 5: includes ANSI after "hello"
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 5) == "hello\x1b[31m");
  // Truncate to 6: includes ANSI + space
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 6) == "hello\x1b[31m ");
}

TEST_CASE("truncate_to_width_ansi_aware - very long line") {
  std::string long_line =
      "[[local.armgcc@r0]] 100% [====================] 276.57MB/276.57MB "
      "arm-gnu-toolchain-14.3.rel1-mingw-w64-x86_64-arm-none-eabi.zip";
  int width = 80;
  std::string result = envy::tui::test::truncate_to_width_ansi_aware(long_line, width);
  CHECK(envy::tui::test::calculate_visible_length(result) == width);
  CHECK(result.size() <= long_line.size());
}

TEST_CASE("truncate_to_width_ansi_aware - colored very long line") {
  std::string s =
      "\x1b[1m[[local.armgcc@r0]]\x1b[0m 100% "
      "[====================] 276.57MB/276.57MB "
      "\x1b[32marm-gnu-toolchain.zip\x1b[0m extra text";
  int width = 60;
  std::string result = envy::tui::test::truncate_to_width_ansi_aware(s, width);
  CHECK(envy::tui::test::calculate_visible_length(result) == width);
}

TEST_CASE("truncate_to_width_ansi_aware - only ANSI codes") {
  std::string s = "\x1b[31m\x1b[1m\x1b[0m";
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 5) == s);
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 0) == "");
}

TEST_CASE("truncate_to_width_ansi_aware - complete ANSI after truncation point") {
  // Complete ANSI sequences after truncation point should be included
  std::string s = "hello\x1b[31m world";  // "hello" + red code + " world"
  std::string result = envy::tui::test::truncate_to_width_ansi_aware(s, 5);
  // Should include the complete ANSI sequence after "hello"
  CHECK(result == "hello\x1b[31m");
  CHECK(envy::tui::test::calculate_visible_length(result) == 5);
}

TEST_CASE("pad_to_width - now truncates long lines") {
  // After our fix, pad_to_width should truncate lines that are too long
  std::string long_line = "This is a very long line that exceeds the terminal width";
  std::string result = envy::tui::test::pad_to_width(long_line, 20);
  CHECK(envy::tui::test::calculate_visible_length(result) == 20);
  CHECK(result == "This is a very long ");
}

TEST_CASE("pad_to_width - truncates colored long lines") {
  std::string s = "\x1b[31mThis is a very long colored line\x1b[0m that exceeds width";
  std::string result = envy::tui::test::pad_to_width(s, 15);
  CHECK(envy::tui::test::calculate_visible_length(result) == 15);
}

TEST_CASE("calculate_visible_length - handles tabs") {
  // Tabs count as 8 columns
  CHECK(envy::tui::test::calculate_visible_length("\t") == 8);
  CHECK(envy::tui::test::calculate_visible_length("a\tb") ==
        10);  // a + tab + b = 1 + 8 + 1
  CHECK(envy::tui::test::calculate_visible_length("\t\t") == 16);
  CHECK(envy::tui::test::calculate_visible_length("hello\tworld") ==
        18);  // 5 + 8 + 5 = 18
}

TEST_CASE("truncate_to_width_ansi_aware - handles tabs") {
  // Tab counts as 8 columns, so should be truncated
  std::string s = "hello\tworld";  // 5 + 8 + 5 = 18 columns
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 13) == "hello\t");  // 5 + 8 = 13
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 10) ==
        "hello");  // Tab would exceed, so truncate before it
  CHECK(envy::tui::test::truncate_to_width_ansi_aware(s, 5) == "hello");
  CHECK(envy::tui::test::truncate_to_width_ansi_aware("a\tb", 5) ==
        "a");  // 'a' fits, tab would exceed
}

TEST_CASE("pad_to_width - handles tabs") {
  // Tabs count as 8 columns
  std::string s = "a\tb";  // 1 + 8 + 1 = 10 columns
  CHECK(envy::tui::test::calculate_visible_length(envy::tui::test::pad_to_width(s, 10)) ==
        10);
  CHECK(envy::tui::test::pad_to_width(s, 10) == "a\tb");  // Exactly fits

  // Tab makes line too long, should truncate to just "a" then pad to 5
  std::string result = envy::tui::test::pad_to_width(s, 5);
  CHECK(result == "a    ");  // 'a' + 4 spaces = 5 visible chars
  CHECK(envy::tui::test::calculate_visible_length(result) == 5);
}

// ============================================================================
// section_delete tests
// ============================================================================

TEST_CASE("section_delete removes section") {
  auto const h{ envy::tui::section_create() };
  CHECK(h != envy::tui::kInvalidSection);

  envy::tui::section_set_content(
      h,
      envy::tui::section_frame{ .label = "test",
                                .content =
                                    envy::tui::static_text_data{ .text = "hello" } });
  CHECK(envy::tui::section_has_content(h));

  envy::tui::section_delete(h);
  CHECK_FALSE(envy::tui::section_has_content(h));
}

TEST_CASE("section_delete with invalid handle is a no-op") {
  // Should not crash or throw
  envy::tui::section_delete(envy::tui::kInvalidSection);
}

TEST_CASE("section_delete with nonexistent handle is a no-op") {
  envy::tui::section_delete(99999);
}

TEST_CASE("section_delete does not affect other sections") {
  auto const h1{ envy::tui::section_create() };
  auto const h2{ envy::tui::section_create() };

  envy::tui::section_set_content(
      h1,
      envy::tui::section_frame{ .label = "first",
                                .content = envy::tui::static_text_data{ .text = "a" } });
  envy::tui::section_set_content(
      h2,
      envy::tui::section_frame{ .label = "second",
                                .content = envy::tui::static_text_data{ .text = "b" } });

  CHECK(envy::tui::section_has_content(h1));
  CHECK(envy::tui::section_has_content(h2));

  envy::tui::section_delete(h1);
  CHECK_FALSE(envy::tui::section_has_content(h1));
  CHECK(envy::tui::section_has_content(h2));

  envy::tui::section_delete(h2);
  CHECK_FALSE(envy::tui::section_has_content(h2));
}

TEST_CASE("section_delete after set_complete") {
  auto const h{ envy::tui::section_create() };
  envy::tui::section_set_content(
      h,
      envy::tui::section_frame{ .label = "done",
                                .content =
                                    envy::tui::static_text_data{ .text = "finished" } });
  envy::tui::section_set_complete(h);
  CHECK(envy::tui::section_has_content(h));

  envy::tui::section_delete(h);
  CHECK_FALSE(envy::tui::section_has_content(h));
}

TEST_CASE("section_delete double delete is a no-op") {
  auto const h{ envy::tui::section_create() };
  envy::tui::section_set_content(
      h,
      envy::tui::section_frame{ .label = "test",
                                .content = envy::tui::static_text_data{ .text = "x" } });

  envy::tui::section_delete(h);
  CHECK_FALSE(envy::tui::section_has_content(h));

  // Second delete should be a no-op
  envy::tui::section_delete(h);
  CHECK_FALSE(envy::tui::section_has_content(h));
}

// ============================================================================
// section_commit tests
// ============================================================================

TEST_CASE_FIXTURE(captured_output, "section_commit lands the row above what follows") {
  envy::tui::test::g_terminal_width = 120;
  envy::tui::test::g_isatty = true;

  auto const h{ envy::tui::section_create() };
  envy::tui::section_set_content(
      h,
      envy::tui::section_frame{ .label = "hash",
                                .content =
                                    envy::tui::static_text_data{ .text = "committed" } });
  envy::tui::section_commit(h);
  CHECK_FALSE(envy::tui::section_has_content(h));

  CHECK_NOTHROW(envy::tui::run(std::nullopt));
  envy::tui::info("after");
  CHECK_NOTHROW(envy::tui::shutdown());

  REQUIRE(messages.size() == 2);
  CHECK(messages[0].find("hash") != std::string::npos);
  CHECK(messages[0].find("committed") != std::string::npos);
  CHECK(messages[1] == "after\n");
}

TEST_CASE_FIXTURE(captured_output, "section_commit with nothing to say says nothing") {
  envy::tui::test::g_isatty = true;

  auto const h{ envy::tui::section_create() };
  envy::tui::section_commit(h);  // retires the row anyway: nothing left to draw
  envy::tui::section_commit(h);  // already retired
  envy::tui::section_commit(envy::tui::kInvalidSection);
  envy::tui::section_commit(99999);

  CHECK_NOTHROW(envy::tui::run(std::nullopt));
  envy::tui::info("only line");
  CHECK_NOTHROW(envy::tui::shutdown());

  REQUIRE(messages.size() == 1);
  CHECK(messages[0] == "only line\n");
}

TEST_CASE_FIXTURE(captured_output, "section_commit commits one line per row") {
  envy::tui::test::g_terminal_width = 120;
  envy::tui::test::g_isatty = true;

  for (auto const *name : { "first", "second" }) {
    auto const h{ envy::tui::section_create() };
    envy::tui::section_set_content(
        h,
        envy::tui::section_frame{ .label = "hash",
                                  .content =
                                      envy::tui::static_text_data{ .text = name } });
    envy::tui::section_commit(h);
    CHECK_FALSE(envy::tui::section_has_content(h));
  }

  CHECK_NOTHROW(envy::tui::run(std::nullopt));
  CHECK_NOTHROW(envy::tui::shutdown());

  REQUIRE(messages.size() == 2);
  CHECK(messages[0].find("first") != std::string::npos);
  CHECK(messages[1].find("second") != std::string::npos);
}

TEST_CASE_FIXTURE(captured_output, "section_commit off a TTY renders the fallback row") {
  envy::tui::test::g_terminal_width = 120;
  envy::tui::test::g_isatty = false;

  auto const h{ envy::tui::section_create() };
  envy::tui::section_set_content(
      h,
      envy::tui::section_frame{ .label = "extract",
                                .content =
                                    envy::tui::static_text_data{ .text = "done" } });
  envy::tui::section_commit(h);

  CHECK_NOTHROW(envy::tui::run(std::nullopt));
  CHECK_NOTHROW(envy::tui::shutdown());

  REQUIRE(messages.size() == 1);
  CHECK(messages[0] == "[extract] done\n");

  envy::tui::test::g_isatty = true;
}

TEST_CASE_FIXTURE(captured_output, "section_commit off a TTY withholds a complete row") {
  envy::tui::test::g_terminal_width = 120;
  envy::tui::test::g_isatty = false;

  auto const h{ envy::tui::section_create() };
  envy::tui::section_set_content(
      h,
      envy::tui::section_frame{ .label = "import",
                                .content =
                                    envy::tui::static_text_data{ .text = "imported" } });
  envy::tui::section_set_complete(h);  // the caller's own line speaks for this row
  envy::tui::section_commit(h);

  CHECK_NOTHROW(envy::tui::run(std::nullopt));
  CHECK_NOTHROW(envy::tui::shutdown());

  CHECK(messages.empty());

  envy::tui::test::g_isatty = true;
}

#endif  // ENVY_UNIT_TEST
