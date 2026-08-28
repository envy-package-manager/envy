#include "luarc.h"

#include "doctest.h"
#include "picojson.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST_CASE("make_portable_path") {
#ifdef _WIN32
  char const *home{ std::getenv("USERPROFILE") };
  char const env_var[]{ "${env:USERPROFILE}" };
  char const sep{ '\\' };
#else
  char const *home{ std::getenv("HOME") };
  char const env_var[]{ "${env:HOME}" };
  char const sep{ '/' };
#endif
  REQUIRE(home);
  std::string const home_str{ home };

  SUBCASE("replaces home prefix with env var") {
    fs::path const path{ home_str + sep + "Library" + sep + "Caches" };
    CHECK(envy::make_portable_path(path) ==
          std::string{ env_var } + "/" + "Library" + "/" + "Caches");
  }

  SUBCASE("preserves paths not under home") {
#ifdef _WIN32
    fs::path const path{ "C:\\Windows\\System32" };
    CHECK(envy::make_portable_path(path) == "C:/Windows/System32");
#else
    fs::path const path{ "/tmp/some/other/path" };
    CHECK(envy::make_portable_path(path) == "/tmp/some/other/path");
#endif
  }

  SUBCASE("handles home as exact path") {
    fs::path const path{ home_str };
    CHECK(envy::make_portable_path(path) == env_var);
  }

  SUBCASE("does not replace partial home matches") {
    fs::path const path{ home_str + "-other" + sep + "something" };
    std::string expected{ home_str + "-other" + sep + "something" };
    std::replace(expected.begin(), expected.end(), '\\', '/');
    CHECK(envy::make_portable_path(path) == expected);
  }
}

namespace {

// Parse rewrite result and extract workspace.library array entries
std::vector<std::string> parse_library(std::string const &json) {
  picojson::value root;
  picojson::parse(root, json);
  auto &lib{ root.get<picojson::object>().at("workspace.library").get<picojson::array>() };
  std::vector<std::string> result;
  for (auto const &v : lib) { result.push_back(v.get<std::string>()); }
  return result;
}

picojson::array parse_library_raw(std::string const &json) {
  picojson::value root;
  picojson::parse(root, json);
  return root.get<picojson::object>().at("workspace.library").get<picojson::array>();
}

std::vector<std::string> const kCanonical{
  "~/Library/Caches/envy/envy/1.0.0",
  "~/.cache/envy/envy/1.0.0",
  "${env:USERPROFILE}/AppData/Local/envy/envy/1.0.0"
};

}  // namespace

TEST_CASE("rewrite_luarc_types_path") {
  SUBCASE("replaces single old-style entry with canonical set") {
    std::string const input{
      R"({"workspace.library": ["${env:HOME}/.cache/envy/envy/0.1.0"]})"
    };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 3);
    CHECK(lib[0] == kCanonical[0]);
    CHECK(lib[1] == kCanonical[1]);
    CHECK(lib[2] == kCanonical[2]);
  }

  SUBCASE("replaces multiple accumulated entries from different platforms") {
    std::string const input{ R"({"workspace.library": [)"
                             R"("${env:HOME}/Library/Caches/envy/envy/0.0.35",)"
                             R"("${env:HOME}/work/firmware/.envy-cache/envy/0.0.35")"
                             R"(]})" };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 3);
    CHECK(lib[0] == kCanonical[0]);
    CHECK(lib[1] == kCanonical[1]);
    CHECK(lib[2] == kCanonical[2]);
  }

  SUBCASE("already-correct returns nullopt") {
    std::string input{ R"({"workspace.library": [)" };
    for (size_t i{ 0 }; i < kCanonical.size(); ++i) {
      if (i > 0) { input += ","; }
      input += "\"" + kCanonical[i] + "\"";
    }
    input += "]}";
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    CHECK_FALSE(result.has_value());
  }

  SUBCASE("non-envy entries preserved alongside canonical set") {
    std::string const input{
      R"({"workspace.library": ["/usr/local/lua-libs", "~/Library/Caches/envy/envy/0.1.0", "/another/lib"]})"
    };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 5);
    CHECK(lib[0] == "/usr/local/lua-libs");
    CHECK(lib[1] == "/another/lib");
    CHECK(lib[2] == kCanonical[0]);
    CHECK(lib[3] == kCanonical[1]);
    CHECK(lib[4] == kCanonical[2]);
  }

  SUBCASE("empty library adds canonical set") {
    auto result{ envy::rewrite_luarc_types_path(R"({"workspace.library": []})",
                                                kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 3);
    CHECK(lib[0] == kCanonical[0]);
  }

  SUBCASE("/foo/envy/not-a-semver is NOT matched (preserved)") {
    std::string const input{ R"({"workspace.library": ["/foo/envy/not-a-semver"]})" };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 4);
    CHECK(lib[0] == "/foo/envy/not-a-semver");
    CHECK(lib[1] == kCanonical[0]);
  }

  SUBCASE("pre-release semver entries are matched") {
    std::string const input{
      R"({"workspace.library": ["/some/path/envy/0.1.0-beta.1"]})"
    };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 3);
    CHECK(lib[0] == kCanonical[0]);
  }

  SUBCASE("library with only non-string entries adds canonical set") {
    auto result{ envy::rewrite_luarc_types_path(R"({"workspace.library": [42, true]})",
                                                kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library_raw(*result) };
    REQUIRE(lib.size() == 5);
    CHECK(lib[0].is<double>());
    CHECK(lib[1].is<bool>());
    CHECK(lib[2].get<std::string>() == kCanonical[0]);
  }

  SUBCASE("invalid JSON returns nullopt") {
    auto result{ envy::rewrite_luarc_types_path("{not valid json", kCanonical) };
    CHECK_FALSE(result.has_value());
  }

  SUBCASE("empty string returns nullopt") {
    auto result{ envy::rewrite_luarc_types_path("", kCanonical) };
    CHECK_FALSE(result.has_value());
  }

  SUBCASE("root not object returns nullopt") {
    auto result{ envy::rewrite_luarc_types_path("[1, 2, 3]", kCanonical) };
    CHECK_FALSE(result.has_value());
  }

  SUBCASE("missing workspace.library returns nullopt") {
    auto result{ envy::rewrite_luarc_types_path(R"({"other.key": 42})", kCanonical) };
    CHECK_FALSE(result.has_value());
  }

  SUBCASE("workspace.library not array returns nullopt") {
    auto result{ envy::rewrite_luarc_types_path(R"({"workspace.library": "not-an-array"})",
                                                kCanonical) };
    CHECK_FALSE(result.has_value());
  }

  SUBCASE("other JSON keys are preserved") {
    std::string const input{
      R"({"diagnostics.globals": ["envy"], "workspace.library": ["~/.cache/envy/envy/0.1.0"], "completion.enable": true})"
    };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    picojson::value root;
    picojson::parse(root, *result);
    auto &obj{ root.get<picojson::object>() };
    CHECK(obj.count("diagnostics.globals") == 1);
    CHECK(obj.count("completion.enable") == 1);
  }

  SUBCASE("multiple custom entries preserved when adding canonical") {
    auto result{ envy::rewrite_luarc_types_path(
        R"({"workspace.library": ["/custom/lib1", "/custom/lib2", "/custom/lib3"]})",
        kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 6);
    CHECK(lib[0] == "/custom/lib1");
    CHECK(lib[1] == "/custom/lib2");
    CHECK(lib[2] == "/custom/lib3");
    CHECK(lib[3] == kCanonical[0]);
  }

  SUBCASE("trailing slash on envy path still matches") {
    std::string const input{ R"({"workspace.library": ["/foo/envy/1.0.0/"]})" };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 3);
    CHECK(lib[0] == kCanonical[0]);
  }

  SUBCASE("/foo/envy/ with no version is not matched") {
    std::string const input{ R"({"workspace.library": ["/foo/envy/"]})" };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 4);
    CHECK(lib[0] == "/foo/envy/");
  }

  SUBCASE("build metadata semver is matched") {
    std::string const input{ R"({"workspace.library": ["/cache/envy/1.0.0+build.123"]})" };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 3);
    CHECK(lib[0] == kCanonical[0]);
  }

  SUBCASE("multiple /envy/ segments matches on last") {
    std::string const input{ R"({"workspace.library": ["/envy/stuff/envy/2.0.0"]})" };
    auto result{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 3);
    CHECK(lib[0] == kCanonical[0]);
  }

  SUBCASE("round-trip idempotency") {
    std::string const input{
      R"({"workspace.library": ["~/.cache/envy/envy/0.1.0", "/custom/lib"]})"
    };
    auto first{ envy::rewrite_luarc_types_path(input, kCanonical) };
    REQUIRE(first.has_value());
    auto second{ envy::rewrite_luarc_types_path(*first, kCanonical) };
    CHECK_FALSE(second.has_value());
  }

  SUBCASE("empty canonical_paths removes envy entries and adds nothing") {
    std::string const input{
      R"({"workspace.library": ["/custom/lib", "~/.cache/envy/envy/0.1.0"]})"
    };
    std::vector<std::string> const empty;
    auto result{ envy::rewrite_luarc_types_path(input, empty) };
    REQUIRE(result.has_value());
    auto lib{ parse_library(*result) };
    REQUIRE(lib.size() == 1);
    CHECK(lib[0] == "/custom/lib");
  }
}

TEST_CASE("compute_canonical_luarc_paths") {
  SUBCASE("default meta lists the default local tree plus every platform default") {
    // A union, not a choice: .luarc.json is committed, and the same manifest is local for
    // whoever ran `envy cache --local` and shared for everyone else.
    envy::envy_meta meta;
    auto paths{ envy::compute_canonical_luarc_paths(meta) };
    REQUIRE(paths.size() == 4);
    CHECK(paths[0].find(".envy/cache/envy/") == 0);
    CHECK(paths[1].find("~/Library/Caches/envy/envy/") == 0);
    CHECK(paths[2].find("~/.cache/envy/envy/") == 0);
    CHECK(paths[3].find("${env:USERPROFILE}/AppData/Local/envy/envy/") == 0);
  }

  SUBCASE("cache_local replaces the local entry and keeps the shared ones") {
    envy::envy_meta meta;
    meta.cache_local = "out/.envy";
    auto paths{ envy::compute_canonical_luarc_paths(meta) };
    REQUIRE(paths.size() == 4);
    CHECK(paths[0].find("out/.envy/envy/") == 0);
    CHECK(paths[1].find("~/Library/Caches/envy/envy/") == 0);
    CHECK(paths[2].find("~/.cache/envy/envy/") == 0);
    CHECK(paths[3].find("${env:USERPROFILE}/AppData/Local/envy/envy/") == 0);
  }

  SUBCASE("the local entry stays relative so a committed .luarc.json travels") {
    // An absolute path would name the machine that generated the file.
    envy::envy_meta meta;
    meta.cache_local = "out/.envy";
    auto paths{ envy::compute_canonical_luarc_paths(meta) };
    CHECK(paths[0][0] != '/');
    CHECK(paths[0].find(':') == std::string::npos);
  }

  SUBCASE("cache_local backslashes normalized to forward slashes") {
    envy::envy_meta meta;
    meta.cache_local = "out\\.envy";
    auto paths{ envy::compute_canonical_luarc_paths(meta) };
    REQUIRE(paths.size() == 4);
    CHECK(paths[0].find("out/.envy/envy/") == 0);
    CHECK(paths[0].find('\\') == std::string::npos);
  }
}
