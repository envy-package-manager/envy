#if defined(_WIN32)
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "doctest.h"
#include "shell.h"

namespace fs = std::filesystem;

static std::vector<std::string> run_collect(
    std::string_view script,
    envy::shell_choice shell = envy::shell_choice::powershell,
    std::optional<fs::path> cwd = std::nullopt,
    envy::shell_env_t env = envy::shell_getenv()) {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = std::move(cwd),
                           .env = std::move(env),
                           .shell = shell };
  auto const result{ envy::shell_run(script, inv) };
  REQUIRE(result.exit_code == 0);
  REQUIRE(!result.signal.has_value());
  return lines;
}

TEST_CASE("shell_parse_choice Windows defaults to powershell") {
  CHECK(envy::shell_parse_choice(std::nullopt) == envy::shell_choice::powershell);
  CHECK(envy::shell_parse_choice(std::string_view("")) == envy::shell_choice::powershell);
}

TEST_CASE("shell_parse_choice Windows accepts explicit shells") {
  CHECK(envy::shell_parse_choice("powershell") == envy::shell_choice::powershell);
  CHECK(envy::shell_parse_choice("cmd") == envy::shell_choice::cmd);
}

TEST_CASE("shell_parse_choice Windows rejects invalid shells") {
  CHECK_THROWS_AS(envy::shell_parse_choice("bash"), std::invalid_argument);
}

TEST_CASE("shell_run powershell executes multiple lines") {
  auto lines{ run_collect("Write-Output 'first'\nWrite-Output 'second'") };
  REQUIRE(lines.size() == 2);
  CHECK(lines[0] == "first");
  CHECK(lines[1] == "second");
}

TEST_CASE("shell_run exposes custom environment variables (powershell)") {
  auto env{ envy::shell_getenv() };
  env["ENVY_SHELL_TEST"] = "ok";
  auto lines{ run_collect("Write-Output $env:ENVY_SHELL_TEST",
                          envy::shell_choice::powershell,
                          std::nullopt,
                          std::move(env)) };
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "ok");
}

TEST_CASE("shell_run executes with empty environment (powershell)") {
  auto lines =
      run_collect("if ($env:FOO) { Write-Output $env:FOO } else { Write-Output 'blank' }",
                  envy::shell_choice::powershell,
                  std::nullopt,
                  envy::shell_env_t{});
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "blank");
}

TEST_CASE("shell_run supports cmd shell option") {
  auto lines{ run_collect("@echo off\necho first", envy::shell_choice::cmd) };
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "first");
}

TEST_CASE("shell_run respects working directory (powershell)") {
  auto tmp_dir{ fs::temp_directory_path() / "envy-shell-test" };
  fs::create_directories(tmp_dir);
  struct dir_cleanup {
    fs::path path;
    ~dir_cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{ tmp_dir };
  auto lines{ run_collect("Get-Location | Select-Object -ExpandProperty Path",
                          envy::shell_choice::powershell,
                          tmp_dir) };
  REQUIRE(lines.size() == 1);
  auto actual{ fs::weakly_canonical(fs::path{ lines[0] }) };
  auto expected{ fs::weakly_canonical(tmp_dir) };
  CHECK(actual == expected);
}

TEST_CASE("shell_run surfaces non-zero exit codes (powershell)") {
  envy::shell_run_cfg inv{ .on_output_line = [](std::string_view) {},
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::powershell };
  auto const result{ envy::shell_run("exit 7", inv) };
  CHECK(result.exit_code == 7);
  CHECK(!result.signal.has_value());
}

TEST_CASE("shell_run handles callback exceptions (powershell)") {
  envy::shell_run_cfg inv{ .on_output_line =
                               [](std::string_view) { throw std::runtime_error("test"); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::powershell };
  CHECK_THROWS_AS(envy::shell_run("Write-Output 'hi'", inv), std::runtime_error);
}

TEST_CASE("shell_run powershell exits on first error (fail-fast)") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::powershell };
  // cmd.exe /c "exit 1" returns exit code 1, subsequent command should NOT run
  auto const result{ envy::shell_run(
      "Write-Output 'before'\ncmd.exe /c \"exit 1\"\nWrite-Output 'after'",
      inv) };
  CHECK(result.exit_code == 1);
  REQUIRE(lines.size() >= 1);
  CHECK(lines[0] == "before");
  // "after" should NOT appear because fail-fast should stop execution
  for (auto const &line : lines) { CHECK(line != "after"); }
}

TEST_CASE("shell_run powershell fails on nonexistent command") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::powershell };
  // Nonexistent command should fail
  auto const result{ envy::shell_run(
      "Write-Output 'before'\nnonexistent_command_xyz_12345\nWrite-Output 'after'",
      inv) };
  CHECK(result.exit_code != 0);
  // "after" should NOT appear because fail-fast should stop execution
  for (auto const &line : lines) { CHECK(line != "after"); }
}

TEST_CASE("shell_run cmd exits on first error (fail-fast)") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::cmd };
  // exit /b 1 should fail, subsequent command should NOT run
  auto const result{ envy::shell_run("echo before\nexit /b 1\necho after", inv) };
  CHECK(result.exit_code == 1);
  REQUIRE(lines.size() >= 1);
  CHECK(lines[0] == "before");
  // "after" should NOT appear
  for (auto const &line : lines) { CHECK(line != "after"); }
}

TEST_CASE("shell_run cmd fails on nonexistent command") {
  std::vector<std::string> lines;
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::cmd };
  // Nonexistent command should fail, subsequent command should NOT run
  auto const result{
    envy::shell_run("echo before\nnonexistent_command_xyz_12345\necho after", inv)
  };
  CHECK(result.exit_code != 0);
  REQUIRE(lines.size() >= 1);
  CHECK(lines[0] == "before");
  // "after" should NOT appear
  for (auto const &line : lines) { CHECK(line != "after"); }
}

// shell_run must return when the direct child exits, not when the pipes reach EOF: `start
// /b` hands our inherited stdout/stderr handles to a process that outlives the child, and
// waiting for EOF would pin us for that descendant's entire lifetime.
static constexpr char const *kLingeringDescendant{
  "start /b cmd /c \"ping -n 11 127.0.0.1 > nul\""  // ~10s, holds both inherited handles
};
static constexpr double kExitLatencyBudgetSec{ 5.0 };  // half the descendant's lifetime

static double run_with_lingering_descendant(std::string const &script,
                                            std::vector<std::string> &lines) {
  envy::shell_run_cfg inv{ .on_output_line =
                               [&](std::string_view line) { lines.emplace_back(line); },
                           .cwd = std::nullopt,
                           .env = envy::shell_getenv(),
                           .shell = envy::shell_choice::cmd };
  auto const start{ std::chrono::steady_clock::now() };
  auto const result{ envy::shell_run(script, inv) };
  std::chrono::duration<double> const elapsed{ std::chrono::steady_clock::now() - start };
  CHECK(result.exit_code == 0);
  return elapsed.count();
}

TEST_CASE("shell_run returns when child exits with descendant holding pipes") {
  std::vector<std::string> lines;
  double const elapsed{ run_with_lingering_descendant(
      std::string{ "echo hi\n" } + kLingeringDescendant, lines) };
  CHECK(elapsed < kExitLatencyBudgetSec);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "hi");
}

TEST_CASE("shell_run delivers all pre-exit output despite lingering descendant") {
  std::vector<std::string> lines;
  double const elapsed{ run_with_lingering_descendant(
      std::string{ kLingeringDescendant } +
          "\nfor /L %%i in (1,1,200) do @echo 0123456789\necho tail",
      lines) };
  CHECK(elapsed < kExitLatencyBudgetSec);
  REQUIRE(lines.size() == 201);
  CHECK(lines[0] == "0123456789");
  CHECK(lines[200] == "tail");
}

#endif  // _WIN32
