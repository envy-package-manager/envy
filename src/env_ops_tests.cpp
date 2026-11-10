#include "env_ops.h"

#include "sol_util.h"

#include "doctest.h"

#include <stdexcept>
#include <string>

namespace envy {

namespace {

env_ops parse(char const *lua_table) {
  auto lua{ sol_util_make_lua_state() };
  lua->script(std::string{ "t = " } + lua_table);
  return env_ops_parse((*lua)["t"], "spec 'x'");
}

std::string const sep{ std::string(1, env_path_sep()) };

}  // namespace

TEST_CASE("env_ops_parse: value forms") {
  auto const ops{ parse(R"({ B = "v", A = false, C = { prepend = { "p1", "p2" },
                            append = "a1", sep = " " } })") };
  REQUIRE(ops.size() == 3);
  CHECK(ops[0].name == "A");
  CHECK(std::holds_alternative<env_unset>(ops[0].op));
  CHECK(std::get<env_set>(ops[1].op).value == "v");
  auto const &edit{ std::get<env_edit>(ops[2].op) };
  CHECK(edit.prepend == std::vector<std::string>{ "p1", "p2" });
  CHECK(edit.append == std::vector<std::string>{ "a1" });
  CHECK(edit.sep == " ");
}

TEST_CASE("env_ops_parse: nil list holes and empty edits drop out") {
  auto const ops{ parse(
      R"({ P = { prepend = { "a", nil, "b", "" } }, Q = { prepend = {} } })") };
  REQUIRE(ops.size() == 1);
  auto const &edit{ std::get<env_edit>(ops[0].op) };
  CHECK(edit.prepend == std::vector<std::string>{ "a", "b" });
  CHECK(edit.sep == sep);
}

TEST_CASE("env_ops_parse: rejects bad names, reserved names, and bad values") {
  CHECK_THROWS_WITH_AS(parse(R"({ ["1X"] = "v" })"),
                       doctest::Contains("not a valid variable name"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(parse(R"({ ENVY_FOO = "v" })"),
                       doctest::Contains("reserved"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(parse(R"({ X = true })"),
                       doctest::Contains("false to unset"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(parse(R"({ X = 3 })"),
                       doctest::Contains("must be a string"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(parse(R"({ X = { prepnd = "a" } })"),
                       doctest::Contains("prepnd"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(parse(R"({ X = { prepend = { 1 } } })"),
                       doctest::Contains("must be strings"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(parse(R"({ X = { append = "a", sep = "" } })"),
                       doctest::Contains("sep"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(parse(R"({ "list" })"),
                       doctest::Contains("variable names"),
                       std::runtime_error);
}

TEST_CASE("env_ops_parse: NUL is rejected in every value position") {
  for (char const *t : { R"({ X = "a\0b" })",
                         R"({ X = { prepend = "a\0b" } })",
                         R"({ X = { append = { "ok", "a\0b" } } })",
                         R"({ X = { append = "a", sep = "\0" } })" }) {
    CAPTURE(t);
    CHECK_THROWS_WITH_AS(parse(t), doctest::Contains("NUL"), std::runtime_error);
  }
}

TEST_CASE("env_ops_apply: later layers win") {
  shell_env_t env{ { "PATH", "/usr/bin" }, { "GONE", "x" } };
  env_ops_apply(env, parse(R"({ PATH = { prepend = "/deep" }, GONE = false })"));
  env_ops_apply(env, parse(R"({ PATH = { prepend = "/near", append = "/tail" } })"));
  CHECK(env.at("PATH") == "/near" + sep + "/deep" + sep + "/usr/bin" + sep + "/tail");
  CHECK_FALSE(env.contains("GONE"));

  env_ops_apply(env, parse(R"({ PATH = "/only" })"));
  CHECK(env.at("PATH") == "/only");
}

TEST_CASE("env_ops_apply: an edit on an unset or empty variable has no stray separator") {
  shell_env_t env{ { "EMPTY", "" } };
  env_ops_apply(env, parse(R"({ NEW = { append = "a" }, EMPTY = { prepend = "p" } })"));
  CHECK(env.at("NEW") == "a");
  CHECK(env.at("EMPTY") == "p");
}

TEST_CASE("env_restore: round-trips set, changed, empty, and originally-unset names") {
  shell_env_t const base{ { "KEEP", "k" }, { "CHANGED", "old" }, { "EMPTY", "" } };
  shell_env_t target{ base };
  env_put(target, "CHANGED", "new");
  env_put(target, "ADDED", "a");
  env_put(target, "EMPTY", "now-set");

  auto const live{ env_with_state(
      base,
      target,
      { .root = "/proj", .exe = "/bin/envy", .genfile = "/c/g", .gen = "7" }) };
  CHECK(live.at("ENVY_ACTIVATE_ROOT") == "/proj");
  CHECK(live.at("ENVY_ACTIVATE_GEN") == "7");
  CHECK(live.at("ENVY_ACTIVATE_ORIG__CHANGED") == "old");
  CHECK_FALSE(live.contains("ENVY_ACTIVATE_ORIG__ADDED"));

  auto const restored{ env_restore(live) };
  CHECK(restored == base);
}

TEST_CASE("env_restore: no state is the identity, minus stray state variables") {
  shell_env_t const live{ { "A", "1" }, { "ENVY_ACTIVATE_KEY", "/p" } };
  CHECK(env_restore(live) == shell_env_t{ { "A", "1" } });
}

TEST_CASE("env_restore: a foreign schema is refused") {
  shell_env_t const live{ { "ENVY_ACTIVATE_VARS", "A" },
                          { "ENVY_ACTIVATE_SCHEMA", "99" } };
  CHECK_THROWS_WITH_AS(env_restore(live),
                       doctest::Contains("restart your shell"),
                       std::runtime_error);
}

TEST_CASE("env_changed_names: ignores the activation state") {
  shell_env_t const a{ { "A", "1" }, { "B", "2" } };
  shell_env_t const b{ { "A", "1" }, { "C", "3" }, { "ENVY_ACTIVATE_ROOT", "/p" } };
  CHECK(env_changed_names(a, b) == std::vector<std::string>{ "B", "C" });
}

TEST_CASE("env_script: POSIX quoting") {
  shell_env_t const from{ { "GONE", "x" }, { "SAME", "s" } };
  shell_env_t const to{ { "SAME", "s" }, { "V", "it's $HOME\n\"q\" \\ ;" } };
  CHECK(env_script(env_shell::BASH, from, to) ==
        "unset GONE\nexport V='it'\\''s $HOME\n\"q\" \\ ;'\n");
  CHECK(env_script(env_shell::ZSH, from, to) == env_script(env_shell::BASH, from, to));
}

TEST_CASE("env_script: fish quoting and PATH lists") {
  shell_env_t const from{ { "GONE", "x" } };
  shell_env_t const to{ { "V", "it's \\" }, { "MYPATH", "/a:/b c:" } };
  CHECK(env_script(env_shell::FISH, from, to) ==
        "set -e -g GONE\n"
        "set -gx MYPATH '/a' '/b c' ''\n"
        "set -gx V 'it\\'s \\\\'\n");
}

TEST_CASE("env_script: PowerShell quoting, including typographic quotes") {
  shell_env_t const from{ { "GONE", "x" } };
  shell_env_t const to{ { "V", "it's \xE2\x80\x99$x" } };
  CHECK(env_script(env_shell::POWERSHELL, from, to) ==
        "Remove-Item -LiteralPath 'Env:GONE' -ErrorAction SilentlyContinue\n"
        "$env:V = 'it''s \xE2\x80\x99\xE2\x80\x99$x'\n");
}

TEST_CASE("env_script: refuses names a shell cannot assign") {
  shell_env_t const to{ { "BAD-NAME", "x" } };
  CHECK_THROWS_AS(env_script(env_shell::BASH, {}, to), std::runtime_error);
}

TEST_CASE("env_shell_parse") {
  CHECK(env_shell_parse("bash") == env_shell::BASH);
  CHECK(env_shell_parse("powershell") == env_shell::POWERSHELL);
  CHECK_FALSE(env_shell_parse("cmd").has_value());
}

}  // namespace envy
