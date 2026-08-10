#include "pkg_cfg.h"

#include "sol/sol.hpp"

#include "doctest.h"

#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Helper to parse a Lua string into a sol::object
sol::object lua_eval(char const *script, sol::state &lua) {
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

TEST_CASE("pkg_cfg::parse rejects string shorthand") {
  sol::state lua;
  auto lua_val{ lua_eval("result = 'arm.gcc@v2'", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       doctest::Contains("shorthand string syntax requires table"),
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse rejects table without source") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'gnu.binutils@v3' }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       doctest::Contains("must specify 'source' field"),
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse allows reference-only dependency when enabled") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'python' }", lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake"), true) };

  CHECK(cfg->identity == "python");
  CHECK(cfg->is_weak_reference());
  CHECK(cfg->weak == nullptr);
}

TEST_CASE("pkg_cfg::parse allows weak dependency with fallback when enabled") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'python', weak = { spec = 'vendor.python@r4', source = "
      "'/fake/python.lua' } }",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake"), true) };

  CHECK(cfg->identity == "python");
  CHECK(cfg->is_weak_reference());
  REQUIRE(cfg->weak);
  CHECK(cfg->weak->identity == "vendor.python@r4");
  CHECK(std::holds_alternative<envy::pkg_cfg::local_source>(cfg->weak->source));
}

TEST_CASE("pkg_cfg::parse parses table with remote source") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'arm.gcc@v2', source = 'https://example.com/gcc.lua', sha256 = "
      "'abc123' }",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  CHECK(cfg->identity == "arm.gcc@v2");

  auto const *remote{ std::get_if<envy::pkg_cfg::remote_source>(&cfg->source) };
  REQUIRE(remote != nullptr);
  CHECK(remote->url == "https://example.com/gcc.lua");
  CHECK(remote->sha256 == "abc123");
}

TEST_CASE("pkg_cfg::parse parses table with local source") {
  sol::state lua;
  auto lua_val{
    lua_eval("result = { spec = 'local.tool@v1', source = './specs/tool.lua' }", lua)
  };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/project/envy.lua")) };

  CHECK(cfg->identity == "local.tool@v1");

  auto const *local{ std::get_if<envy::pkg_cfg::local_source>(&cfg->source) };
  REQUIRE(local != nullptr);
  CHECK(local->file_path == fs::path("/project/specs/tool.lua"));
}

TEST_CASE("pkg_cfg::parse resolves relative file paths") {
  sol::state lua;
  auto lua_val{
    lua_eval("result = { spec = 'local.tool@v1', source = '../sibling/tool.lua' }", lua)
  };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/project/sub/envy.lua")) };

  auto const *local{ std::get_if<envy::pkg_cfg::local_source>(&cfg->source) };
  REQUIRE(local != nullptr);
  CHECK(local->file_path == fs::path("/project/sibling/tool.lua"));
}

TEST_CASE("pkg_cfg::parse parses table with options") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'arm.gcc@v2', source = '/fake/r.lua', options = { version = "
      "'13.2.0', target = "
      "'arm-none-eabi' } }",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  CHECK(cfg->identity == "arm.gcc@v2");

  // Deserialize and check
  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  CHECK(sol::object(opts["version"]).as<std::string>() == "13.2.0");
  CHECK(sol::object(opts["target"]).as<std::string>() == "arm-none-eabi");
}

TEST_CASE("pkg_cfg::parse parses table with empty options") {
  sol::state lua;
  auto lua_val{
    lua_eval("result = { spec = 'arm.gcc@v2', source = '/fake/r.lua', options = {} }", lua)
  };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  CHECK(cfg->identity == "arm.gcc@v2");
  CHECK(cfg->serialized_options == "{}");
}

TEST_CASE("pkg_cfg::parse parses table with all fields") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'arm.gcc@v2', source = 'https://example.com/gcc.lua', sha256 = "
      "'abc123', "
      "options = { version = '13.2.0' } }",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  CHECK(cfg->identity == "arm.gcc@v2");

  auto const *remote{ std::get_if<envy::pkg_cfg::remote_source>(&cfg->source) };
  REQUIRE(remote != nullptr);
  CHECK(remote->url == "https://example.com/gcc.lua");
  CHECK(remote->sha256 == "abc123");

  // Deserialize and check
  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  CHECK(sol::object(opts["version"]).as<std::string>() == "13.2.0");
}

TEST_CASE("pkg_cfg::parse parses product dependency fields") {
  SUBCASE("strong product dependency") {
    sol::state lua;
    auto lua_val{ lua_eval(
        "result = { spec = 'local.provider@v1', product = 'tool', source = "
        "'/fake/provider.lua' }",
        lua) };

    auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake"), true) };

    CHECK(cfg->product.has_value());
    CHECK(*cfg->product == "tool");
    CHECK_FALSE(cfg->is_weak_reference());
  }

  SUBCASE("weak product dependency with fallback") {
    sol::state lua;
    auto lua_val{ lua_eval(
        "result = { spec = 'local.consumer@v1', product = 'tool', weak = { spec = "
        "'vendor.tool@v1', source = '/fake/tool.lua' } }",
        lua) };

    auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake"), true) };

    CHECK(cfg->product.has_value());
    CHECK(cfg->is_weak_reference());
    REQUIRE(cfg->weak);
    CHECK_FALSE(cfg->weak->product.has_value());
  }

  SUBCASE("ref-only product dependency unconstrained") {
    sol::state lua;
    auto lua_val{ lua_eval("result = { product = 'tool' }",  // No spec field
                           lua) };

    auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake"), true) };

    CHECK(cfg->product.has_value());
    CHECK(*cfg->product == "tool");
    CHECK(cfg->identity.empty());  // Empty identity means unconstrained
    CHECK(cfg->is_weak_reference());
    CHECK(cfg->weak == nullptr);
  }

  SUBCASE("ref-only product dependency constrained") {
    sol::state lua;
    auto lua_val{ lua_eval("result = { spec = 'local.consumer@v1', product = 'tool' }",
                           lua) };

    auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake"), true) };

    CHECK(cfg->product.has_value());
    CHECK(*cfg->product == "tool");
    CHECK(cfg->identity == "local.consumer@v1");  // Constraint identity
    CHECK(cfg->is_weak_reference());
    CHECK(cfg->weak == nullptr);
  }

  SUBCASE("rejects non-string product") {
    sol::state lua;
    auto lua_val{
      lua_eval("result = { spec = 'foo@v1', product = 42, source = '/fake/foo.lua' }", lua)
    };

    CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                         doctest::Contains("product"),
                         std::runtime_error);
  }

  SUBCASE("rejects empty product") {
    sol::state lua;
    auto lua_val{
      lua_eval("result = { spec = 'foo@v1', product = '', source = '/fake/foo.lua' }", lua)
    };

    CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                         doctest::Contains("cannot be empty"),
                         std::runtime_error);
  }
}

// Error cases ----------------------------------------------------------------

TEST_CASE("pkg_cfg::parse errors on invalid identity format") {
  sol::state lua;
  auto lua_val{
    lua_eval("result = { spec = 'invalid-no-at-sign', source = '/fake/r.lua' }", lua)
  };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Invalid spec identity format: invalid-no-at-sign",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on identity missing namespace") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'gcc@v2', source = '/fake/r.lua' }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Invalid spec identity format: gcc@v2",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on identity missing name") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'arm.@v2', source = '/fake/r.lua' }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Invalid spec identity format: arm.@v2",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on identity missing version") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'arm.gcc@', source = '/fake/r.lua' }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Invalid spec identity format: arm.gcc@",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on identity missing @ sign") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'arm.gcc', source = '/fake/r.lua' }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Invalid spec identity format: arm.gcc",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on identity missing dot") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'armgcc@v2', source = '/fake/r.lua' }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Invalid spec identity format: armgcc@v2",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on non-string and non-table value") {
  sol::state lua;
  auto lua_val{ lua_eval("result = 123", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Spec entry must be string or table",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on table missing spec field") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { source = 'https://example.com/foo.lua' }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Spec table missing required 'spec' field",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on non-string spec field") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 123 }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Spec: spec must be a string",
                       std::runtime_error);
}

// Test removed - can no longer specify both url and file since we unified to 'source'

TEST_CASE("pkg_cfg::parse allows url without sha256 (permissive mode)") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'arm.gcc@v2', source = 'https://example.com/gcc.lua' }",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };
  CHECK(cfg->identity == "arm.gcc@v2");
  CHECK(cfg->is_remote());
  auto const *remote{ std::get_if<envy::pkg_cfg::remote_source>(&cfg->source) };
  REQUIRE(remote != nullptr);
  CHECK(remote->url == "https://example.com/gcc.lua");
  CHECK(remote->sha256.empty());  // No SHA256 provided (permissive)
}

TEST_CASE("pkg_cfg::parse errors on non-string source") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'arm.gcc@v2', source = 123, sha256 = 'abc' }",
                         lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Spec 'source' field must be string or table",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on non-string sha256") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'arm.gcc@v2', source = 'https://example.com/gcc.lua', sha256 = "
      "123 "
      "}",
      lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Spec source: sha256 must be a string",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on non-string source (local)") {
  sol::state lua;
  auto lua_val{ lua_eval("result = { spec = 'local.tool@v1', source = 123 }", lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Spec 'source' field must be string or table",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on non-table options") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'arm.gcc@v2', source = '/fake/r.lua', options = 'not a table' "
      "}",
      lua) };

  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::path("/fake")),
                       "Spec 'options' field must be table",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse accepts non-string option values") {
  sol::state lua;
  auto lua_val{ lua_eval(
      "result = { spec = 'arm.gcc@v2', source = '/fake/r.lua', options = { version = "
      "123, "
      "debug = true, nested = { key = 'value' } } }",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  // Deserialize and check
  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  CHECK(sol::object(opts["version"]).is<lua_Integer>());
  CHECK(sol::object(opts["version"]).as<int64_t>() == 123);
  CHECK(sol::object(opts["debug"]).is<bool>());
  CHECK(sol::object(opts["debug"]).as<bool>() == true);
  CHECK(sol::object(opts["nested"]).is<sol::table>());
}

TEST_CASE("pkg_cfg::parse errors on function in options") {
  sol::state lua;
  // lua_eval succeeds (functions become placeholders), but parse rejects them
  auto lua_val{ lua_eval(R"(
    result = {
      spec = 'arm.gcc@v2',
      source = '/fake/r.lua',
      options = { func = function() return 42 end }
    }
  )",
                         lua) };
  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::current_path()),
                       "Unsupported Lua type: function",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse errors on function nested in options") {
  sol::state lua;
  // lua_eval succeeds (functions become placeholders), but parse rejects them
  auto lua_val{ lua_eval(R"(
    result = {
      spec = 'arm.gcc@v2',
      source = '/fake/r.lua',
      options = {
        compiler = {
          version = '13.2',
          callback = function() return true end
        }
      }
    }
  )",
                         lua) };
  CHECK_THROWS_WITH_AS(envy::pkg_cfg::parse(lua_val, fs::current_path()),
                       "Unsupported Lua type: function",
                       std::runtime_error);
}

TEST_CASE("pkg_cfg::parse serializes simple string array") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'brew.pkg@r0', source = '/fake/r.lua',
                    options = { packages = { "neovim", "bat", "pv" } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  // Deserialize and check
  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table packages = opts["packages"];
  REQUIRE(packages.size() == 3);
  CHECK(packages[1].get<std::string>() == "neovim");
  CHECK(packages[2].get<std::string>() == "bat");
  CHECK(packages[3].get<std::string>() == "pv");
}

TEST_CASE("pkg_cfg::parse serializes integer array") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { ports = { 8080, 8081, 8082 } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table ports = opts["ports"];
  REQUIRE(ports.size() == 3);
  CHECK(ports[1].get<lua_Integer>() == 8080);
  CHECK(ports[2].get<lua_Integer>() == 8081);
  CHECK(ports[3].get<lua_Integer>() == 8082);
}

TEST_CASE("pkg_cfg::parse serializes mixed-type array") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { mixed = { "str", 42, true, "end" } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table mixed = opts["mixed"];
  REQUIRE(mixed.size() == 4);
  CHECK(mixed[1].get<std::string>() == "str");
  CHECK(mixed[2].get<lua_Integer>() == 42);
  CHECK(mixed[3].get<bool>() == true);
  CHECK(mixed[4].get<std::string>() == "end");
}

TEST_CASE("pkg_cfg::parse serializes float array") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { values = { 1.5, 2.5, 3.5 } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table values = opts["values"];
  REQUIRE(values.size() == 3);
  CHECK(values[1].get<lua_Number>() == doctest::Approx(1.5));
  CHECK(values[2].get<lua_Number>() == doctest::Approx(2.5));
  CHECK(values[3].get<lua_Number>() == doctest::Approx(3.5));
}

TEST_CASE("pkg_cfg::parse preserves array order") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { items = { "z", "a", "m", "b" } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table items = opts["items"];
  REQUIRE(items.size() == 4);
  // Verify array maintains order, not sorted lexicographically
  CHECK(items[1].get<std::string>() == "z");
  CHECK(items[2].get<std::string>() == "a");
  CHECK(items[3].get<std::string>() == "m");
  CHECK(items[4].get<std::string>() == "b");
}

TEST_CASE("pkg_cfg::parse serializes nested arrays") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { matrix = { { 1, 2 }, { 3, 4 } } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table matrix = opts["matrix"];
  REQUIRE(matrix.size() == 2);
  sol::table row1 = matrix[1];
  sol::table row2 = matrix[2];
  CHECK(row1[1].get<lua_Integer>() == 1);
  CHECK(row1[2].get<lua_Integer>() == 2);
  CHECK(row2[1].get<lua_Integer>() == 3);
  CHECK(row2[2].get<lua_Integer>() == 4);
}

TEST_CASE("pkg_cfg::parse serializes table containing arrays") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { config = { flags = { "-Wall", "-O2" }, level = 3 } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table config = opts["config"];
  sol::table flags = config["flags"];
  REQUIRE(flags.size() == 2);
  CHECK(flags[1].get<std::string>() == "-Wall");
  CHECK(flags[2].get<std::string>() == "-O2");
  CHECK(config["level"].get<lua_Integer>() == 3);
}

TEST_CASE("pkg_cfg::parse serializes array containing tables") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { items = { { name = "foo" }, { name = "bar" } } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table items = opts["items"];
  REQUIRE(items.size() == 2);
  sol::table item1 = items[1];
  sol::table item2 = items[2];
  CHECK(item1["name"].get<std::string>() == "foo");
  CHECK(item2["name"].get<std::string>() == "bar");
}

TEST_CASE("pkg_cfg::parse serializes sparse table as table not array") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { sparse = { [1] = "a", [3] = "c" } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table sparse = opts["sparse"];
  // Sparse table should not serialize as array - only string keys preserved
  // Since numeric keys aren't strings, they'll be dropped by current table logic
  CHECK(sparse.size() == 0);
}

TEST_CASE("pkg_cfg::parse serializes single-element array") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'local.test@r0', source = '/fake/r.lua',
                    options = { singleton = { "only" } } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;
  sol::table singleton = opts["singleton"];
  REQUIRE(singleton.size() == 1);
  CHECK(singleton[1].get<std::string>() == "only");
}

TEST_CASE("pkg_cfg::parse serializes complex real-world options") {
  sol::state lua;
  auto lua_val{ lua_eval(
      R"(result = { spec = 'brew.pkg@r0', source = '/fake/r.lua',
                    options = {
                      packages = { "ghostty", "neovim", "pv", "bat" },
                      version = "1.2.3",
                      flags = { debug = true, optimize = false }
                    } })",
      lua) };

  auto const *cfg{ envy::pkg_cfg::parse(lua_val, fs::path("/fake")) };

  auto opts_result{ lua.safe_script("return " + cfg->serialized_options) };
  REQUIRE(opts_result.valid());
  sol::table opts = opts_result;

  sol::table packages = opts["packages"];
  REQUIRE(packages.size() == 4);
  CHECK(packages[1].get<std::string>() == "ghostty");
  CHECK(packages[2].get<std::string>() == "neovim");
  CHECK(packages[3].get<std::string>() == "pv");
  CHECK(packages[4].get<std::string>() == "bat");

  CHECK(opts["version"].get<std::string>() == "1.2.3");

  sol::table flags = opts["flags"];
  CHECK(flags["debug"].get<bool>() == true);
  CHECK(flags["optimize"].get<bool>() == false);
}

// ==== source equality and bundle declaration comparison ====
//
// pkg_key is identity + options, so two declarations of one bundle identity collapse
// onto a single package; bundle_source_compare is what lets the engine tell an
// agreeing redeclaration from a conflicting one.

namespace {

envy::pkg_cfg::bundle_source remote_bundle(char const *identity,
                                           char const *url,
                                           char const *sha = "") {
  return { .bundle_identity = identity,
           .fetch_source = envy::pkg_cfg::remote_source{ .url = url, .sha256 = sha } };
}

envy::pkg_cfg::bundle_source git_bundle(char const *identity,
                                        char const *url,
                                        char const *ref) {
  return { .bundle_identity = identity,
           .fetch_source = envy::pkg_cfg::git_source{ .url = url, .ref = ref } };
}

envy::pkg_cfg::bundle_source local_bundle(char const *identity, char const *path) {
  return { .bundle_identity = identity,
           .fetch_source = envy::pkg_cfg::local_source{ .file_path = path } };
}

envy::pkg_cfg::bundle_source custom_bundle(char const *identity,
                                           std::vector<envy::pkg_cfg *> deps = {}) {
  return { .bundle_identity = identity,
           .fetch_source =
               envy::pkg_cfg::custom_fetch_source{ .dependencies = std::move(deps) } };
}

}  // namespace

TEST_CASE("remote_source equality covers every field") {
  envy::pkg_cfg::remote_source const base{ .url = "https://x/b.tgz", .sha256 = "aa" };

  CHECK(base == envy::pkg_cfg::remote_source{ .url = "https://x/b.tgz", .sha256 = "aa" });
  CHECK_FALSE(base ==
              envy::pkg_cfg::remote_source{ .url = "https://y/b.tgz", .sha256 = "aa" });
  CHECK_FALSE(base ==
              envy::pkg_cfg::remote_source{ .url = "https://x/b.tgz", .sha256 = "bb" });

  envy::pkg_cfg::remote_source with_subdir{ base };
  with_subdir.subdir = "inner";
  CHECK_FALSE(base == with_subdir);
  CHECK(with_subdir == with_subdir);
}

TEST_CASE("git_source equality covers every field") {
  envy::pkg_cfg::git_source const base{ .url = "git://x/b.git", .ref = "main" };

  CHECK(base == envy::pkg_cfg::git_source{ .url = "git://x/b.git", .ref = "main" });
  CHECK_FALSE(base == envy::pkg_cfg::git_source{ .url = "git://x/b.git", .ref = "dev" });
  CHECK_FALSE(base == envy::pkg_cfg::git_source{ .url = "git://y/b.git", .ref = "main" });

  envy::pkg_cfg::git_source with_subdir{ base };
  with_subdir.subdir = "inner";
  CHECK_FALSE(base == with_subdir);
}

TEST_CASE("local_source equality compares the path") {
  envy::pkg_cfg::local_source const base{ .file_path = "/a/b" };

  CHECK(base == envy::pkg_cfg::local_source{ .file_path = "/a/b" });
  CHECK_FALSE(base == envy::pkg_cfg::local_source{ .file_path = "/a/c" });
}

TEST_CASE("bundle_source_compare: identical declarations are SAME") {
  CHECK(envy::bundle_source_compare(remote_bundle("a.b@v1", "https://x/b.tgz", "aa"),
                                    remote_bundle("a.b@v1", "https://x/b.tgz", "aa")) ==
        envy::bundle_source_match::SAME);
  CHECK(envy::bundle_source_compare(git_bundle("a.b@v1", "git://x/b.git", "main"),
                                    git_bundle("a.b@v1", "git://x/b.git", "main")) ==
        envy::bundle_source_match::SAME);
  CHECK(envy::bundle_source_compare(local_bundle("a.b@v1", "/a/b"),
                                    local_bundle("a.b@v1", "/a/b")) ==
        envy::bundle_source_match::SAME);
}

TEST_CASE("bundle_source_compare: a differing payload is DIFFERENT") {
  CHECK(envy::bundle_source_compare(remote_bundle("a.b@v1", "https://x/b.tgz"),
                                    remote_bundle("a.b@v1", "https://y/b.tgz")) ==
        envy::bundle_source_match::DIFFERENT);
  CHECK(envy::bundle_source_compare(remote_bundle("a.b@v1", "https://x/b.tgz", "aa"),
                                    remote_bundle("a.b@v1", "https://x/b.tgz", "bb")) ==
        envy::bundle_source_match::DIFFERENT);
  CHECK(envy::bundle_source_compare(git_bundle("a.b@v1", "git://x/b.git", "main"),
                                    git_bundle("a.b@v1", "git://x/b.git", "v2")) ==
        envy::bundle_source_match::DIFFERENT);
  CHECK(envy::bundle_source_compare(local_bundle("a.b@v1", "/a/b"),
                                    local_bundle("a.b@v1", "/a/c")) ==
        envy::bundle_source_match::DIFFERENT);
}

TEST_CASE("bundle_source_compare: mismatched source kinds are DIFFERENT") {
  auto const remote{ remote_bundle("a.b@v1", "https://x/b.tgz") };

  CHECK(envy::bundle_source_compare(remote, git_bundle("a.b@v1", "git://x/b.git", "m")) ==
        envy::bundle_source_match::DIFFERENT);
  CHECK(envy::bundle_source_compare(remote, local_bundle("a.b@v1", "/a/b")) ==
        envy::bundle_source_match::DIFFERENT);
  // A closure versus a URL is decidable, unlike closure versus closure.
  CHECK(envy::bundle_source_compare(remote, custom_bundle("a.b@v1")) ==
        envy::bundle_source_match::DIFFERENT);
  CHECK(envy::bundle_source_compare(custom_bundle("a.b@v1"), remote) ==
        envy::bundle_source_match::DIFFERENT);
}

TEST_CASE("bundle_source_compare: a differing bundle identity is DIFFERENT") {
  // Two specs pulling one spec identity out of unrelated bundles.
  CHECK(envy::bundle_source_compare(remote_bundle("a.b@v1", "https://x/b.tgz"),
                                    remote_bundle("a.c@v1", "https://x/b.tgz")) ==
        envy::bundle_source_match::DIFFERENT);
  CHECK(envy::bundle_source_compare(custom_bundle("a.b@v1"), custom_bundle("a.c@v1")) ==
        envy::bundle_source_match::DIFFERENT);
}

TEST_CASE("bundle_source_compare: two fetch closures are INCOMPARABLE") {
  CHECK(envy::bundle_source_compare(custom_bundle("a.b@v1"), custom_bundle("a.b@v1")) ==
        envy::bundle_source_match::INCOMPARABLE);

  // Dependency lists are per-parse cfg pointers, so they cannot settle it either way.
  auto *dep{ envy::pkg_cfg::pool()->emplace("a.dep@v1",
                                            envy::pkg_cfg::remote_source{ .url = "u" },
                                            "{}",
                                            std::nullopt,
                                            nullptr,
                                            nullptr,
                                            std::vector<envy::pkg_cfg *>{},
                                            std::nullopt,
                                            fs::path("/fake")) };
  CHECK(envy::bundle_source_compare(custom_bundle("a.b@v1", { dep }),
                                    custom_bundle("a.b@v1")) ==
        envy::bundle_source_match::INCOMPARABLE);
}

TEST_CASE("bundle_source_compare is symmetric") {
  auto const remote{ remote_bundle("a.b@v1", "https://x/b.tgz") };
  auto const git{ git_bundle("a.b@v1", "git://x/b.git", "main") };
  auto const custom{ custom_bundle("a.b@v1") };

  for (auto const &lhs : { remote, git, custom }) {
    for (auto const &rhs : { remote, git, custom }) {
      CHECK(envy::bundle_source_compare(lhs, rhs) ==
            envy::bundle_source_compare(rhs, lhs));
    }
  }
}
