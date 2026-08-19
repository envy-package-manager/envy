#if defined(_WIN32)
#error POSIX-only
#endif

#include "shell.h"

#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Long enough that waiting on it is unmistakable, short enough to not litter for long.
static constexpr int kDescendantLifetimeSec{ 10 };
static constexpr double kExitLatencyBudgetSec{ 3.0 };

static std::vector<std::string> run_collect(std::string_view script,
                                            std::optional<fs::path> cwd = std::nullopt,
                                            envy::shell_env_t env = envy::shell_getenv()) {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = cwd,
                           .env = std::move(env),
                           .shell = envy::shell_choice::bash };
  auto const result{ envy::shell_run(script, inv) };
  REQUIRE(result.exit_code == 0);
  REQUIRE(!result.signal.has_value());
  return lines;
}

TEST_CASE("shell_parse_choice POSIX supports bash and sh") {
  CHECK(envy::shell_parse_choice(std::nullopt) == envy::shell_choice::bash);
  CHECK(envy::shell_parse_choice("bash") == envy::shell_choice::bash);
  CHECK(envy::shell_parse_choice("sh") == envy::shell_choice::sh);
  CHECK_THROWS_AS(envy::shell_parse_choice("powershell"), std::invalid_argument);
}

TEST_CASE("shell_run executes multiple lines") {
  auto lines{ run_collect("echo first\nprintf 'second\\n'\n") };
  REQUIRE(lines.size() == 2);
  CHECK(lines[0] == "first");
  CHECK(lines[1] == "second");
}

TEST_CASE("shell_run exposes custom environment variables") {
  auto env{ envy::shell_getenv() };
  env["ENVY_SHELL_TEST"] = "ok";
  auto lines{ run_collect("printf '%s\\n' \"$ENVY_SHELL_TEST\"", std::nullopt, env) };
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "ok");
}

TEST_CASE("shell_run executes with empty environment") {
  auto lines = run_collect("FOO=blank; export FOO; printf '%s\\n' \"$FOO\"",
                           std::nullopt,
                           envy::shell_env_t{});
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "blank");
}

TEST_CASE("shell_run surfaces non-zero exit codes") {
  envy::shell_run_cfg inv{ .on_output_line = [](std::string_view) {},
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  auto const result{ envy::shell_run("exit 7", inv) };
  CHECK(result.exit_code == 7);
  CHECK(!result.signal.has_value());
}

TEST_CASE("shell_run delivers trailing partial lines") {
  auto lines{ run_collect("printf 'without-newline'") };
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "without-newline");
}

TEST_CASE("shell_run handles callback exceptions") {
  envy::shell_run_cfg inv{ .on_output_line =
                               [](std::string_view) { throw std::runtime_error("test"); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  CHECK_THROWS_AS(envy::shell_run("echo hi", inv), std::runtime_error);
}

TEST_CASE("shell_run handles empty script") {
  auto lines{ run_collect("") };
  CHECK(lines.empty());
}

TEST_CASE("shell_run respects working directory") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = "/tmp",
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  auto const result{ envy::shell_run("pwd", inv) };
  REQUIRE(result.exit_code == 0);
  REQUIRE(lines.size() == 1);
  auto actual{ fs::weakly_canonical(fs::path{ lines[0] }) };
  auto expected{ fs::weakly_canonical(fs::path{ "/tmp" }) };
  CHECK(actual == expected);
}

TEST_CASE("shell_run handles invalid working directory") {
  envy::shell_run_cfg inv{ .on_output_line = [](std::string_view) {},
                           .cwd = "/nonexistent/directory/path",
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  auto const result{ envy::shell_run("echo hi", inv) };
  CHECK(result.exit_code == 127);
}

TEST_CASE("shell_run delivers split stdout/stderr callbacks") {
  std::vector<std::string> stdout_lines;
  std::vector<std::string> stderr_lines;
  std::vector<std::string> all_lines;
  envy::shell_run_cfg inv{
    .on_output_line = [&](std::string_view line) { all_lines.emplace_back(line); },
    .on_stdout_line = [&](std::string_view line) { stdout_lines.emplace_back(line); },
    .on_stderr_line = [&](std::string_view line) { stderr_lines.emplace_back(line); },
    .env = envy::shell_getenv(),
    .shell = envy::shell_choice::bash,
  };
  auto const result{ envy::shell_run(
      "printf 'out1\\n'; >&2 printf 'err1\\n'; printf 'out2\\n'; >&2 printf 'err2\\n'",
      inv) };
  REQUIRE(result.exit_code == 0);
  CHECK(stdout_lines == std::vector<std::string>{ "out1", "out2" });
  CHECK(stderr_lines == std::vector<std::string>{ "err1", "err2" });
  CHECK(all_lines.size() == 4);
  auto count_line = [&](std::string const &needle) {
    return std::count(all_lines.begin(), all_lines.end(), needle);
  };
  CHECK(count_line("out1") == 1);
  CHECK(count_line("out2") == 1);
  CHECK(count_line("err1") == 1);
  CHECK(count_line("err2") == 1);
  auto find_index = [&](std::string const &needle) {
    auto it = std::find(all_lines.begin(), all_lines.end(), needle);
    return static_cast<int>(std::distance(all_lines.begin(), it));
  };
  CHECK(find_index("out1") < find_index("out2"));
  CHECK(find_index("err1") < find_index("err2"));
}

TEST_CASE("shell_run handles large output") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  auto const result{ envy::shell_run("for i in {1..1000}; do printf '%0100d\\n' $i; done",
                                     inv) };
  REQUIRE(result.exit_code == 0);
  CHECK(lines.size() == 1000);
  CHECK(lines[0].size() == 100);
  CHECK(lines[999].size() == 100);
}

TEST_CASE("shell_run callback is required") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  auto const result{ envy::shell_run("echo test", inv) };
  CHECK(result.exit_code == 0);
}

TEST_CASE("shell_run bash exits on first error (fail-fast)") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  // First command fails (false returns 1), second command should NOT run
  auto const result{ envy::shell_run("echo before\nfalse\necho after", inv) };
  CHECK(result.exit_code == 1);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "before");
}

TEST_CASE("shell_run sh exits on first error (fail-fast)") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::sh };
  // First command fails (false returns 1), second command should NOT run
  auto const result{ envy::shell_run("echo before\nfalse\necho after", inv) };
  CHECK(result.exit_code == 1);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "before");
}

TEST_CASE("shell_run bash fails on command not found") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  // Nonexistent command should fail, subsequent command should NOT run
  auto const result{
    envy::shell_run("echo before\nnonexistent_command_xyz_12345\necho after", inv)
  };
  CHECK(result.exit_code != 0);
  CHECK(lines.size() >= 1);
  CHECK(lines[0] == "before");
  // "after" should NOT appear because fail-fast should stop execution
  for (auto const &line : lines) { CHECK(line != "after"); }
}

// shell_run must return when the direct child exits, not when the pipes reach EOF: a
// descendant that inherits stdout/stderr and outlives the child would otherwise pin envy
// for that descendant's entire lifetime.
static double run_with_lingering_descendant(std::string_view script,
                                            std::vector<std::string> &lines) {
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  auto const start{ std::chrono::steady_clock::now() };
  auto const result{ envy::shell_run(script, inv) };
  std::chrono::duration<double> const elapsed{ std::chrono::steady_clock::now() - start };
  CHECK(result.exit_code == 0);
  CHECK(!result.signal.has_value());
  return elapsed.count();
}

TEST_CASE("shell_run returns when child exits with descendant holding stdout") {
  std::vector<std::string> lines;
  auto const script{ "echo hi; (sleep " + std::to_string(kDescendantLifetimeSec) +
                     " 2>/dev/null &)" };
  double const elapsed{ run_with_lingering_descendant(script, lines) };
  CHECK(elapsed < kExitLatencyBudgetSec);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "hi");
}

TEST_CASE("shell_run returns when child exits with descendant holding stderr") {
  std::vector<std::string> lines;
  auto const script{ ">&2 echo hi; (sleep " + std::to_string(kDescendantLifetimeSec) +
                     " >/dev/null &)" };
  double const elapsed{ run_with_lingering_descendant(script, lines) };
  CHECK(elapsed < kExitLatencyBudgetSec);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "hi");
}

TEST_CASE("shell_run delivers all pre-exit output despite lingering descendant") {
  std::vector<std::string> lines;
  auto const script{ "(sleep " + std::to_string(kDescendantLifetimeSec) +
                     " &); for i in {1..500}; do printf '%0100d\\n' $i; done; "
                     "printf 'tail-no-newline'" };
  double const elapsed{ run_with_lingering_descendant(script, lines) };
  CHECK(elapsed < kExitLatencyBudgetSec);
  REQUIRE(lines.size() == 501);
  CHECK(lines[0].size() == 100);
  CHECK(lines[499].size() == 100);
  CHECK(lines[500] == "tail-no-newline");
}

TEST_CASE("shell_run bash fails on exit 1 mid-script") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::bash };
  auto const result{ envy::shell_run("echo line1\nexit 42\necho line2", inv) };
  CHECK(result.exit_code == 42);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "line1");
}
