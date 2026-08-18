#include "manifest.h"

#include "util.h"

#include "sol/sol.hpp"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path test_data_root() {
  auto root{ fs::current_path() / "test_data" / "manifest" };
  if (!fs::exists(root)) {
    root = fs::current_path().parent_path().parent_path() / "test_data" / "manifest";
  }
  return fs::absolute(root);
}

}  // namespace

TEST_CASE("manifest::discover finds envy.lua in current directory") {
  auto test_root{ test_data_root() };
  auto repo_root{ test_root / "repo" };
  REQUIRE(fs::exists(repo_root / "envy.lua"));

  auto result{ envy::manifest::discover(false, repo_root) };

  REQUIRE(result.has_value());
  CHECK(result->path.filename() == "envy.lua");
  CHECK(result->path.parent_path() == repo_root);
}

TEST_CASE("manifest::discover searches upward from subdirectory") {
  auto test_root{ test_data_root() };
  auto nested{ test_root / "repo" / "sibling" };
  REQUIRE(fs::exists(nested));

  auto result{ envy::manifest::discover(false, nested) };

  REQUIRE(result.has_value());
  CHECK(result->path.filename() == "envy.lua");
  CHECK(result->path.parent_path() == test_root / "repo");
}

TEST_CASE("manifest::discover traverses through submodule (.git file)") {
  auto test_root{ test_data_root() };
  auto submodule_nested{ test_root / "repo" / "submodule" / "nested" };
  REQUIRE(fs::exists(submodule_nested));

  // Verify .git is a file (submodule), not directory
  auto git_file{ test_root / "repo" / "submodule" / ".git" };
  if (!fs::exists(git_file)) {
    // Fixture absent on this platform; create a placeholder .git file to emulate submodule
    // boundary.
    std::ofstream f{ git_file };
    f << "gitdir: ../.git/modules/submodule";
  }
  REQUIRE(fs::exists(git_file));
  REQUIRE(fs::is_regular_file(git_file));

  auto result{ envy::manifest::discover(false, submodule_nested) };

  REQUIRE(result.has_value());
  CHECK(result->path.filename() == "envy.lua");
  CHECK(result->path.parent_path() == test_root / "repo");

  // Leave placeholder in place for subsequent test runs; fixture removal unnecessary.
}

TEST_CASE("manifest::discover stops at .git directory boundary") {
  // Create a temporary repo structure without envy.lua
  auto temp_root{ fs::temp_directory_path() / "envy-test-git-boundary" };
  fs::create_directories(temp_root / "test_repo" / ".git");
  fs::create_directories(temp_root / "test_repo" / "subdir");

  auto result{ envy::manifest::discover(false, temp_root / "test_repo" / "subdir") };
  // Should stop at .git directory, not find anything
  CHECK_FALSE(result.has_value());

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover finds envy.lua in non-git directory") {
  auto test_root{ test_data_root() };
  auto non_git{ test_root / "non_git_dir" };
  REQUIRE(fs::exists(non_git / "envy.lua"));

  auto result{ envy::manifest::discover(false, non_git) };

  REQUIRE(result.has_value());
  CHECK(result->path.filename() == "envy.lua");
  CHECK(result->path.parent_path() == non_git);
}

TEST_CASE("manifest::discover searches upward in non-git directory") {
  auto test_root{ test_data_root() };
  auto deeply_nested{ test_root / "non_git_dir" / "deeply" / "nested" / "path" };
  REQUIRE(fs::exists(deeply_nested));

  auto result{ envy::manifest::discover(false, deeply_nested) };

  REQUIRE(result.has_value());
  CHECK(result->path.filename() == "envy.lua");
  CHECK(result->path.parent_path() == test_root / "non_git_dir");
}

TEST_CASE("manifest::discover returns nullopt when no envy.lua found") {
  // Use a system temp directory that's guaranteed to not have envy.lua
  auto temp_root{ fs::temp_directory_path() / "envy-test-no-manifest" };
  fs::create_directories(temp_root);

  auto result{ envy::manifest::discover(false, temp_root) };
  CHECK_FALSE(result.has_value());

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover carries the manifest's bytes and directives") {
  auto const test_root{ test_data_root() };

  auto const result{ envy::manifest::discover(false, test_root / "cache_directive") };

  REQUIRE(result.has_value());
  REQUIRE(result->meta.version.has_value());
  CHECK(*result->meta.version == "1.2.3");
  REQUIRE(result->meta.cache_for_platform().has_value());
  CHECK(*result->meta.cache_for_platform() == "relcache");
  // The bytes come back with the directives, so loading them into Lua costs no second
  // read.
  CHECK(result->content == envy::util_load_file(result->path));
}

// parse_envy_meta stop-at-code tests --------------------------

TEST_CASE("parse_envy_meta ignores directives below the manifest's first code line") {
  auto const meta{ envy::parse_envy_meta(R"(-- @envy version "1.2.3"
PACKAGES = {}
-- @envy cache-posix "too-late"
-- @envy cache-win "too-late"
)") };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "1.2.3");
  CHECK_FALSE(meta.cache_for_platform().has_value());
}

// The six cases below fix where the header ends. Each has a twin in
// functional_tests/test_bootstrap.py driving the launcher scripts over the same bytes: the
// launchers resolve a version and a mirror before this parser ever runs, so a rule that
// holds in only one of them downloads one release and execs a binary that wanted another.

TEST_CASE("parse_envy_meta reads past a tab-indented comment") {
  // Tabs and spaces indent a header line alike. Worth its own case because cmd.exe's
  // `for /f` strips only the delimiters it is given: envy.bat naming space alone left the
  // tab in the first token, so a tab-indented comment ended its header two lines early.
  auto const meta{ envy::parse_envy_meta(
      "\t-- a tab-indented comment\n"
      "\t-- @envy bin \"tools\"\n"
      "-- @envy version \"3.2.1\"\n"
      "PACKAGES = {}\n") };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "3.2.1");
  REQUIRE(meta.bin.has_value());
  CHECK(*meta.bin == "tools");
}

TEST_CASE("parse_envy_meta takes the last of a repeated directive") {
  // Assignment per match, so the later line wins -- what an author editing a value in
  // place and leaving the old line above it would expect. The launchers' read loops
  // overwrite the same way, and the four shell hooks scan the whole header rather than
  // stopping at the first hit, so none of the six can put a different bin directory on
  // PATH than another.
  auto const meta{ envy::parse_envy_meta(
      "-- @envy bin \"old\"\n"
      "-- @envy bin \"new\"\n"
      "PACKAGES = {}\n") };

  REQUIRE(meta.bin.has_value());
  CHECK(*meta.bin == "new");
}

TEST_CASE("parse_envy_meta ends the header at a semicolon-led line") {
  // Lua's empty statement is a line of code like any other. Worth its own case because
  // cmd.exe's `for /f` reads `;` as a comment marker by default and skipped such a line
  // instead of stopping on it, so envy.bat took the directive underneath; it now passes
  // `eol=` to clear the marker.
  auto const meta{ envy::parse_envy_meta(
      "-- @envy version \"1.2.3\"\n"
      ";PACKAGES = {}\n"
      "-- @envy version \"9.9.9\"\n") };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "1.2.3");
}

TEST_CASE("parse_envy_meta ends the header at a block comment's continuation line") {
  // `  still inside it ]]` starts no `--`, so the scan treats it as code. A reader that
  // tracked Lua block-comment nesting instead would take the directive below it.
  auto const meta{ envy::parse_envy_meta(R"(--[[ a block comment
  still inside it ]]
-- @envy version "9.9.9"
)") };

  CHECK_FALSE(meta.version.has_value());
}

TEST_CASE("parse_envy_meta reads a directive inside a block comment") {
  // Inside the block the line does start with `--`, so it parses. Pinned as the deliberate
  // consequence of matching on the comment marker alone, not endorsed as a way to write
  // one.
  auto const meta{ envy::parse_envy_meta(R"(--[[
-- @envy version "7.6.5"
]]
PACKAGES = {}
)") };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "7.6.5");
}

TEST_CASE("parse_envy_meta reads a CRLF manifest") {
  // A blank line is `\r` alone, which the stop-at-code test counts as whitespace, and a
  // value ends at its closing quote -- so no carriage return reaches a download URL.
  auto const meta{ envy::parse_envy_meta(
      "-- @envy version \"5.4.3\"\r\n\r\n-- @envy bin \"tools\"\r\nPACKAGES = {}\r\n") };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "5.4.3");
  REQUIRE(meta.bin.has_value());
  CHECK(*meta.bin == "tools");
}

TEST_CASE("parse_envy_meta reads a header that runs to end of file") {
  // No code line to stop at, and no trailing newline either.
  auto const meta{ envy::parse_envy_meta("-- @envy version \"6.5.4\"") };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "6.5.4");
}

TEST_CASE("parse_envy_meta reads the whole header past blanks and plain comments") {
  auto const meta{ envy::parse_envy_meta(R"(-- a plain comment

  -- @envy version "9.9.9"

-- another comment
   -- @envy bin "tools"
PACKAGES = {}
)") };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "9.9.9");
  REQUIRE(meta.bin.has_value());
  CHECK(*meta.bin == "tools");
}

// find_envy_directive shares the walk above, so these cases pin only what the span adds:
// which line a rewriter lands on, and where the value's bytes sit within it.

TEST_CASE("find_envy_directive spans the value inside the quotes") {
  std::string_view const content{ "-- @envy version \"1.2.3\"\nPACKAGES = {}\n" };

  auto const span{ envy::find_envy_directive(content, "version") };

  REQUIRE(span.has_value());
  CHECK(content.substr(span->value_begin, span->value_end - span->value_begin) == "1.2.3");
  CHECK(content.substr(span->line_begin, span->line_end - span->line_begin) ==
        "-- @envy version \"1.2.3\"");
}

TEST_CASE("find_envy_directive returns the last of a repeated directive") {
  // The lower line is the one every reader sees.
  std::string_view const content{ "-- @envy bin \"old\"\n-- @envy bin \"new\"\nX = 1\n" };

  auto const span{ envy::find_envy_directive(content, "bin") };

  REQUIRE(span.has_value());
  CHECK(content.substr(span->value_begin, span->value_end - span->value_begin) == "new");
}

TEST_CASE("find_envy_directive keeps a CRLF's carriage return out of the value span") {
  std::string_view const content{ "-- @envy version \"5.4.3\"\r\nPACKAGES = {}\r\n" };

  auto const span{ envy::find_envy_directive(content, "version") };

  REQUIRE(span.has_value());
  CHECK(content.substr(span->value_begin, span->value_end - span->value_begin) == "5.4.3");
  // The line span does hold the '\r': it ends at the '\n', which is what lets a whole-line
  // delete take both bytes and an insert land after them.
  CHECK(content[span->line_end - 1] == '\r');
  CHECK(content[span->line_end] == '\n');
}

TEST_CASE("find_envy_directive ignores directives below the first code line") {
  auto const span{ envy::find_envy_directive("PACKAGES = {}\n-- @envy bin \"tools\"\n",
                                             "bin") };

  CHECK_FALSE(span.has_value());
}

TEST_CASE("find_envy_directive spans a header that runs to end of file") {
  std::string_view const content{ "-- @envy version \"6.5.4\"" };

  auto const span{ envy::find_envy_directive(content, "version") };

  REQUIRE(span.has_value());
  CHECK(span->line_end == content.size());
  CHECK(content.substr(span->value_begin, span->value_end - span->value_begin) == "6.5.4");
}

TEST_CASE("find_envy_directive returns nullopt for an absent key") {
  CHECK_FALSE(
      envy::find_envy_directive("-- @envy version \"1.0\"\n", "sha256sums").has_value());
}

// load tests -------------------------------------------------

TEST_CASE("manifest::load parses simple string package") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = { { spec = "arm.gcc@v2", source = "/fake/r.lua" } }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");
  CHECK(m->packages[0]->is_local());
  CHECK(m->packages[0]->serialized_options == "{}");
}

TEST_CASE("manifest::load parses multiple string packages") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { spec = "arm.gcc@v2", source = "/fake/r.lua" },
      { spec = "gnu.binutils@v3", source = "/fake/r.lua" },
      { spec = "vendor.openocd@v1", source = "/fake/r.lua" }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 3);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");
  CHECK(m->packages[1]->identity == "gnu.binutils@v3");
  CHECK(m->packages[2]->identity == "vendor.openocd@v1");
}

TEST_CASE("manifest::load parses table package with remote source") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        source = "https://example.com/gcc.lua",
        sha256 = "abc123"
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");

  auto const *remote{ std::get_if<envy::pkg_cfg::remote_source>(&m->packages[0]->source) };
  REQUIRE(remote != nullptr);
  CHECK(remote->url == "https://example.com/gcc.lua");
  CHECK(remote->sha256 == "abc123");
}

TEST_CASE("manifest::load parses table package with local source") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "local.wrapper@v1",
        source = "./specs/wrapper.lua"
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/project/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "local.wrapper@v1");

  auto const *local{ std::get_if<envy::pkg_cfg::local_source>(&m->packages[0]->source) };
  REQUIRE(local != nullptr);
  CHECK(local->file_path == fs::path("/project/specs/wrapper.lua"));
}

TEST_CASE("manifest::load parses table package with options") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2", source = "/fake/r.lua",
        options = {
          version = "13.2.0",
          target = "arm-none-eabi"
        }
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");

  // Deserialize and check
  sol::state lua;
  auto opts_result{ lua.safe_script("return " + m->packages[0]->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  CHECK(sol::object(opts["version"]).as<std::string>() == "13.2.0");
  CHECK(sol::object(opts["target"]).as<std::string>() == "arm-none-eabi");
}

TEST_CASE("manifest::load parses table package with setup selection") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { spec = "fi.jlink@r0", source = "/fake/r.lua", setup = { "udev_rules", "extras" } },
      { spec = "arm.gcc@v2", source = "/fake/r.lua" }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 2);
  REQUIRE(m->packages[0]->setup.has_value());
  REQUIRE(m->packages[0]->setup->size() == 2);
  CHECK((*m->packages[0]->setup)[0] == "udev_rules");
  CHECK((*m->packages[0]->setup)[1] == "extras");
  CHECK(!m->packages[1]->setup.has_value());
}

TEST_CASE("manifest::load parses empty setup selection as explicit empty") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = { { spec = "fi.jlink@r0", source = "/fake/r.lua", setup = {} } }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  REQUIRE(m->packages[0]->setup.has_value());
  CHECK(m->packages[0]->setup->empty());
}

TEST_CASE("manifest::load errors on non-table setup field") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = { { spec = "fi.jlink@r0", source = "/fake/r.lua", setup = "udev" } }
  )" };

  CHECK_THROWS_WITH(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                    doctest::Contains("'setup' field must be a table"));
}

TEST_CASE("manifest::load errors on non-string setup entry") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = { { spec = "fi.jlink@r0", source = "/fake/r.lua", setup = { 42 } } }
  )" };

  CHECK_THROWS_WITH(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                    doctest::Contains("'setup' entries must be non-empty strings"));
}

TEST_CASE("manifest::load setup selection does not affect serialized options") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { spec = "fi.jlink@r0", source = "/fake/r.lua",
        options = { version = "9.30" }, setup = { "udev_rules" } }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->serialized_options == "{version=\"9.30\"}");
  CHECK(m->packages[0]->format_key() == "fi.jlink@r0{version=\"9.30\"}");
}

TEST_CASE("manifest::load parses mixed string and table packages") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { spec = "envy.homebrew@v4", source = "/fake/r.lua" },
      {
        spec = "arm.gcc@v2",
        source = "https://example.com/gcc.lua",
        sha256 = "abc123",
        options = { version = "13.2.0" }
      },
      { spec = "gnu.make@v1", source = "/fake/r.lua" }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 3);
  CHECK(m->packages[0]->identity == "envy.homebrew@v4");
  CHECK(m->packages[1]->identity == "arm.gcc@v2");
  CHECK(m->packages[2]->identity == "gnu.make@v1");
}

TEST_CASE("manifest::load allows platform conditionals") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {}
    if envy.PLATFORM == "darwin" then
      PACKAGES = { { spec = "envy.homebrew@v4", source = "/fake/r.lua" } }
    elseif envy.PLATFORM == "linux" then
      PACKAGES = { { spec = "system.apt@v1", source = "/fake/r.lua" } }
    elseif envy.PLATFORM == "windows" then
      PACKAGES = { { spec = "system.choco@v1", source = "/fake/r.lua" } }
    end
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  // Should have exactly one package based on current platform
  REQUIRE(m->packages.size() == 1);
#if defined(__APPLE__) && defined(__MACH__)
  CHECK(m->packages[0]->identity == "envy.homebrew@v4");
#elif defined(__linux__)
  CHECK(m->packages[0]->identity == "system.apt@v1");
#elif defined(_WIN32)
  CHECK(m->packages[0]->identity == "system.choco@v1");
#endif
}

TEST_CASE("manifest::load stores manifest path") {
  char const *script{ "-- @envy bin-dir \"tools\"\nPACKAGES = {}" };

  auto m{ envy::manifest::load(script, fs::path("/some/project/envy.lua")) };

  CHECK(m->manifest_path == fs::path("/some/project/envy.lua"));
}

TEST_CASE("manifest::load resolves relative file paths") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "local.tool@v1",
        source = "../sibling/tool.lua"
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/project/sub/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  auto const *local{ std::get_if<envy::pkg_cfg::local_source>(&m->packages[0]->source) };
  REQUIRE(local != nullptr);
  CHECK(local->file_path == fs::path("/project/sibling/tool.lua"));
}

// Error cases ------------------------------------------------------------

TEST_CASE("manifest::load errors on missing packages global") {
  char const *script{ "-- @envy bin-dir \"tools\"\n-- no packages" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Manifest must define 'PACKAGES' global as a table",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on non-table packages") {
  char const *script{ "-- @envy bin-dir \"tools\"\nPACKAGES = 'not a table'" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Manifest must define 'PACKAGES' global as a table",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on invalid package entry type") {
  char const *script{ "-- @envy bin-dir \"tools\"\nPACKAGES = { 123 }" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Spec entry must be string or table",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on missing spec field") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { source = "https://example.com/foo.lua" }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Spec table missing required 'spec' field",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on non-string spec field") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { spec = 123 }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Spec: spec must be a string",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on invalid spec identity format") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = { { spec = "invalid-no-at-sign", source = "/fake/r.lua" } }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Invalid spec identity format: invalid-no-at-sign",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on identity missing namespace") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = { { spec = "gcc@v2", source = "/fake/r.lua" } }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Invalid spec identity format: gcc@v2",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on identity missing version") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = { { spec = "arm.gcc@", source = "/fake/r.lua" } }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Invalid spec identity format: arm.gcc@",
                       std::runtime_error);
}

// Test removed - can no longer specify both url and file since we unified to 'source'

TEST_CASE("manifest::load allows url without sha256 (permissive mode)") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        source = "https://example.com/gcc.lua"
      }
    }
  )" };

  auto const result{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };
  REQUIRE(result->packages.size() == 1);
  CHECK(result->packages[0]->identity == "arm.gcc@v2");
  CHECK(result->packages[0]->is_remote());
  auto const *remote{ std::get_if<envy::pkg_cfg::remote_source>(
      &result->packages[0]->source) };
  REQUIRE(remote != nullptr);
  CHECK(remote->sha256.empty());  // No SHA256 provided (permissive)
}

TEST_CASE("manifest::load errors on non-string source") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        source = 123,
        sha256 = "abc"
      }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Spec 'source' field must be string or table",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on non-string sha256") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        source = "https://example.com/gcc.lua",
        sha256 = 123
      }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Spec source: sha256 must be a string",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on non-string source (local)") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "local.tool@v1",
        source = 123
      }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Spec 'source' field must be string or table",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on non-table options") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        source = "/fake/r.lua",
        options = "not a table"
      }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Spec 'options' field must be table",
                       std::runtime_error);
}

TEST_CASE("manifest::load accepts non-string option values") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2", source = "/fake/r.lua",
        options = { version = 123, debug = true, nested = { key = "value" } }
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);

  // Deserialize and check
  sol::state lua;
  auto opts_result{ lua.safe_script("return " + m->packages[0]->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  CHECK(sol::object(opts["version"]).is<lua_Integer>());
  CHECK(sol::object(opts["version"]).as<int64_t>() == 123);
  CHECK(sol::object(opts["debug"]).is<bool>());
  CHECK(sol::object(opts["debug"]).as<bool>() == true);
  CHECK(sol::object(opts["nested"]).is<sol::table>());
}

TEST_CASE("manifest::load allows same identity with different options") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { spec = "arm.gcc@v2", source = "/fake/r.lua", options = { version = "13.2.0" } },
      { spec = "arm.gcc@v2", source = "/fake/r.lua", options = { version = "12.0.0" } }
    }
  )" };

  // Should not throw
  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };
  REQUIRE(m->packages.size() == 2);
}

TEST_CASE("manifest::load allows duplicate packages") {
  char const *script{ R"(
    -- @envy bin-dir "tools"
    PACKAGES = {
      { spec = "arm.gcc@v2", source = "/fake/r.lua" },
      { spec = "arm.gcc@v2", source = "/fake/r.lua" }
    }
  )" };

  // Should not throw - duplicates are allowed, resolved during spec resolution
  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };
  REQUIRE(m->packages.size() == 2);
}

TEST_CASE("manifest::load errors on Lua syntax error") {
  // Note: Lua syntax errors occur before bin-dir validation, so no bin-dir needed
  CHECK_THROWS_AS(envy::manifest::load("-- @envy bin-dir \"tools\"\n"
                                       "PACKAGES = { this is not valid lua }",
                                       fs::path("/fake/envy.lua")),
                  std::runtime_error);
}

TEST_CASE("manifest::load errors on Lua runtime error") {
  // Note: Lua runtime errors occur after Lua execution, so bin-dir is needed
  CHECK_THROWS_AS(
      envy::manifest::load("-- @envy bin-dir \"tools\"\nerror('intentional error')",
                           fs::path("/fake/envy.lua")),
      std::runtime_error);
}

// @envy directive parsing tests -------------------------------------------

// ============================================================================
// @envy schema directive tests
// ============================================================================

TEST_CASE("parse_envy_meta extracts schema") {
  auto meta{ envy::parse_envy_meta(R"(
-- @envy schema "2"
-- @envy bin "tools"
PACKAGES = {}
)") };

  CHECK(meta.schema == 2);
}

TEST_CASE("parse_envy_meta schema defaults to 0 when absent") {
  auto meta{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
PACKAGES = {}
)") };

  CHECK(meta.schema == 0);
}

TEST_CASE("parse_envy_meta ignores invalid schema") {
  // Non-numeric
  auto meta1{ envy::parse_envy_meta("-- @envy schema \"abc\"\n") };
  CHECK(meta1.schema == 0);

  // Zero
  auto meta2{ envy::parse_envy_meta("-- @envy schema \"0\"\n") };
  CHECK(meta2.schema == 0);

  // Negative
  auto meta3{ envy::parse_envy_meta("-- @envy schema \"-1\"\n") };
  CHECK(meta3.schema == 0);
}

TEST_CASE("manifest::load populates schema field") {
  char const *script{ R"(
-- @envy schema "3"
-- @envy bin "tools"
PACKAGES = {}
)" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  CHECK(m->meta.schema == 3);
}

TEST_CASE("manifest::load schema is 1 with directive") {
  char const *script{ R"(
-- @envy schema "1"
-- @envy bin "tools"
PACKAGES = {}
)" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  CHECK(m->meta.schema == 1);
}

TEST_CASE("manifest::load schema defaults to 0 when absent") {
  char const *script{ R"(
-- @envy bin "tools"
PACKAGES = {}
)" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  CHECK(m->meta.schema == 0);
}

TEST_CASE("parse_envy_meta extracts version") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy version "1.2.3"
PACKAGES = {}
)") };

  REQUIRE(directives.version.has_value());
  CHECK(*directives.version == "1.2.3");
  CHECK_FALSE(directives.cache_posix.has_value());
  CHECK_FALSE(directives.cache_win.has_value());
  CHECK_FALSE(directives.mirror.has_value());
}

TEST_CASE("parse_envy_meta extracts all directives") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy version "2.0.0"
-- @envy cache-posix "/opt/envy-cache"
-- @envy cache-win "C:\opt\envy-cache"
-- @envy mirror "https://internal.corp/releases"
PACKAGES = {}
)") };

  REQUIRE(directives.version.has_value());
  CHECK(*directives.version == "2.0.0");
  REQUIRE(directives.cache_posix.has_value());
  CHECK(*directives.cache_posix == "/opt/envy-cache");
  REQUIRE(directives.cache_win.has_value());
  CHECK(*directives.cache_win == "C:\\opt\\envy-cache");
  REQUIRE(directives.mirror.has_value());
  CHECK(*directives.mirror == "https://internal.corp/releases");
}

TEST_CASE("parse_envy_meta handles escaped quotes") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy version "1.0.0-\"beta\""
PACKAGES = {}
)") };

  REQUIRE(directives.version.has_value());
  CHECK(*directives.version == "1.0.0-\"beta\"");
}

TEST_CASE("parse_envy_meta handles escaped backslash") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy cache-win "C:\\Users\\test\\cache"
-- @envy version "1.0.0-with\\backslash"
PACKAGES = {}
)") };
  REQUIRE(directives.cache_win.has_value());
  CHECK(*directives.cache_win == "C:\\Users\\test\\cache");
  REQUIRE(directives.version.has_value());
  CHECK(*directives.version == "1.0.0-with\\backslash");
}

TEST_CASE("parse_envy_meta handles mixed escapes") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy version "test-\"quoted\"-and-\\backslash"
PACKAGES = {}
)") };

  REQUIRE(directives.version.has_value());
  CHECK(*directives.version == "test-\"quoted\"-and-\\backslash");
}

TEST_CASE("parse_envy_meta returns empty for missing directives") {
  auto directives{ envy::parse_envy_meta(R"(
-- This manifest has no @envy directives
PACKAGES = {}
)") };

  CHECK_FALSE(directives.version.has_value());
  CHECK_FALSE(directives.cache_posix.has_value());
  CHECK_FALSE(directives.cache_win.has_value());
  CHECK_FALSE(directives.mirror.has_value());
}

TEST_CASE("parse_envy_meta handles whitespace variants") {
  auto directives{ envy::parse_envy_meta(
      "--   @envy   version   \"1.0.0\"\n"
      "--\t@envy\tcache-win\t\"C:\\path\"\n"
      "--\t@envy\tcache-posix\t\"/path\"\n"
      "PACKAGES = {}\n") };

  REQUIRE(directives.version.has_value());
  CHECK(*directives.version == "1.0.0");
  REQUIRE(directives.cache_win.has_value());
  CHECK(*directives.cache_win == "C:\\path");
  REQUIRE(directives.cache_posix.has_value());
  CHECK(*directives.cache_posix == "/path");
}

TEST_CASE("parse_envy_meta finds directives anywhere in file") {
  std::string script;
  for (int i = 0; i < 50; ++i) { script += "-- line " + std::to_string(i) + "\n"; }
  script += "-- @envy version \"deep-in-file\"\n";
  script += "PACKAGES = {}\n";

  auto const meta{ envy::parse_envy_meta(script) };

  REQUIRE(meta.version.has_value());
  CHECK(*meta.version == "deep-in-file");
}

TEST_CASE("parse_envy_meta ignores unknown directives") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy version "1.0.0"
-- @envy unknown "some-value"
-- @envy future_directive "another-value"
PACKAGES = {}
)") };

  REQUIRE(directives.version.has_value());
  CHECK(*directives.version == "1.0.0");
  // Unknown directives silently ignored
}

TEST_CASE("manifest::load populates directives field") {
  char const *script{ R"(
-- @envy version "1.2.3"
-- @envy bin-dir "tools"
-- @envy cache-posix "/custom/cache"
-- @envy cache-win "C:\custom\cache"
PACKAGES = {}
)" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->meta.version.has_value());
  CHECK(*m->meta.version == "1.2.3");
  REQUIRE(m->meta.bin.has_value());
  CHECK(*m->meta.bin == "tools");
  REQUIRE(m->meta.cache_posix.has_value());
  CHECK(*m->meta.cache_posix == "/custom/cache");
  REQUIRE(m->meta.cache_win.has_value());
  CHECK(*m->meta.cache_win == "C:\\custom\\cache");
  CHECK_FALSE(m->meta.mirror.has_value());
}

TEST_CASE("parse_envy_meta extracts bin") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools/bin"
PACKAGES = {}
)") };

  REQUIRE(directives.bin.has_value());
  CHECK(*directives.bin == "tools/bin");
}

TEST_CASE("parse_envy_meta extracts bin-dir as legacy alias") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin-dir "legacy/path"
PACKAGES = {}
)") };

  REQUIRE(directives.bin.has_value());
  CHECK(*directives.bin == "legacy/path");
}

TEST_CASE("parse_envy_meta extracts bin with path separators") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "../sibling/tools"
PACKAGES = {}
)") };

  REQUIRE(directives.bin.has_value());
  CHECK(*directives.bin == "../sibling/tools");
}

TEST_CASE("manifest::load errors on missing bin directive") {
  char const *script{ R"(
-- @envy version "1.0.0"
PACKAGES = {}
)" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Manifest missing required '@envy bin' directive.\n"
                       "Add to manifest header, e.g.: -- @envy bin \"tools\"",
                       std::runtime_error);
}

// ============================================================================
// @envy deploy directive tests
// ============================================================================

TEST_CASE("parse_envy_meta extracts deploy true") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
-- @envy deploy "true"
PACKAGES = {}
)") };

  REQUIRE(directives.deploy.has_value());
  CHECK(*directives.deploy == true);
}

TEST_CASE("parse_envy_meta extracts deploy false") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
-- @envy deploy "false"
PACKAGES = {}
)") };

  REQUIRE(directives.deploy.has_value());
  CHECK(*directives.deploy == false);
}

TEST_CASE("parse_envy_meta deploy absent yields nullopt") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
PACKAGES = {}
)") };

  CHECK_FALSE(directives.deploy.has_value());
}

TEST_CASE("parse_envy_meta ignores invalid deploy value") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
-- @envy deploy "invalid"
PACKAGES = {}
)") };

  // Invalid boolean strings result in nullopt
  CHECK_FALSE(directives.deploy.has_value());
}

// ============================================================================
// @envy root directive tests
// ============================================================================

TEST_CASE("parse_envy_meta extracts root true") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
-- @envy root "true"
PACKAGES = {}
)") };

  REQUIRE(directives.root.has_value());
  CHECK(*directives.root == true);
}

TEST_CASE("parse_envy_meta extracts root false") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
-- @envy root "false"
PACKAGES = {}
)") };

  REQUIRE(directives.root.has_value());
  CHECK(*directives.root == false);
}

TEST_CASE("parse_envy_meta root absent yields nullopt") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
PACKAGES = {}
)") };

  CHECK_FALSE(directives.root.has_value());
}

TEST_CASE("parse_envy_meta ignores invalid root value") {
  auto directives{ envy::parse_envy_meta(R"(
-- @envy bin "tools"
-- @envy root "maybe"
PACKAGES = {}
)") };

  // Invalid boolean strings result in nullopt
  CHECK_FALSE(directives.root.has_value());
}

// ============================================================================
// PACKAGE_DEPOTS global tests
// ============================================================================

TEST_CASE("parse_envy_meta: package-depot directive is a hard error") {
  CHECK_THROWS_WITH_AS(envy::parse_envy_meta(R"(
-- @envy bin "tools"
-- @envy package-depot "https://example.com/depot.txt"
PACKAGES = {}
)"),
                       doctest::Contains("'@envy package-depot' directive removed"),
                       std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: absent yields empty depot config") {
  auto m{ envy::manifest::load("-- @envy bin \"tools\"\nPACKAGES = {}",
                               fs::path("/fake/envy.lua")) };
  CHECK(m->package_depots.empty());
}

TEST_CASE("PACKAGE_DEPOTS: single string entry") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { "https://example.com/depot.txt" }
)",
                               fs::path("/fake/envy.lua")) };

  REQUIRE(m->package_depots.size() == 1);
  auto const *uri{ std::get_if<envy::manifest::depot_uri>(&m->package_depots[0]) };
  REQUIRE(uri);
  CHECK(uri->url == "https://example.com/depot.txt");
}

TEST_CASE("PACKAGE_DEPOTS: multiple string entries keep declaration order") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  "https://example.com/darwin-arm64.txt",
  "https://example.com/linux-x86_64.txt",
  "s3://bucket/depot.txt",
}
)",
                               fs::path("/fake/envy.lua")) };

  REQUIRE(m->package_depots.size() == 3);
  CHECK(std::get<envy::manifest::depot_uri>(m->package_depots[0]).url ==
        "https://example.com/darwin-arm64.txt");
  CHECK(std::get<envy::manifest::depot_uri>(m->package_depots[1]).url ==
        "https://example.com/linux-x86_64.txt");
  CHECK(std::get<envy::manifest::depot_uri>(m->package_depots[2]).url ==
        "s3://bucket/depot.txt");
}

TEST_CASE("PACKAGE_DEPOTS: computed string entry captures evaluated value") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { "https://example.com/depot-" .. envy.PLATFORM_ARCH .. ".txt" }
)",
                               fs::path("/fake/envy.lua")) };

  REQUIRE(m->package_depots.size() == 1);
  auto const &url{ std::get<envy::manifest::depot_uri>(m->package_depots[0]).url };
  CHECK(url.starts_with("https://example.com/depot-"));
  CHECK(url.ends_with(".txt"));
  CHECK(url.size() > std::string{ "https://example.com/depot-.txt" }.size());
}

TEST_CASE("PACKAGE_DEPOTS: table entry with FETCH only has empty DEPENDS") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { FETCH = function(ctx) return {} end } }
)",
                               fs::path("/fake/envy.lua")) };

  REQUIRE(m->package_depots.size() == 1);
  auto const *fn{ std::get_if<envy::manifest::depot_fetch_fn>(&m->package_depots[0]) };
  REQUIRE(fn);
  CHECK(fn->depends.empty());
  CHECK(fn->lua_index == 1);
}

TEST_CASE("PACKAGE_DEPOTS: table entry captures DEPENDS identities") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  {
    DEPENDS = { "tools.jfrog@v1", "tools.aws@v2" },
    FETCH = function(ctx) return {} end,
  },
}
)",
                               fs::path("/fake/envy.lua")) };

  REQUIRE(m->package_depots.size() == 1);
  auto const &fn{ std::get<envy::manifest::depot_fetch_fn>(m->package_depots[0]) };
  REQUIRE(fn.depends.size() == 2);
  CHECK(fn.depends[0] == "tools.jfrog@v1");
  CHECK(fn.depends[1] == "tools.aws@v2");
}

TEST_CASE("PACKAGE_DEPOTS: mixed string and table entries") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  "https://cdn.example.com/public.txt",
  { FETCH = function(ctx) return {} end },
  "s3://bucket/depot.txt",
}
)",
                               fs::path("/fake/envy.lua")) };

  REQUIRE(m->package_depots.size() == 3);
  CHECK(std::holds_alternative<envy::manifest::depot_uri>(m->package_depots[0]));
  auto const &fn{ std::get<envy::manifest::depot_fetch_fn>(m->package_depots[1]) };
  CHECK(fn.lua_index == 2);
  CHECK(std::holds_alternative<envy::manifest::depot_uri>(m->package_depots[2]));
}

TEST_CASE("PACKAGE_DEPOTS: non-table global is an error") {
  CHECK_THROWS_WITH_AS(
      envy::manifest::load("-- @envy bin \"tools\"\nPACKAGES = {}\nPACKAGE_DEPOTS = 42",
                           fs::path("/fake/envy.lua")),
      doctest::Contains("PACKAGE_DEPOTS must be a table"),
      std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: entry neither string nor table is an error") {
  CHECK_THROWS_WITH_AS(
      envy::manifest::load(
          "-- @envy bin \"tools\"\nPACKAGES = {}\nPACKAGE_DEPOTS = { 42 }",
          fs::path("/fake/envy.lua")),
      doctest::Contains("must be a URI string or a {DEPENDS, FETCH} table"),
      std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: empty string entry is an error") {
  CHECK_THROWS_WITH_AS(
      envy::manifest::load(
          "-- @envy bin \"tools\"\nPACKAGES = {}\nPACKAGE_DEPOTS = { \"\" }",
          fs::path("/fake/envy.lua")),
      doctest::Contains("non-empty URI string"),
      std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: table entry without FETCH is an error") {
  CHECK_THROWS_WITH_AS(
      envy::manifest::load(
          "-- @envy bin \"tools\"\nPACKAGES = {}\nPACKAGE_DEPOTS = { { DEPENDS = {} } }",
          fs::path("/fake/envy.lua")),
      doctest::Contains("requires a FETCH function"),
      std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: non-function FETCH is an error") {
  CHECK_THROWS_WITH_AS(
      envy::manifest::load(
          "-- @envy bin \"tools\"\nPACKAGES = {}\nPACKAGE_DEPOTS = { { FETCH = \"x\" } }",
          fs::path("/fake/envy.lua")),
      doctest::Contains("requires a FETCH function"),
      std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: non-table DEPENDS is an error") {
  CHECK_THROWS_WITH_AS(envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { DEPENDS = "tools.jfrog@v1", FETCH = function() end } }
)",
                                            fs::path("/fake/envy.lua")),
                       doctest::Contains("DEPENDS must be a table"),
                       std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: non-string DEPENDS entry is an error") {
  CHECK_THROWS_WITH_AS(envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { DEPENDS = { 42 }, FETCH = function() end } }
)",
                                            fs::path("/fake/envy.lua")),
                       doctest::Contains("DEPENDS entries must be non-empty strings"),
                       std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: run_depot_fetch passes ctx and returns raw text") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  {
    DEPENDS = { "tools.jfrog@v1" },
    FETCH = function(ctx)
      return ctx.tmp_dir .. "|" .. ctx.deps["tools.jfrog@v1"].pkg_path
    end,
  },
}
)",
                               fs::path("/fake/envy.lua")) };

  auto const result{ m->run_depot_fetch(1,
                                        nullptr,
                                        fs::path("/fake/tmp"),
                                        { { "tools.jfrog@v1", "/fake/pkg" } }) };
  auto const *text{ std::get_if<std::string>(&result) };
  REQUIRE(text);
  CHECK(*text == "/fake/tmp|/fake/pkg");
}

TEST_CASE("PACKAGE_DEPOTS: run_depot_fetch converts entries table") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = {
  {
    FETCH = function(ctx)
      return {
        { url = "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst",
          sha256 = string.rep("a", 64) },
        { url = "/local/pkg@v2-linux-x86_64-blake3-bbbb.tar.zst" },
      }
    end,
  },
}
)",
                               fs::path("/fake/envy.lua")) };

  auto const result{ m->run_depot_fetch(1, nullptr, fs::path("/fake/tmp"), {}) };
  auto const *entries{ std::get_if<std::vector<envy::depot_entry>>(&result) };
  REQUIRE(entries);
  REQUIRE(entries->size() == 2);
  CHECK((*entries)[0].url == "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst");
  REQUIRE((*entries)[0].sha256.has_value());
  CHECK(*(*entries)[0].sha256 == std::string(64, 'a'));
  CHECK((*entries)[1].url == "/local/pkg@v2-linux-x86_64-blake3-bbbb.tar.zst");
  CHECK_FALSE((*entries)[1].sha256.has_value());
}

TEST_CASE("PACKAGE_DEPOTS: run_depot_fetch rejects invalid return shape") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { FETCH = function(ctx) return 42 end } }
)",
                               fs::path("/fake/envy.lua")) };

  CHECK_THROWS_WITH_AS(m->run_depot_fetch(1, nullptr, fs::path("/fake/tmp"), {}),
                       doctest::Contains("FETCH must return"),
                       std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: run_depot_fetch surfaces Lua errors") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { FETCH = function(ctx) error("boom") end } }
)",
                               fs::path("/fake/envy.lua")) };

  CHECK_THROWS_WITH_AS(m->run_depot_fetch(1, nullptr, fs::path("/fake/tmp"), {}),
                       doctest::Contains("FETCH failed"),
                       std::runtime_error);
}

TEST_CASE("PACKAGE_DEPOTS: run_depot_fetch rejects entry without url") {
  auto m{ envy::manifest::load(R"(-- @envy bin "tools"
PACKAGES = {}
PACKAGE_DEPOTS = { { FETCH = function(ctx) return { { sha256 = string.rep("a", 64) } } end } }
)",
                               fs::path("/fake/envy.lua")) };

  CHECK_THROWS_WITH_AS(m->run_depot_fetch(1, nullptr, fs::path("/fake/tmp"), {}),
                       doctest::Contains("require a non-empty 'url'"),
                       std::runtime_error);
}

// ============================================================================
// manifest::discover() with root directive tests
// ============================================================================

TEST_CASE("manifest::discover with root=false continues search upward") {
  // Create temp structure: parent/child where child has root=false
  auto temp_root{ fs::canonical(fs::temp_directory_path()) / "envy-test-root-false" };
  auto parent_dir{ temp_root / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  // Parent manifest: root=true (default, stops search)
  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\nPACKAGES = {}\n";
  parent_manifest.close();

  // Child manifest: root=false (continues search)
  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(false, child_dir) };

  // Should find parent (root=true) instead of child (root=false)
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == parent_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover with root=true stops immediately") {
  auto temp_root{ fs::canonical(fs::temp_directory_path()) / "envy-test-root-true" };
  auto parent_dir{ temp_root / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  // Parent manifest
  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\nPACKAGES = {}\n";
  parent_manifest.close();

  // Child manifest: root=true (default)
  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"true\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(false, child_dir) };

  // Should stop at child (root=true)
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == child_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover with all root=false uses closest to filesystem root") {
  // Create 3-level structure: grandparent/parent/child, all root=false
  auto temp_root{ fs::canonical(fs::temp_directory_path()) / "envy-test-all-root-false" };
  auto grandparent_dir{ temp_root / "grandparent" };
  auto parent_dir{ grandparent_dir / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  // All manifests have root=false
  std::ofstream grandparent_manifest{ grandparent_dir / "envy.lua" };
  grandparent_manifest
      << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  grandparent_manifest.close();

  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  parent_manifest.close();

  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(false, child_dir) };

  // Should use grandparent (closest to filesystem root among non-roots)
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == grandparent_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover F-T-F uses middle (root=true) manifest") {
  // 3-level: grandparent(F) -> parent(T) -> child(F)
  auto temp_root{ fs::canonical(fs::temp_directory_path()) / "envy-test-ftf" };
  auto grandparent_dir{ temp_root / "grandparent" };
  auto parent_dir{ grandparent_dir / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  std::ofstream grandparent_manifest{ grandparent_dir / "envy.lua" };
  grandparent_manifest
      << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  grandparent_manifest.close();

  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\n-- @envy root \"true\"\nPACKAGES = {}\n";
  parent_manifest.close();

  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(false, child_dir) };

  // Should stop at parent (root=true)
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == parent_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover F-F with no grandparent uses parent") {
  // 2-level: parent(F) -> child(F), no grandparent manifest
  auto temp_root{ fs::canonical(fs::temp_directory_path()) /
                  "envy-test-ff-no-grandparent" };
  auto parent_dir{ temp_root / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  parent_manifest.close();

  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(false, child_dir) };

  // Should use parent (closest to root among non-roots, no grandparent manifest exists)
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == parent_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover F with no parent skips to grandparent") {
  // 3-level: grandparent(F) -> parent(no manifest) -> child(F)
  auto temp_root{ fs::canonical(fs::temp_directory_path()) /
                  "envy-test-f-skip-grandparent" };
  auto grandparent_dir{ temp_root / "grandparent" };
  auto parent_dir{ grandparent_dir / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  std::ofstream grandparent_manifest{ grandparent_dir / "envy.lua" };
  grandparent_manifest
      << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  grandparent_manifest.close();

  // No manifest in parent_dir

  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(false, child_dir) };

  // Should use grandparent (closest to root, skipping parent which has no manifest)
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == grandparent_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover with only child manifest (root=false) uses child") {
  // Single manifest with root=false, no parents
  auto temp_root{ fs::canonical(fs::temp_directory_path()) /
                  "envy-test-single-root-false" };
  auto child_dir{ temp_root / "child" };

  fs::create_directories(child_dir);

  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(false, child_dir) };

  // Should use child even though root=false (only manifest in tree)
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == child_dir);

  fs::remove_all(temp_root);
}

// ============================================================================
// manifest::discover(nearest=true) tests
// ============================================================================

TEST_CASE("manifest::discover nearest returns first envy.lua found") {
  // Create parent/child where child has root=false, parent has root=true
  auto temp_root{ fs::canonical(fs::temp_directory_path()) / "envy-test-nearest-basic" };
  auto parent_dir{ temp_root / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\nPACKAGES = {}\n";
  parent_manifest.close();

  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  child_manifest.close();

  // Normal discover walks up to parent (root=true)
  auto normal_result{ envy::manifest::discover(false, child_dir) };
  REQUIRE(normal_result.has_value());
  CHECK(normal_result->path.parent_path() == parent_dir);

  // Nearest discover returns child immediately
  auto nearest_result{ envy::manifest::discover(true, child_dir) };
  REQUIRE(nearest_result.has_value());
  CHECK(nearest_result->path.parent_path() == child_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover nearest ignores root directive") {
  // Child has root=true, but nearest should still return it (first found)
  auto temp_root{ fs::canonical(fs::temp_directory_path()) /
                  "envy-test-nearest-root-true" };
  auto parent_dir{ temp_root / "parent" };
  auto child_dir{ parent_dir / "child" };

  fs::create_directories(child_dir);

  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\nPACKAGES = {}\n";
  parent_manifest.close();

  std::ofstream child_manifest{ child_dir / "envy.lua" };
  child_manifest << "-- @envy bin \"tools\"\n-- @envy root \"true\"\nPACKAGES = {}\n";
  child_manifest.close();

  auto result{ envy::manifest::discover(true, child_dir) };
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == child_dir);

  fs::remove_all(temp_root);
}

TEST_CASE("manifest::discover nearest from subdirectory without manifest") {
  // Start in a subdirectory that has no envy.lua; nearest should find parent's
  auto temp_root{ fs::canonical(fs::temp_directory_path()) / "envy-test-nearest-subdir" };
  auto parent_dir{ temp_root / "parent" };
  auto sub_dir{ parent_dir / "subdir" };

  fs::create_directories(sub_dir);

  std::ofstream parent_manifest{ parent_dir / "envy.lua" };
  parent_manifest << "-- @envy bin \"tools\"\n-- @envy root \"false\"\nPACKAGES = {}\n";
  parent_manifest.close();

  auto result{ envy::manifest::discover(true, sub_dir) };
  REQUIRE(result.has_value());
  CHECK(result->path.parent_path() == parent_dir);

  fs::remove_all(temp_root);
}

// ============================================================================
// BUNDLES table tests
// ============================================================================

TEST_CASE("manifest::load parses package with bundle alias") {
  char const *script{ R"(
    -- @envy bin "tools"
    BUNDLES = {
      toolchain = {
        identity = "acme.toolchain@v1",
        source = "https://example.com/toolchain.tar.gz"
      }
    }
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        bundle = "toolchain"
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");
  CHECK(m->packages[0]->is_bundle_source());
  CHECK(m->packages[0]->bundle_identity.has_value());
  CHECK(*m->packages[0]->bundle_identity == "acme.toolchain@v1");

  auto const *bundle_src{ std::get_if<envy::pkg_cfg::bundle_source>(
      &m->packages[0]->source) };
  REQUIRE(bundle_src);
  CHECK(bundle_src->bundle_identity == "acme.toolchain@v1");
}

TEST_CASE("manifest::load parses package with inline bundle") {
  char const *script{ R"(
    -- @envy bin "tools"
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        bundle = {
          identity = "inline.bundle@v1",
          source = "https://example.com/inline.tar.gz"
        }
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");
  CHECK(m->packages[0]->is_bundle_source());
  CHECK(*m->packages[0]->bundle_identity == "inline.bundle@v1");
}

TEST_CASE("manifest::load errors on unknown bundle alias") {
  char const *script{ R"(
    -- @envy bin "tools"
    BUNDLES = {}
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        bundle = "nonexistent"
      }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Bundle alias 'nonexistent' not found in BUNDLES table for spec "
                       "'arm.gcc@v2'",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on package with both source and bundle") {
  char const *script{ R"(
    -- @envy bin "tools"
    BUNDLES = {
      tc = { identity = "acme.tc@v1", source = "https://example.com/tc.tar.gz" }
    }
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        source = "https://example.com/gcc.lua",
        bundle = "tc"
      }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Package cannot specify both 'source' and 'bundle' fields",
                       std::runtime_error);
}

TEST_CASE("manifest::load errors on bundle package without spec") {
  char const *script{ R"(
    -- @envy bin "tools"
    BUNDLES = {
      tc = { identity = "acme.tc@v1", source = "https://example.com/tc.tar.gz" }
    }
    PACKAGES = {
      {
        bundle = "tc"
      }
    }
  )" };

  CHECK_THROWS_WITH_AS(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                       "Package with 'bundle' field requires 'spec' field",
                       std::runtime_error);
}

TEST_CASE("manifest::load parses package with bundle and options") {
  char const *script{ R"(
    -- @envy bin "tools"
    BUNDLES = {
      tc = { identity = "acme.tc@v1", source = "https://example.com/tc.tar.gz" }
    }
    PACKAGES = {
      {
        spec = "arm.gcc@v2",
        bundle = "tc",
        options = { version = "13.2.0" }
      }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");
  CHECK(m->packages[0]->is_bundle_source());

  // Check options are preserved
  sol::state lua;
  auto opts_result{ lua.safe_script("return " + m->packages[0]->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  CHECK(sol::object(opts["version"]).as<std::string>() == "13.2.0");
}

// --- platforms field parsing ---

TEST_CASE("manifest::load parses platforms on non-bundle package") {
  char const *script{ R"(
    -- @envy bin "tools"
    PACKAGES = {
      { spec = "local.apt@v1", source = "/fake/apt.lua", platforms = { "linux" } }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "local.apt@v1");
  REQUIRE(m->packages[0]->platforms.size() == 1);
  CHECK(m->packages[0]->platforms[0] == "linux");
}

TEST_CASE("manifest::load parses multiple platforms") {
  char const *script{ R"(
    -- @envy bin "tools"
    PACKAGES = {
      { spec = "local.ragel@v1", source = "/fake/ragel.lua",
        platforms = { "darwin", "linux" } }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  REQUIRE(m->packages[0]->platforms.size() == 2);
  CHECK(m->packages[0]->platforms[0] == "darwin");
  CHECK(m->packages[0]->platforms[1] == "linux");
}

TEST_CASE("manifest::load omitted platforms field yields empty vector") {
  char const *script{ R"(
    -- @envy bin "tools"
    PACKAGES = {
      { spec = "arm.gcc@v2", source = "/fake/r.lua" }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->platforms.empty());
}

TEST_CASE("manifest::load parses platforms on bundle package") {
  char const *script{ R"(
    -- @envy bin "tools"
    BUNDLES = {
      tc = { identity = "acme.tc@v1", source = "https://example.com/tc.tar.gz" }
    }
    PACKAGES = {
      { spec = "arm.gcc@v2", bundle = "tc", platforms = { "linux", "darwin-arm64" } }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  CHECK(m->packages[0]->identity == "arm.gcc@v2");
  REQUIRE(m->packages[0]->platforms.size() == 2);
  CHECK(m->packages[0]->platforms[0] == "linux");
  CHECK(m->packages[0]->platforms[1] == "darwin-arm64");
}

TEST_CASE("manifest::load errors on non-string platforms entry") {
  char const *script{ R"(
    -- @envy bin "tools"
    PACKAGES = {
      { spec = "local.tool@v1", source = "/fake/tool.lua", platforms = { 42 } }
    }
  )" };

  CHECK_THROWS_WITH(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                    "platforms entries must be strings");
}

TEST_CASE("manifest::load platforms on os-arch constraint") {
  char const *script{ R"(
    -- @envy bin "tools"
    PACKAGES = {
      { spec = "local.tool@v1", source = "/fake/tool.lua",
        platforms = { "darwin-arm64", "linux-x86_64" } }
    }
  )" };

  auto m{ envy::manifest::load(script, fs::path("/fake/envy.lua")) };

  REQUIRE(m->packages.size() == 1);
  REQUIRE(m->packages[0]->platforms.size() == 2);
  CHECK(m->packages[0]->platforms[0] == "darwin-arm64");
  CHECK(m->packages[0]->platforms[1] == "linux-x86_64");
}

TEST_CASE("manifest::load errors on non-table platforms value") {
  char const *script{ R"(
    -- @envy bin "tools"
    PACKAGES = {
      { spec = "local.tool@v1", source = "/fake/tool.lua", platforms = "linux" }
    }
  )" };

  CHECK_THROWS_WITH(envy::manifest::load(script, fs::path("/fake/envy.lua")),
                    "platforms must be a table");
}

// --- @envy sha256sums ---

TEST_CASE("parse_envy_meta extracts sha256sums") {
  auto meta{ envy::parse_envy_meta(R"(
-- @envy version "1.2.3"
-- @envy sha256sums "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
PACKAGES = {}
)") };

  REQUIRE(meta.sha256sums.has_value());
  CHECK(*meta.sha256sums ==
        "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
}

TEST_CASE("parse_envy_meta sha256sums absent by default") {
  auto meta{ envy::parse_envy_meta("-- @envy version \"1.2.3\"\nPACKAGES = {}\n") };
  CHECK_FALSE(meta.sha256sums.has_value());
}

TEST_CASE("parse_envy_meta rejects a malformed sha256sums pin") {
  // Fail loudly instead of storing a pin that can never match: a truncated or typo'd hash
  // would otherwise turn every bootstrap into an attestation failure at download time, far
  // from the manifest that caused it.
  CHECK_THROWS_AS(envy::parse_envy_meta("-- @envy version \"1.2.3\"\n"
                                        "-- @envy sha256sums \"deadbeef\"\n"),
                  std::runtime_error);
  CHECK_THROWS_AS(envy::parse_envy_meta("-- @envy version \"1.2.3\"\n"
                                        "-- @envy sha256sums \"\"\n"),
                  std::runtime_error);
  // 64 characters, but 'g' is not a hex digit.
  CHECK_THROWS_AS(envy::parse_envy_meta("-- @envy version \"1.2.3\"\n"
                                        "-- @envy sha256sums \"" +
                                        std::string(63, 'a') + "g\"\n"),
                  std::runtime_error);
}

TEST_CASE("parse_envy_meta rejects sha256sums without a pinned version") {
  // A sums pin names one release's checksum file. With the version resolved dynamically
  // the pin would describe a different release than the one downloaded, so verification
  // would fail confusingly or -- worse, if treated as optional -- be skipped silently.
  CHECK_THROWS_AS(envy::parse_envy_meta("-- @envy sha256sums \"" + std::string(64, 'a') +
                                        "\"\nPACKAGES = {}\n"),
                  std::runtime_error);
}

TEST_CASE("parse_envy_meta accepts an uppercase sha256sums pin") {
  // certutil and Get-FileHash both emit uppercase, so a hand-pasted pin often is.
  auto meta{ envy::parse_envy_meta(
      "-- @envy version \"1.2.3\"\n"
      "-- @envy sha256sums \"" +
      std::string(64, 'A') + "\"\n") };
  REQUIRE(meta.sha256sums.has_value());
  CHECK(*meta.sha256sums == std::string(64, 'A'));
}

TEST_CASE("parse_envy_meta accepts a mirror containing backslashes") {
  // A Windows `file://` or UNC mirror is a normal value. Enforcing the mirror character
  // set on read, rather than only at `envy init --mirror` where a user-supplied value
  // enters the manifest, rejected these and broke every manifest-aware command for such a
  // project: all 17 test_reexec cases failed on Windows.
  SUBCASE("raw path, as a Windows tool or user writes it") {
    // `\U` is not a recognized escape, so it survives verbatim.
    auto meta{ envy::parse_envy_meta("-- @envy mirror \"file://C:\\Users\\me\\rel\"\n") };
    REQUIRE(meta.mirror.has_value());
    CHECK(*meta.mirror == "file://C:\\Users\\me\\rel");
  }

  SUBCASE("escaped path, per the documented directive escaping") {
    auto meta{ envy::parse_envy_meta(
        "-- @envy mirror \"file://C:\\\\Users\\\\me\\\\rel\"\n") };
    REQUIRE(meta.mirror.has_value());
    CHECK(*meta.mirror == "file://C:\\Users\\me\\rel");
  }

  SUBCASE("UNC share") {
    auto meta{ envy::parse_envy_meta("-- @envy mirror \"\\\\\\\\srv\\\\envy\"\n") };
    REQUIRE(meta.mirror.has_value());
    CHECK(*meta.mirror == "\\\\srv\\envy");
  }
}
