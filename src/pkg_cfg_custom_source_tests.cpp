#include "pkg_cfg.h"

#include "doctest.h"
#include "sol/sol.hpp"
#include "sol_util.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// Helper: Set up a Lua environment simulating a spec with dependencies
// Creates a dependencies global array with the given spec table
void setup_spec_environment(sol::state &lua,
                            std::string const &identity,
                            std::vector<std::string> const &dep_identities = {}) {
  // Build Lua code that creates a dependencies global
  std::string lua_code{ "DEPENDENCIES = {\n  {\n    spec = \"" + identity +
                        "\",\n    source = {\n" };

  if (!dep_identities.empty()) {
    lua_code += "      dependencies = {\n";
    for (auto const &dep_id : dep_identities) {
      lua_code += "        { spec = \"" + dep_id + "\", source = \"file:///tmp/" + dep_id +
                  ".lua\" },\n";
    }
    lua_code += "      },\n";
  }

  lua_code += "      fetch = function(ctx)\n";
  lua_code += "        return \"" + identity + "\"\n";  // Function returns identity
  lua_code += "      end\n";
  lua_code += "    }\n";
  lua_code += "  }\n";
  lua_code += "}\n";

  auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error("Lua error: " + std::string{ err.what() });
  }
}

// Helper: Create and parse a pkg_cfg with custom source fetch
// The fetch function returns the spec identity for verification
envy::pkg_cfg *create_spec_with_custom_fetch(
    sol::state &lua,
    std::string const &identity,
    std::vector<std::string> const &dep_identities = {}) {
  // Set up dependencies global for lookup
  setup_spec_environment(lua, identity, dep_identities);

  // Build Lua code that creates a spec with custom source fetch
  std::string lua_code{ "return {\n  spec = \"" + identity + "\",\n  source = {\n" };

  if (!dep_identities.empty()) {
    lua_code += "    dependencies = {\n";
    for (auto const &dep_id : dep_identities) {
      lua_code += "      { spec = \"" + dep_id + "\", source = \"file:///tmp/" + dep_id +
                  ".lua\" },\n";
    }
    lua_code += "    },\n";
  }

  lua_code += "    fetch = function(ctx)\n";
  lua_code += "      return \"" + identity + "\"\n";  // Function returns identity
  lua_code += "    end\n";
  lua_code += "  }\n}";

  // Execute Lua code to create table
  auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error("Lua error: " + std::string{ err.what() });
  }

  sol::object spec_val = result;
  return envy::pkg_cfg::parse(spec_val, fs::current_path());
}

// Helper: Call a pkg_cfg's custom fetch function and return result
std::string call_custom_fetch(sol::state &lua, envy::pkg_cfg const *cfg) {
  if (!cfg->has_fetch_function()) {
    throw std::runtime_error("pkg_cfg has no custom fetch function");
  }

  // Look up function using dynamic lookup
  sol::table deps = lua["DEPENDENCIES"];
  if (!deps.valid()) { throw std::runtime_error("DEPENDENCIES global not found"); }

  // Find the matching dependency
  for (size_t i{ 1 };; ++i) {
    sol::object dep_entry = deps[i];
    if (!dep_entry.valid()) { break; }

    if (dep_entry.is<sol::table>()) {
      sol::table dep_table{ dep_entry.as<sol::table>() };
      sol::object spec_obj = dep_table["spec"];
      if (spec_obj.valid() && spec_obj.is<std::string>() &&
          spec_obj.as<std::string>() == cfg->identity) {
        sol::table source_table = dep_table["source"];
        sol::function fetch_func = source_table["fetch"];

        // Create dummy ctx table
        sol::table ctx{ lua.create_table() };

        // Call function(ctx)
        auto result{ fetch_func(ctx) };
        if (!result.valid()) { throw std::runtime_error("Function call failed"); }

        return result.get<std::string>();
      }
    }
  }

  throw std::runtime_error("Failed to lookup source.fetch for " + cfg->identity);
}

}  // namespace

TEST_CASE("pkg_cfg - function returns correct identity") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  envy::pkg_cfg *cfg{ create_spec_with_custom_fetch(lua, "local.foo@v1") };

  CHECK(cfg->identity == "local.foo@v1");
  CHECK(cfg->has_fetch_function());

  // Call the function and verify it returns the identity
  std::string result{ call_custom_fetch(lua, cfg) };
  CHECK(result == "local.foo@v1");
}

TEST_CASE("pkg_cfg - multiple specs have correct functions") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  // Set up dependencies global with all three specs
  std::string lua_code{ R"(
    DEPENDENCIES = {
      {
        spec = "local.foo@v1",
        source = {
          fetch = function(ctx) return "local.foo@v1" end
        }
      },
      {
        spec = "local.bar@v1",
        source = {
          fetch = function(ctx) return "local.bar@v1" end
        }
      },
      {
        spec = "local.baz@v1",
        source = {
          fetch = function(ctx) return "local.baz@v1" end
        }
      }
    }
  )" };

  lua.safe_script(lua_code);

  // Parse each spec (they'll all use the same dependencies global)
  sol::table deps_table = lua["DEPENDENCIES"];
  sol::object val_foo = deps_table[1];
  sol::object val_bar = deps_table[2];
  sol::object val_baz = deps_table[3];

  envy::pkg_cfg *cfg_foo{ envy::pkg_cfg::parse(val_foo, fs::current_path()) };
  envy::pkg_cfg *cfg_bar{ envy::pkg_cfg::parse(val_bar, fs::current_path()) };
  envy::pkg_cfg *cfg_baz{ envy::pkg_cfg::parse(val_baz, fs::current_path()) };

  // Verify each has a function
  CHECK(cfg_foo->has_fetch_function());
  CHECK(cfg_bar->has_fetch_function());
  CHECK(cfg_baz->has_fetch_function());

  // Call each function and verify correct association
  CHECK(call_custom_fetch(lua, cfg_foo) == "local.foo@v1");
  CHECK(call_custom_fetch(lua, cfg_bar) == "local.bar@v1");
  CHECK(call_custom_fetch(lua, cfg_baz) == "local.baz@v1");
}

TEST_CASE("pkg_cfg - with source dependencies") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  envy::pkg_cfg *cfg{ create_spec_with_custom_fetch(
      lua,
      "local.parent@v1",
      { "local.tool1@v1", "local.tool2@v1" }) };

  CHECK(cfg->identity == "local.parent@v1");
  CHECK(cfg->source_dependencies.size() == 2);
  CHECK(cfg->source_dependencies[0]->identity == "local.tool1@v1");
  CHECK(cfg->source_dependencies[1]->identity == "local.tool2@v1");

  // Function still works
  CHECK(call_custom_fetch(lua, cfg) == "local.parent@v1");
}

TEST_CASE("pkg_cfg - function persists across multiple calls") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  envy::pkg_cfg *cfg{ create_spec_with_custom_fetch(lua, "local.persistent@v1") };

  // Call the function many times
  for (int i{ 0 }; i < 50; ++i) {
    CHECK(call_custom_fetch(lua, cfg) == "local.persistent@v1");
  }
}

TEST_CASE("pkg_cfg - error on dependencies without fetch") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  std::string lua_code{ R"(
    return {
      spec = "local.broken@v1",
      source = {
        dependencies = {
          { spec = "local.tool@v1", source = "file:///tmp/tool.lua" }
        }
      }
    }
  )" };

  CHECK_THROWS_WITH(
      [&]() {
        auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
        sol::object val = result;
        envy::pkg_cfg::parse(val, fs::current_path());
      }(),
      "source.dependencies requires source.fetch function");
}

TEST_CASE("pkg_cfg - error on fetch not a function") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  std::string lua_code{ R"(
    return {
      spec = "local.broken@v1",
      source = {
        dependencies = {
          { spec = "local.tool@v1", source = "file:///tmp/tool.lua" }
        },
        fetch = "not-a-function"
      }
    }
  )" };

  CHECK_THROWS_WITH(
      [&]() {
        auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
        sol::object val = result;
        envy::pkg_cfg::parse(val, fs::current_path());
      }(),
      "source.fetch must be a function");
}

TEST_CASE("pkg_cfg - error on dependencies not array") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  std::string lua_code{ R"(
    return {
      spec = "local.broken@v1",
      source = {
        dependencies = "not-an-array",
        fetch = function(ctx) end
      }
    }
  )" };

  CHECK_THROWS_WITH(
      [&]() {
        auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
        sol::object val = result;
        envy::pkg_cfg::parse(val, fs::current_path());
      }(),
      "source.dependencies must be array (table)");
}

TEST_CASE("pkg_cfg - error on empty source table") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  std::string lua_code{ R"(
    return {
      spec = "local.broken@v1",
      source = {}
    }
  )" };

  CHECK_THROWS_WITH(
      [&]() {
        auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
        sol::object val = result;
        envy::pkg_cfg::parse(val, fs::current_path());
      }(),
      "source table must have either URL string or dependencies+fetch function");
}

TEST_CASE("pkg_cfg - error on parse without lua_State") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  std::string lua_code{ R"(
    return {
      spec = "local.test@v1",
      source = {
        fetch = function(ctx) end
      }
    }
  )" };

  auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
  sol::object val = result;

  // Verify parsing with lua_State works for custom source.fetch
  CHECK_NOTHROW(envy::pkg_cfg::parse(val, fs::current_path()));
}

TEST_CASE("pkg_cfg - no function without source table") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  std::string lua_code{ R"(
    return {
      spec = "local.normal@v1",
      source = "file:///tmp/normal.lua"
    }
  )" };

  auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
  sol::object val = result;
  envy::pkg_cfg *cfg{ envy::pkg_cfg::parse(val, fs::current_path()) };

  CHECK(cfg->identity == "local.normal@v1");
  CHECK_FALSE(cfg->has_fetch_function());
  CHECK(cfg->source_dependencies.empty());
}

// -- fetch-dependency product references ------------------------------------
//
// A `source.dependencies` entry is parsed through parse_fetch_dependency, which
// is parse(..., allow_weak=true) plus one extra rule: a `product` reference must
// be strong. The weak pass runs only at a resolution barrier, after every
// spec_fetch, so a weak product reference could never be edged before the fetch
// function that consumes it. Rejecting at parse time also stops a bare entry
// (which parses to an empty identity) from reaching the graph.

namespace {

sol::object eval_entry(sol::state &lua, std::string const &lua_code) {
  auto result{ lua.safe_script("return " + lua_code, sol::script_pass_on_error) };
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error("Lua error: " + std::string{ err.what() });
  }
  return result;
}

}  // namespace

TEST_CASE("parse_fetch_dependency: bare product entry is rejected") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  // No 'spec', no 'source' — parse(..., allow_weak=true) yields an empty identity,
  // which would be pushed into the weak pass as an unmatchable empty query.
  sol::object entry{ eval_entry(lua, R"({ product = "jf" })") };
  CHECK_THROWS_WITH(envy::pkg_cfg::parse_fetch_dependency(entry, fs::current_path()),
                    doctest::Contains("must be a strong reference"));
}

TEST_CASE("parse_fetch_dependency: product with spec but no source is rejected") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  sol::object entry{ eval_entry(lua, R"({ spec = "local.tool@v1", product = "jf" })") };
  CHECK_THROWS_WITH(envy::pkg_cfg::parse_fetch_dependency(entry, fs::current_path()),
                    doctest::Contains("must be a strong reference"));
}

TEST_CASE("parse_fetch_dependency: strong product entry is accepted and pins provider") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  sol::object entry{ eval_entry(
      lua,
      R"({ spec = "local.tool@v1", source = "file:///tmp/tool.lua", product = "jf" })") };
  envy::pkg_cfg *cfg{ envy::pkg_cfg::parse_fetch_dependency(entry, fs::current_path()) };

  CHECK(cfg->identity == "local.tool@v1");
  CHECK(cfg->product.has_value());
  CHECK(*cfg->product == "jf");
  CHECK_FALSE(cfg->is_weak_reference());
}

TEST_CASE("parse_fetch_dependency: weak entry without a product is still allowed") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  // Identity-only weak fetch dependencies predate product references and stay
  // legal; only the product form is rejected.
  sol::object entry{ eval_entry(lua, R"({ spec = "local.tool@v1" })") };
  envy::pkg_cfg *cfg{ envy::pkg_cfg::parse_fetch_dependency(entry, fs::current_path()) };

  CHECK(cfg->identity == "local.tool@v1");
  CHECK(cfg->is_weak_reference());
  CHECK_FALSE(cfg->product.has_value());
}

TEST_CASE("parse_fetch_dependency: source.dependencies rejects a bare product entry") {
  auto lua_state{ envy::sol_util_make_lua_state() };
  sol::state &lua{ *lua_state };

  // Reaching the rule through a whole source table, not just the entry helper:
  // both fetch-dependency parse sites must funnel through parse_fetch_dependency.
  std::string const lua_code{ R"(
    return {
      spec = "local.parent@v1",
      source = {
        dependencies = { { product = "jf" } },
        fetch = function(tmp) end,
      },
    }
  )" };
  auto result{ lua.safe_script(lua_code, sol::script_pass_on_error) };
  REQUIRE(result.valid());
  sol::object val = result;

  CHECK_THROWS_WITH(envy::pkg_cfg::parse(val, fs::current_path()),
                    doctest::Contains("must be a strong reference"));
}
