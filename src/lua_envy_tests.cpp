#include "lua_envy.h"

#include "sol_util.h"

#include "doctest.h"

namespace envy {

TEST_CASE("envy.EXE_EXT injected into Lua state") {
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua);

  sol::table envy_table = (*lua)["envy"];
  REQUIRE(envy_table.valid());
  sol::object ext_obj = envy_table["EXE_EXT"];
  REQUIRE(ext_obj.is<std::string>());
  std::string const ext{ ext_obj.as<std::string>() };

#if defined(_WIN32)
  CHECK(ext == ".exe");
#else
  CHECK(ext.empty());
#endif
}

TEST_CASE("envy.extend") {
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua);

  SUBCASE("extends target with single list") {
    auto result{ lua->safe_script(R"lua(
      local t = {1, 2}
      envy.extend(t, {3, 4})
      return t[1], t[2], t[3], t[4], #t
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<int>(0) == 1);
    CHECK(result.get<int>(1) == 2);
    CHECK(result.get<int>(2) == 3);
    CHECK(result.get<int>(3) == 4);
    CHECK(result.get<int>(4) == 4);
  }

  SUBCASE("extends target with multiple lists") {
    auto result{ lua->safe_script(R"lua(
      local t = {"a"}
      envy.extend(t, {"b", "c"}, {"d"})
      return t[1], t[2], t[3], t[4], #t
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<std::string>(0) == "a");
    CHECK(result.get<std::string>(1) == "b");
    CHECK(result.get<std::string>(2) == "c");
    CHECK(result.get<std::string>(3) == "d");
    CHECK(result.get<int>(4) == 4);
  }

  SUBCASE("handles empty source lists") {
    auto result{ lua->safe_script(R"lua(
      local t = {1}
      envy.extend(t, {}, {2}, {})
      return t[1], t[2], #t
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<int>(0) == 1);
    CHECK(result.get<int>(1) == 2);
    CHECK(result.get<int>(2) == 2);
  }

  SUBCASE("handles empty target") {
    auto result{ lua->safe_script(R"lua(
      local t = {}
      envy.extend(t, {1, 2, 3})
      return t[1], t[2], t[3], #t
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<int>(0) == 1);
    CHECK(result.get<int>(1) == 2);
    CHECK(result.get<int>(2) == 3);
    CHECK(result.get<int>(3) == 3);
  }

  SUBCASE("returns target table") {
    auto result{ lua->safe_script(R"lua(
      local t = {1}
      local r = envy.extend(t, {2})
      return t == r
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<bool>(0) == true);
  }

  SUBCASE("works with no additional arguments") {
    auto result{ lua->safe_script(R"lua(
      local t = {1, 2, 3}
      envy.extend(t)
      return #t
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<int>(0) == 3);
  }

  SUBCASE("errors on non-table first argument") {
    auto result{ lua->safe_script(R"lua(
      local ok, err = pcall(function() envy.extend("not a table", {1}) end)
      return ok, err:match("first argument must be a table") ~= nil
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<bool>(0) == false);
    CHECK(result.get<bool>(1) == true);
  }

  SUBCASE("errors on non-table additional argument") {
    auto result{ lua->safe_script(R"lua(
      local ok, err = pcall(function() envy.extend({}, {1}, "bad", {2}) end)
      return ok, err:match("argument 3 must be a table") ~= nil
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<bool>(0) == false);
    CHECK(result.get<bool>(1) == true);
  }

  SUBCASE("preserves non-array keys in target") {
    auto result{ lua->safe_script(R"lua(
      local t = {1, 2, name = "test"}
      envy.extend(t, {3})
      return t[1], t[2], t[3], t.name, #t
    )lua") };
    REQUIRE(result.valid());
    CHECK(result.get<int>(0) == 1);
    CHECK(result.get<int>(1) == 2);
    CHECK(result.get<int>(2) == 3);
    CHECK(result.get<std::string>(3) == "test");
    CHECK(result.get<int>(4) == 3);
  }
}

// The chunk name envy.loadenv reads with debug.getinfo to anchor a relative module
// path; the file need not exist, only its directory.
constexpr char kCaller[]{ "@test_data/lua/caller.lua" };

TEST_CASE("envy.loadenv hands back what the module returns") {
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua);

  SUBCASE("a returned table is the result") {
    auto const result{ lua->safe_script(R"lua(
      local m = envy.loadenv("loadenv.mod_table")
      return m.NAME, m.greet()
    )lua",
                                        sol::script_pass_on_error,
                                        kCaller) };
    REQUIRE(result.valid());
    CHECK(result.get<std::string>(0) == "table-mod");
    CHECK(result.get<std::string>(1) == "hi");
  }

  SUBCASE("a module that returns nothing still hands back its globals") {
    auto const result{ lua->safe_script(R"lua(
      local m = envy.loadenv("loadenv.mod_globals")
      return m.NAME, m.greet()
    )lua",
                                        sol::script_pass_on_error,
                                        kCaller) };
    REQUIRE(result.valid());
    CHECK(result.get<std::string>(0) == "globals-mod");
    CHECK(result.get<std::string>(1) == "hello");
  }

  SUBCASE("a non-table return value is an error naming the module") {
    auto const result{ lua->safe_script(R"lua(
      local ok, err = pcall(function() return envy.loadenv("loadenv.mod_number") end)
      return ok, err
    )lua",
                                        sol::script_pass_on_error,
                                        kCaller) };
    REQUIRE(result.valid());
    CHECK(result.get<bool>(0) == false);
    CHECK(result.get<std::string>(1).find("loadenv.mod_number") != std::string::npos);
  }
}

TEST_CASE("envy.loadenv_bundle points a spec at envy.loadenv_spec") {
  // Only a manifest can reach a bundle by alias; installed on every other state as
  // a refusal, so a spec gets a sentence rather than a nil field.
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua);

  auto const result{ lua->safe_script(R"lua(
    local ok, err = pcall(function() envy.loadenv_bundle("tools", "lib.entries") end)
    return ok, err
  )lua",
                                      sol::script_pass_on_error) };
  REQUIRE(result.valid());
  CHECK(result.get<bool>(0) == false);
  CHECK(result.get<std::string>(1).find("envy.loadenv_spec") != std::string::npos);
}

}  // namespace envy
