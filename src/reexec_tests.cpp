#include "reexec.h"

#include "doctest.h"

#include <string>
#include <vector>

// --- reexec_should decision logic ---

TEST_CASE("reexec_should: no @envy version returns PROCEED") {
  CHECK(envy::reexec_should("2.0.0", std::nullopt, false, false) ==
        envy::reexec_decision::PROCEED);
}

TEST_CASE("reexec_should: dev build 0.0.0 returns PROCEED") {
  CHECK(envy::reexec_should("0.0.0", std::string{ "1.5.0" }, false, false) ==
        envy::reexec_decision::PROCEED);
}

TEST_CASE("reexec_should: version match returns PROCEED") {
  CHECK(envy::reexec_should("1.5.0", std::string{ "1.5.0" }, false, false) ==
        envy::reexec_decision::PROCEED);
}

TEST_CASE("reexec_should: ENVY_REEXEC set returns PROCEED") {
  CHECK(envy::reexec_should("2.0.0", std::string{ "1.5.0" }, true, false) ==
        envy::reexec_decision::PROCEED);
}

TEST_CASE("reexec_should: ENVY_NO_REEXEC set returns PROCEED") {
  CHECK(envy::reexec_should("2.0.0", std::string{ "1.5.0" }, false, true) ==
        envy::reexec_decision::PROCEED);
}

TEST_CASE("reexec_should: both ENVY_REEXEC and ENVY_NO_REEXEC set returns PROCEED") {
  CHECK(envy::reexec_should("2.0.0", std::string{ "1.5.0" }, true, true) ==
        envy::reexec_decision::PROCEED);
}

TEST_CASE("reexec_should: version mismatch (downgrade) returns REEXEC") {
  CHECK(envy::reexec_should("2.0.0", std::string{ "1.5.0" }, false, false) ==
        envy::reexec_decision::REEXEC);
}

TEST_CASE("reexec_should: version mismatch (upgrade) returns REEXEC") {
  CHECK(envy::reexec_should("1.0.0", std::string{ "2.0.0" }, false, false) ==
        envy::reexec_decision::REEXEC);
}

TEST_CASE("reexec_should: empty requested version string triggers REEXEC") {
  // optional with empty string is still a value; "" != "2.0.0" → mismatch
  CHECK(envy::reexec_should("2.0.0", std::string{ "" }, false, false) ==
        envy::reexec_decision::REEXEC);
}

TEST_CASE("reexec_should: dev build 0.0.0 even with REEXEC flag returns PROCEED") {
  // Dev build check comes before REEXEC flag check — 0.0.0 always wins
  CHECK(envy::reexec_should("0.0.0", std::string{ "1.5.0" }, true, false) ==
        envy::reexec_decision::PROCEED);
}

TEST_CASE("reexec_should: ENVY_NO_REEXEC takes priority over version mismatch") {
  // no_reexec is checked before version comparison
  CHECK(envy::reexec_should("2.0.0", std::string{ "1.5.0" }, false, true) ==
        envy::reexec_decision::PROCEED);
}

// --- reexec_argv_without: what the re-exec'd child is handed ---

namespace {

// A mutable argv, as main() receives it. doctest owns nothing here; the string literals
// outlive every test.
std::vector<char *> make_argv(std::vector<char const *> const &args) {
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto const *a : args) { argv.push_back(const_cast<char *>(a)); }
  argv.push_back(nullptr);
  return argv;
}

std::vector<std::string> to_strings(std::vector<char *> const &argv) {
  std::vector<std::string> out;
  for (auto const *a : argv) {
    if (!a) { break; }
    out.emplace_back(a);
  }
  return out;
}

}  // namespace

TEST_CASE("reexec_argv_without: drops a separated option and its value") {
  // The value goes too: left behind, it reaches the child as a stray positional.
  auto argv{ make_argv({ "envy", "init", "proj", "bin", "--envy-version", "1.2.3" }) };
  auto const kept{ to_strings(envy::reexec_argv_without(argv.data(), "--envy-version")) };
  CHECK(kept == std::vector<std::string>{ "envy", "init", "proj", "bin" });
}

TEST_CASE("reexec_argv_without: drops the '=' form") {
  auto argv{ make_argv({ "envy", "init", "--envy-version=1.2.3", "proj" }) };
  auto const kept{ to_strings(envy::reexec_argv_without(argv.data(), "--envy-version")) };
  CHECK(kept == std::vector<std::string>{ "envy", "init", "proj" });
}

TEST_CASE("reexec_argv_without: keeps every other option in order") {
  auto argv{ make_argv({ "envy",
                         "init",
                         "proj",
                         "bin",
                         "--mirror",
                         "https://m",
                         "--envy-version",
                         "1.2.3",
                         "--pin-sums" }) };
  auto const kept{ to_strings(envy::reexec_argv_without(argv.data(), "--envy-version")) };
  CHECK(kept == std::vector<std::string>{ "envy",
                                          "init",
                                          "proj",
                                          "bin",
                                          "--mirror",
                                          "https://m",
                                          "--pin-sums" });
}

TEST_CASE("reexec_argv_without: absent option leaves argv untouched") {
  auto argv{ make_argv({ "envy", "sync", "--verbose" }) };
  auto const kept{ to_strings(envy::reexec_argv_without(argv.data(), "--envy-version")) };
  CHECK(kept == std::vector<std::string>{ "envy", "sync", "--verbose" });
}

TEST_CASE("reexec_argv_without: trailing option with no value") {
  // The parser would have rejected this already; the filter must not read past the
  // terminator.
  auto argv{ make_argv({ "envy", "init", "--envy-version" }) };
  auto const kept{ to_strings(envy::reexec_argv_without(argv.data(), "--envy-version")) };
  CHECK(kept == std::vector<std::string>{ "envy", "init" });
}

TEST_CASE("reexec_argv_without: a repeated option is dropped every time") {
  auto argv{ make_argv(
      { "envy", "init", "--envy-version", "1.2.3", "--envy-version=4.5.6", "proj" }) };
  auto const kept{ to_strings(envy::reexec_argv_without(argv.data(), "--envy-version")) };
  CHECK(kept == std::vector<std::string>{ "envy", "init", "proj" });
}

TEST_CASE("reexec_argv_without: a longer option that merely shares the prefix stays") {
  auto argv{ make_argv({ "envy", "init", "--envy-version-check" }) };
  auto const kept{ to_strings(envy::reexec_argv_without(argv.data(), "--envy-version")) };
  CHECK(kept == std::vector<std::string>{ "envy", "init", "--envy-version-check" });
}

TEST_CASE("reexec_argv_without: result is always null-terminated") {
  auto argv{ make_argv({ "envy" }) };
  auto const filtered{ envy::reexec_argv_without(argv.data(), "--envy-version") };
  REQUIRE(filtered.size() == 2);
  CHECK(filtered.back() == nullptr);

  auto const empty{ envy::reexec_argv_without(nullptr, "--envy-version") };
  REQUIRE(empty.size() == 1);
  CHECK(empty.back() == nullptr);
}

// --- reexec_child_argv: the whole of reexec_exec bar the exec ---

TEST_CASE("reexec_child_argv: no dropped options passes argv through") {
  auto argv{ make_argv({ "envy", "install", "--verbose" }) };
  envy::reexec_request const req{ "/cache/envy/1.2.3/envy", {} };
  CHECK(to_strings(envy::reexec_child_argv(req, argv.data())) ==
        std::vector<std::string>{ "envy", "install", "--verbose" });
}

TEST_CASE("reexec_child_argv: every dropped option goes, in one pass each") {
  auto argv{ make_argv(
      { "envy", "init", "proj", "--envy-version", "1.2.3", "--tag=x", "--pin-sums" }) };
  envy::reexec_request const req{ "/tmp/envy", { "--envy-version", "--tag" } };
  CHECK(to_strings(envy::reexec_child_argv(req, argv.data())) ==
        std::vector<std::string>{ "envy", "init", "proj", "--pin-sums" });
}

TEST_CASE("reexec_child_argv: result is owned and null-terminated") {
  // reexec_exec hands .data() straight to exec, so the terminator is not optional.
  auto argv{ make_argv({ "envy" }) };
  envy::reexec_request const req{ "/tmp/envy", { "--envy-version" } };
  auto const child{ envy::reexec_child_argv(req, argv.data()) };
  REQUIRE(child.size() == 2);
  CHECK(child.back() == nullptr);
}
