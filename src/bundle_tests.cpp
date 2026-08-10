#include "bundle.h"

#include "sol/sol.hpp"

#include "doctest.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Helper to parse a Lua string into a sol::table
sol::table lua_eval_table(char const *script, sol::state &lua) {
  auto result{ lua.safe_script(script) };
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error("Lua script failed: " + std::string(err.what()));
  }

  sol::object result_obj = lua["result"];
  if (!result_obj.valid()) { throw std::runtime_error("No 'result' global found"); }
  if (!result_obj.is<sol::table>()) {
    throw std::runtime_error("'result' is not a table");
  }

  return result_obj.as<sol::table>();
}

// Helper to parse a Lua string into a sol::object
sol::object lua_eval_obj(char const *script, sol::state &lua) {
  auto result{ lua.safe_script(script) };
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error("Lua script failed: " + std::string(err.what()));
  }

  sol::object result_obj = lua["result"];
  if (!result_obj.valid()) { throw std::runtime_error("No 'result' global found"); }
  return result_obj;
}

}  // namespace

// bundle::parse_inline tests

TEST_CASE("bundle::parse_inline parses remote source with sha256") {
  sol::state lua;
  auto table{ lua_eval_table(
      "result = { identity = 'acme.toolchain@v1', source = "
      "'https://example.com/bundle.tar.gz', sha256 = 'abc123' }",
      lua) };

  auto src{ envy::bundle::parse_inline(table, fs::path("/fake/envy.lua")) };

  CHECK(src.bundle_identity == "acme.toolchain@v1");

  auto const *remote{ std::get_if<envy::pkg_cfg::remote_source>(&src.fetch_source) };
  REQUIRE(remote);
  CHECK(remote->url == "https://example.com/bundle.tar.gz");
  CHECK(remote->sha256 == "abc123");
}

TEST_CASE("bundle::parse_inline parses remote source without sha256") {
  sol::state lua;
  auto table{ lua_eval_table(
      "result = { identity = 'acme.toolchain@v1', source = "
      "'https://example.com/bundle.tar.gz' }",
      lua) };

  auto src{ envy::bundle::parse_inline(table, fs::path("/fake/envy.lua")) };

  auto const *remote{ std::get_if<envy::pkg_cfg::remote_source>(&src.fetch_source) };
  REQUIRE(remote);
  CHECK(remote->sha256.empty());
}

TEST_CASE("bundle::parse_inline parses relative local source") {
  sol::state lua;
  auto table{ lua_eval_table(
      "result = { identity = 'local.bundle@v1', source = './bundles/local-bundle' }",
      lua) };

  auto src{ envy::bundle::parse_inline(table, fs::path("/project/envy.lua")) };

  CHECK(src.bundle_identity == "local.bundle@v1");

  auto const *local{ std::get_if<envy::pkg_cfg::local_source>(&src.fetch_source) };
  REQUIRE(local);
  CHECK(local->file_path == fs::path("/project/bundles/local-bundle"));
}

TEST_CASE("bundle::parse_inline parses absolute local source") {
  sol::state lua;
  auto table{ lua_eval_table(
      "result = { identity = 'local.bundle@v1', source = '/absolute/path/to/bundle' }",
      lua) };

  auto src{ envy::bundle::parse_inline(table, fs::path("/project/envy.lua")) };

  auto const *local{ std::get_if<envy::pkg_cfg::local_source>(&src.fetch_source) };
  REQUIRE(local);
  CHECK(local->file_path == fs::path("/absolute/path/to/bundle"));
}

TEST_CASE("bundle::parse_inline parses git source with ref") {
  sol::state lua;
  auto table{ lua_eval_table(
      "result = { identity = 'git.bundle@v1', source = 'https://github.com/org/repo.git', "
      "ref "
      "= 'abc123def' }",
      lua) };

  auto src{ envy::bundle::parse_inline(table, fs::path("/fake/envy.lua")) };

  CHECK(src.bundle_identity == "git.bundle@v1");

  auto const *git{ std::get_if<envy::pkg_cfg::git_source>(&src.fetch_source) };
  REQUIRE(git);
  CHECK(git->url == "https://github.com/org/repo.git");
  CHECK(git->ref == "abc123def");
}

TEST_CASE("bundle::parse_inline errors on missing identity") {
  sol::state lua;
  auto table{ lua_eval_table("result = { source = 'https://example.com/bundle.tar.gz' }",
                             lua) };

  CHECK_THROWS_WITH_AS(envy::bundle::parse_inline(table, fs::path("/fake")),
                       doctest::Contains("missing required 'identity' field"),
                       std::runtime_error);
}

TEST_CASE("bundle::parse_inline errors on empty identity") {
  sol::state lua;
  auto table{ lua_eval_table(
      "result = { identity = '', source = 'https://example.com/bundle.tar.gz' }",
      lua) };

  CHECK_THROWS_WITH_AS(envy::bundle::parse_inline(table, fs::path("/fake")),
                       doctest::Contains("missing required 'identity' field"),
                       std::runtime_error);
}

TEST_CASE("bundle::parse_inline errors on missing source") {
  sol::state lua;
  auto table{ lua_eval_table("result = { identity = 'acme.bundle@v1' }", lua) };

  CHECK_THROWS_WITH_AS(envy::bundle::parse_inline(table, fs::path("/fake")),
                       doctest::Contains("missing required 'source' field"),
                       std::runtime_error);
}

TEST_CASE("bundle::parse_inline errors on git source without ref") {
  sol::state lua;
  auto table{ lua_eval_table(
      "result = { identity = 'git.bundle@v1', source = 'https://github.com/org/repo.git' "
      "}",
      lua) };

  CHECK_THROWS_WITH_AS(envy::bundle::parse_inline(table, fs::path("/fake")),
                       doctest::Contains("git source requires 'ref' field"),
                       std::runtime_error);
}

// bundle::parse_aliases tests

TEST_CASE("bundle::parse_aliases parses valid table") {
  sol::state lua;
  auto obj{ lua_eval_obj(
      "result = {\n"
      "  toolchain = { identity = 'acme.toolchain@v1', source = "
      "'https://example.com/tc.tar.gz' "
      "},\n"
      "  libs = { identity = 'acme.libs@v2', source = './local/libs' }\n"
      "}",
      lua) };

  auto bundles{ envy::bundle::parse_aliases(obj, fs::path("/project/envy.lua")) };

  REQUIRE(bundles.size() == 2);

  auto toolchain_it{ bundles.find("toolchain") };
  REQUIRE(toolchain_it != bundles.end());
  CHECK(toolchain_it->second.bundle_identity == "acme.toolchain@v1");

  auto libs_it{ bundles.find("libs") };
  REQUIRE(libs_it != bundles.end());
  CHECK(libs_it->second.bundle_identity == "acme.libs@v2");
}

TEST_CASE("bundle::parse_aliases returns empty map for nil") {
  sol::state lua;
  lua.safe_script("result = nil");
  sol::object obj{ lua["result"] };

  auto bundles{ envy::bundle::parse_aliases(obj, fs::path("/fake")) };

  CHECK(bundles.empty());
}

TEST_CASE("bundle::parse_aliases returns empty map for missing") {
  sol::state lua;
  lua.safe_script("-- no result defined");
  sol::object obj{ lua["nonexistent"] };

  auto bundles{ envy::bundle::parse_aliases(obj, fs::path("/fake")) };

  CHECK(bundles.empty());
}

TEST_CASE("bundle::parse_aliases errors on non-table") {
  sol::state lua;
  auto obj{ lua_eval_obj("result = 'not a table'", lua) };

  CHECK_THROWS_WITH_AS(envy::bundle::parse_aliases(obj, fs::path("/fake")),
                       doctest::Contains("BUNDLES must be a table"),
                       std::runtime_error);
}

TEST_CASE("bundle::parse_aliases errors on non-string key") {
  sol::state lua;
  auto obj{ lua_eval_obj("result = { [123] = { identity = 'test@v1', source = '/path' } }",
                         lua) };

  CHECK_THROWS_WITH_AS(envy::bundle::parse_aliases(obj, fs::path("/fake")),
                       doctest::Contains("BUNDLES key must be string"),
                       std::runtime_error);
}

TEST_CASE("bundle::parse_aliases errors on non-table value") {
  sol::state lua;
  auto obj{ lua_eval_obj("result = { toolchain = 'not a table' }", lua) };

  CHECK_THROWS_WITH_AS(envy::bundle::parse_aliases(obj, fs::path("/fake")),
                       doctest::Contains("BUNDLES['toolchain'] must be a table"),
                       std::runtime_error);
}

TEST_CASE("bundle::parse_aliases lookup returns end iterator for missing alias") {
  sol::state lua;
  auto obj{ lua_eval_obj(
      "result = { toolchain = { identity = 'acme.toolchain@v1', source = '/path' } }",
      lua) };

  auto bundles{ envy::bundle::parse_aliases(obj, fs::path("/fake")) };
  REQUIRE(!bundles.empty());

  auto result{ bundles.find("nonexistent") };

  CHECK(result == bundles.end());
}

// bundle::from_path tests

TEST_CASE("bundle::from_path parses valid bundle") {
  auto b{ envy::bundle::from_path(fs::path("test_data/bundles/simple-bundle")) };

  CHECK(b.identity == "test.simple-bundle@v1");
  CHECK(b.cache_path == fs::path("test_data/bundles/simple-bundle"));
  CHECK(b.schema == 1);
  REQUIRE(b.specs.size() == 2);
  CHECK(b.specs.at("test.spec_a@v1") == "specs/spec_a.lua");
  CHECK(b.specs.at("test.spec_b@v1") == "specs/spec_b.lua");
}

TEST_CASE("bundle::from_path errors on missing envy-bundle.lua") {
  CHECK_THROWS_WITH_AS(envy::bundle::from_path(fs::path("test_data/bundles")),
                       doctest::Contains("Bundle manifest not found"),
                       std::runtime_error);
}

TEST_CASE("bundle::from_path errors on missing BUNDLE field") {
  CHECK_THROWS_WITH_AS(
      envy::bundle::from_path(fs::path("test_data/bundles/invalid-bundle")),
      doctest::Contains("missing required 'BUNDLE' field"),
      std::runtime_error);
}

// bundle::resolve_spec_path tests

TEST_CASE("bundle::resolve_spec_path returns path for known spec") {
  auto b{ envy::bundle::from_path(fs::path("test_data/bundles/simple-bundle")) };

  auto path{ b.resolve_spec_path("test.spec_a@v1") };

  CHECK(path == fs::path("test_data/bundles/simple-bundle/specs/spec_a.lua"));
}

TEST_CASE("bundle::resolve_spec_path returns empty for unknown spec") {
  auto b{ envy::bundle::from_path(fs::path("test_data/bundles/simple-bundle")) };

  auto path{ b.resolve_spec_path("test.unknown@v1") };

  CHECK(path.empty());
}

// bundle::validate tests

TEST_CASE("bundle::validate succeeds for valid bundle") {
  auto b{ envy::bundle::from_path(fs::path("test_data/bundles/simple-bundle")) };

  CHECK_NOTHROW(b.validate());
}

TEST_CASE("bundle::validate errors on missing spec file") {
  envy::bundle b;
  b.identity = "test.bundle@v1";
  b.cache_path = fs::path("test_data/bundles/simple-bundle");
  b.specs["nonexistent.spec@v1"] = "specs/does-not-exist.lua";

  CHECK_THROWS_WITH_AS(b.validate(),
                       doctest::Contains("file not found"),
                       std::runtime_error);
}

TEST_CASE("bundle::validate errors on IDENTITY mismatch") {
  envy::bundle b;
  b.identity = "test.bundle@v1";
  b.cache_path = fs::path("test_data/bundles/simple-bundle");
  // spec_a.lua has IDENTITY = "test.spec_a@v1", not "wrong.identity@v1"
  b.specs["wrong.identity@v1"] = "specs/spec_a.lua";

  CHECK_THROWS_WITH_AS(b.validate(),
                       doctest::Contains("IDENTITY mismatch"),
                       std::runtime_error);
}

TEST_CASE("bundle::validate errors on spec with syntax error") {
  envy::bundle b;
  b.identity = "test.bundle@v1";
  b.cache_path = fs::path("test_data/spec_util");
  b.specs["test.syntax@v1"] = "syntax_error.lua";

  CHECK_THROWS_WITH_AS(b.validate(),
                       doctest::Contains("failed to execute spec"),
                       std::runtime_error);
}

TEST_CASE("bundle::validate validates all specs in parallel") {
  // Use a bundle with multiple specs - validation should be threaded
  auto b{ envy::bundle::from_path(fs::path("test_data/bundles/simple-bundle")) };
  REQUIRE(b.specs.size() == 2);

  CHECK_NOTHROW(b.validate());
}

// ==== bundle::ensure_pkg_cfg ====
//
// The memo is per declaring scope (one manifest or one spec's DEPENDENCIES), and it
// yields the single cfg every reference to that bundle shares. A redeclaration that
// disagrees must be caught here, because the engine's cross-scope check compares two
// cfgs and this scope only ever produces one.

namespace {

envy::pkg_cfg::bundle_source remote_bundle_src(char const *identity, char const *url) {
  return { .bundle_identity = identity,
           .fetch_source = envy::pkg_cfg::remote_source{ .url = url } };
}

envy::pkg_cfg::bundle_source custom_bundle_src(char const *identity,
                                               std::vector<envy::pkg_cfg *> deps = {}) {
  return { .bundle_identity = identity,
           .fetch_source =
               envy::pkg_cfg::custom_fetch_source{ .dependencies = std::move(deps) } };
}

}  // namespace

TEST_CASE("bundle::ensure_pkg_cfg creates a BUNDLE_ONLY-shaped cfg") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  auto const src{ remote_bundle_src("a.tc@v1", "https://x/tc.tgz") };

  auto *cfg{
    envy::bundle::ensure_pkg_cfg(src, fs::path("/fake/envy.lua"), nullptr, memo)
  };

  REQUIRE(cfg != nullptr);
  // identity == bundle_identity is what marks a cfg as the bundle's own package.
  CHECK(cfg->identity == "a.tc@v1");
  REQUIRE(cfg->bundle_identity.has_value());
  CHECK(*cfg->bundle_identity == "a.tc@v1");
  CHECK(cfg->parent == nullptr);
  CHECK(cfg->declaring_file_path == fs::path("/fake/envy.lua"));
  REQUIRE(std::holds_alternative<envy::pkg_cfg::bundle_source>(cfg->source));
  CHECK(memo.size() == 1);
}

TEST_CASE("bundle::ensure_pkg_cfg records the declaring spec as parent") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  auto *declarer{ envy::pkg_cfg::pool()->emplace(
      "a.spec@v1",
      envy::pkg_cfg::remote_source{ .url = "https://x/s.lua" },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<envy::pkg_cfg *>{},
      std::nullopt,
      fs::path("/fake/spec.lua")) };

  auto *cfg{ envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://x/tc.tgz"),
                                          fs::path("/fake/spec.lua"),
                                          declarer,
                                          memo) };

  CHECK(cfg->parent == declarer);
}

TEST_CASE("bundle::ensure_pkg_cfg reuses the cfg for an identical redeclaration") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  auto const src{ remote_bundle_src("a.tc@v1", "https://x/tc.tgz") };
  fs::path const decl{ "/fake/envy.lua" };

  auto *first{ envy::bundle::ensure_pkg_cfg(src, decl, nullptr, memo) };
  auto *second{ envy::bundle::ensure_pkg_cfg(src, decl, nullptr, memo) };

  CHECK(first == second);
  CHECK(memo.size() == 1);
}

TEST_CASE("bundle::ensure_pkg_cfg rejects a conflicting redeclaration") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  fs::path const decl{ "/fake/envy.lua" };

  envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://x/tc.tgz"),
                               decl,
                               nullptr,
                               memo);

  // Two BUNDLES aliases (or two DEPENDENCIES entries) naming one identity.
  CHECK_THROWS_WITH_AS(
      envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://y/tc.tgz"),
                                   decl,
                                   nullptr,
                                   memo),
      doctest::Contains("conflicting sources"),
      std::runtime_error);
}

TEST_CASE("bundle::ensure_pkg_cfg conflict message names the identity and file") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  fs::path const decl{ "/fake/envy.lua" };

  envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://x/tc.tgz"),
                               decl,
                               nullptr,
                               memo);

  CHECK_THROWS_WITH_AS(
      envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://y/tc.tgz"),
                                   decl,
                                   nullptr,
                                   memo),
      doctest::Contains("a.tc@v1"),
      std::runtime_error);
  CHECK_THROWS_WITH_AS(
      envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://y/tc.tgz"),
                                   decl,
                                   nullptr,
                                   memo),
      doctest::Contains("envy.lua"),
      std::runtime_error);
}

TEST_CASE("bundle::ensure_pkg_cfg rejects a redeclaration with a different kind") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  fs::path const decl{ "/fake/envy.lua" };

  envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://x/tc.tgz"),
                               decl,
                               nullptr,
                               memo);

  CHECK_THROWS_AS(
      envy::bundle::ensure_pkg_cfg(custom_bundle_src("a.tc@v1"), decl, nullptr, memo),
      std::runtime_error);
}

TEST_CASE("bundle::ensure_pkg_cfg keys the memo per bundle identity") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  fs::path const decl{ "/fake/envy.lua" };

  auto *one{ envy::bundle::ensure_pkg_cfg(remote_bundle_src("a.tc@v1", "https://x/a.tgz"),
                                          decl,
                                          nullptr,
                                          memo) };
  auto *two{ envy::bundle::ensure_pkg_cfg(
      remote_bundle_src("a.other@v1", "https://x/b.tgz"),
      decl,
      nullptr,
      memo) };

  CHECK(one != two);
  CHECK(memo.size() == 2);
}

TEST_CASE("bundle::ensure_pkg_cfg forwards custom fetch dependencies") {
  std::unordered_map<std::string, envy::pkg_cfg *> memo;
  auto *dep{ envy::pkg_cfg::pool()->emplace(
      "a.tool@v1",
      envy::pkg_cfg::remote_source{ .url = "https://x/t.tgz" },
      "{}",
      std::nullopt,
      nullptr,
      nullptr,
      std::vector<envy::pkg_cfg *>{},
      std::nullopt,
      fs::path("/fake/envy.lua")) };

  auto *cfg{ envy::bundle::ensure_pkg_cfg(custom_bundle_src("a.tc@v1", { dep }),
                                          fs::path("/fake/envy.lua"),
                                          nullptr,
                                          memo) };

  // The bundle's own dependencies must land before its fetch function runs.
  REQUIRE(cfg->source_dependencies.size() == 1);
  CHECK(cfg->source_dependencies[0] == dep);
}
