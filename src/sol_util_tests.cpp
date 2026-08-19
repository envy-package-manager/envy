#include "sol_util.h"

#include "doctest.h"

#include <stdexcept>

TEST_CASE("sol_util_make_lua_state creates state with standard libraries") {
  auto lua = envy::sol_util_make_lua_state();
  REQUIRE(lua);

  lua->script("x = 10 + 20");
  int x = (*lua)["x"];
  CHECK(x == 30);

  lua->script("y = math.sqrt(16)");
  double y = (*lua)["y"];
  CHECK(y == 4.0);

  lua->script("z = string.upper('hello')");
  std::string z = (*lua)["z"];
  CHECK(z == "HELLO");

  lua->script("t = {a = 1, b = 2}");
  sol::table t = (*lua)["t"];
  CHECK(t["a"].get<int>() == 1);
  CHECK(t["b"].get<int>() == 2);
}

TEST_CASE("sol_util_make_lua_state overrides error to include stack trace") {
  auto lua = envy::sol_util_make_lua_state();

  auto result = lua->safe_script(R"lua(
    function foo()
      error("test error")
    end
    foo()
  )lua",
                                 sol::script_pass_on_error);

  CHECK_FALSE(result.valid());
  sol::error err = result;
  std::string msg = err.what();
  CHECK(msg.find("test error") != std::string::npos);
  CHECK(msg.find("stack traceback:") != std::string::npos);
}

TEST_CASE("sol_util_make_lua_state overrides assert to include stack trace") {
  auto lua = envy::sol_util_make_lua_state();

  auto result = lua->safe_script(R"lua(
    function bar()
      assert(false, "test assertion")
    end
    bar()
  )lua",
                                 sol::script_pass_on_error);

  CHECK_FALSE(result.valid());
  sol::error err = result;
  std::string msg = err.what();
  CHECK(msg.find("test assertion") != std::string::npos);
  CHECK(msg.find("stack traceback:") != std::string::npos);
}

TEST_CASE("sol_util_get_optional returns value when present and correct type") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {flag = true, name = 'test', count = 42}");
  sol::table t = (*lua)["t"];

  SUBCASE("bool") {
    auto result = envy::sol_util_get_optional<bool>(t, "flag", "test");
    REQUIRE(result.has_value());
    CHECK(*result == true);
  }

  SUBCASE("string") {
    auto result = envy::sol_util_get_optional<std::string>(t, "name", "test");
    REQUIRE(result.has_value());
    CHECK(*result == "test");
  }

  SUBCASE("number") {
    auto result = envy::sol_util_get_optional<int>(t, "count", "test");
    REQUIRE(result.has_value());
    CHECK(*result == 42);
  }
}

TEST_CASE("sol_util_get_optional returns nullopt when absent") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {}");
  sol::table t = (*lua)["t"];

  auto result = envy::sol_util_get_optional<bool>(t, "missing", "test");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("sol_util_get_optional returns nullopt when nil") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {x = nil}");
  sol::table t = (*lua)["t"];

  auto result = envy::sol_util_get_optional<bool>(t, "x", "test");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("sol_util_get_optional throws when wrong type") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {flag = 'not a boolean', name = 123, count = true}");
  sol::table t = (*lua)["t"];

  SUBCASE("bool expected, string provided") {
    CHECK_THROWS_WITH_AS(envy::sol_util_get_optional<bool>(t, "flag", "test.function"),
                         "test.function: flag must be a boolean",
                         std::runtime_error);
  }

  SUBCASE("string expected, number provided") {
    CHECK_THROWS_WITH_AS(envy::sol_util_get_optional<std::string>(t, "name", "ctx.run"),
                         "ctx.run: name must be a string",
                         std::runtime_error);
  }

  SUBCASE("number expected, bool provided") {
    CHECK_THROWS_WITH_AS(envy::sol_util_get_optional<int>(t, "count", "parse"),
                         "parse: count must be a number",
                         std::runtime_error);
  }
}

TEST_CASE("sol_util_get_optional handles tables") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {nested = {a = 1}}");
  sol::table t = (*lua)["t"];

  SUBCASE("table present") {
    auto result = envy::sol_util_get_optional<sol::table>(t, "nested", "test");
    REQUIRE(result.has_value());
    CHECK(result->get<int>("a") == 1);
  }

  SUBCASE("wrong type") {
    lua->script("t.nested = 'not a table'");
    CHECK_THROWS_WITH_AS(envy::sol_util_get_optional<sol::table>(t, "nested", "test"),
                         "test: nested must be a table",
                         std::runtime_error);
  }
}

TEST_CASE("sol_util_get_optional handles functions") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {func = function() return 42 end}");
  sol::table t = (*lua)["t"];

  SUBCASE("function present") {
    auto result = envy::sol_util_get_optional<sol::protected_function>(t, "func", "test");
    REQUIRE(result.has_value());
    int r = (*result)();
    CHECK(r == 42);
  }

  SUBCASE("wrong type") {
    lua->script("t.func = 'not a function'");
    CHECK_THROWS_WITH_AS(
        envy::sol_util_get_optional<sol::protected_function>(t, "func", "test"),
        "test: func must be a function",
        std::runtime_error);
  }
}

TEST_CASE("sol_util_get_required returns value when present and correct type") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {flag = true, name = 'test', count = 42}");
  sol::table t = (*lua)["t"];

  SUBCASE("bool") {
    bool result = envy::sol_util_get_required<bool>(t, "flag", "test");
    CHECK(result == true);
  }

  SUBCASE("string") {
    std::string result = envy::sol_util_get_required<std::string>(t, "name", "test");
    CHECK(result == "test");
  }

  SUBCASE("number") {
    int result = envy::sol_util_get_required<int>(t, "count", "test");
    CHECK(result == 42);
  }
}

TEST_CASE("sol_util_get_required throws when absent") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {}");
  sol::table t = (*lua)["t"];

  CHECK_THROWS_WITH_AS(envy::sol_util_get_required<bool>(t, "missing", "test"),
                       "test: missing is required",
                       std::runtime_error);
}

TEST_CASE("sol_util_get_required throws when nil") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {x = nil}");
  sol::table t = (*lua)["t"];

  CHECK_THROWS_WITH_AS(envy::sol_util_get_required<bool>(t, "x", "test"),
                       "test: x is required",
                       std::runtime_error);
}

TEST_CASE("sol_util_get_required throws when wrong type") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {flag = 'not a boolean'}");
  sol::table t = (*lua)["t"];

  CHECK_THROWS_WITH_AS(envy::sol_util_get_required<bool>(t, "flag", "config"),
                       "config: flag must be a boolean",
                       std::runtime_error);
}

TEST_CASE("sol_util_get_or_default returns value when present and correct type") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {flag = true, name = 'test', count = 42}");
  sol::table t = (*lua)["t"];

  SUBCASE("bool") {
    bool result = envy::sol_util_get_or_default<bool>(t, "flag", false, "test");
    CHECK(result == true);
  }

  SUBCASE("string") {
    std::string result =
        envy::sol_util_get_or_default<std::string>(t, "name", "default", "test");
    CHECK(result == "test");
  }

  SUBCASE("number") {
    int result = envy::sol_util_get_or_default<int>(t, "count", 0, "test");
    CHECK(result == 42);
  }
}

TEST_CASE("sol_util_get_or_default returns default when absent") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {}");
  sol::table t = (*lua)["t"];

  SUBCASE("bool") {
    bool result = envy::sol_util_get_or_default<bool>(t, "missing", false, "test");
    CHECK(result == false);
  }

  SUBCASE("string") {
    std::string result =
        envy::sol_util_get_or_default<std::string>(t, "missing", "default_value", "test");
    CHECK(result == "default_value");
  }

  SUBCASE("number") {
    int result = envy::sol_util_get_or_default<int>(t, "missing", 99, "test");
    CHECK(result == 99);
  }
}

TEST_CASE("sol_util_get_or_default returns default when nil") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {x = nil}");
  sol::table t = (*lua)["t"];

  bool result = envy::sol_util_get_or_default<bool>(t, "x", true, "test");
  CHECK(result == true);
}

TEST_CASE("sol_util_get_or_default throws when wrong type") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {flag = 'not a boolean'}");
  sol::table t = (*lua)["t"];

  CHECK_THROWS_WITH_AS(envy::sol_util_get_or_default<bool>(t, "flag", false, "config"),
                       "config: flag must be a boolean",
                       std::runtime_error);
}

TEST_CASE("sol_util_dump_table formats string-keyed table") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {source = 'https://example.com/file.tar.gz', sha256 = 'abc123'}");
  sol::table t = (*lua)["t"];

  std::string result = envy::sol_util_dump_table(t);
  CHECK(result.find("source=") != std::string::npos);
  CHECK(result.find("https://example.com/file.tar.gz") != std::string::npos);
  CHECK(result.find("sha256=") != std::string::npos);
  CHECK(result.find("abc123") != std::string::npos);
  CHECK(result.front() == '{');
  CHECK(result.back() == '}');
}

TEST_CASE("sol_util_dump_table formats integer-keyed array") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {'url1', 'url2'}");
  sol::table t = (*lua)["t"];

  std::string result = envy::sol_util_dump_table(t);
  CHECK(result.find("[1]=") != std::string::npos);
  CHECK(result.find("[2]=") != std::string::npos);
  CHECK(result.find("url1") != std::string::npos);
  CHECK(result.find("url2") != std::string::npos);
}

TEST_CASE("sol_util_dump_table shows nested tables as {...}") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {{source = 'url1'}, {source = 'url2'}}");
  sol::table t = (*lua)["t"];

  std::string result = envy::sol_util_dump_table(t);
  CHECK(result.find("{...}") != std::string::npos);
}

TEST_CASE("sol_util_dump_table truncates long strings") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {url = string.rep('x', 100)}");
  sol::table t = (*lua)["t"];

  std::string result = envy::sol_util_dump_table(t);
  CHECK(result.find("...") != std::string::npos);
  CHECK(result.size() < 100);
}

TEST_CASE("sol_util_dump_table handles empty table") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {}");
  sol::table t = (*lua)["t"];

  CHECK(envy::sol_util_dump_table(t) == "{}");
}

TEST_CASE("sol_util_dump_table shows non-string non-table value types") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {flag = true, count = 42}");
  sol::table t = (*lua)["t"];

  std::string result = envy::sol_util_dump_table(t);
  CHECK(result.find("boolean") != std::string::npos);
  CHECK(result.find("number") != std::string::npos);
}

TEST_CASE("sol_util_get_string_list reads arrays and rejects bad entries") {
  auto lua = envy::sol_util_make_lua_state();
  lua->script("t = {names = {'a', 'b/c'}, empty = {}, wrong = 'str'}");
  sol::table t = (*lua)["t"];

  auto const names{ envy::sol_util_get_string_list(t, "names", "test") };
  REQUIRE(names.size() == 2);
  CHECK(names[0] == "a");
  CHECK(names[1] == "b/c");

  CHECK(envy::sol_util_get_string_list(t, "missing", "test").empty());
  CHECK(envy::sol_util_get_string_list(t, "empty", "test").empty());

  CHECK_THROWS_WITH_AS(envy::sol_util_get_string_list(t, "wrong", "test"),
                       "test: wrong must be a table",
                       std::runtime_error);

  lua->script("t.names = {'a', 42}");
  CHECK_THROWS_WITH_AS(envy::sol_util_get_string_list(t, "names", "ctx"),
                       "ctx: names[2] must be a non-empty string",
                       std::runtime_error);

  lua->script("t.names = {''}");
  CHECK_THROWS_WITH_AS(envy::sol_util_get_string_list(t, "names", "ctx"),
                       "ctx: names[1] must be a non-empty string",
                       std::runtime_error);
}

TEST_CASE("type_name_for_error returns correct names") {
  CHECK(envy::detail::type_name_for_error<bool>() == "boolean");
  CHECK(envy::detail::type_name_for_error<std::string>() == "string");
  CHECK(envy::detail::type_name_for_error<sol::table>() == "table");
  CHECK(envy::detail::type_name_for_error<sol::protected_function>() == "function");
  CHECK(envy::detail::type_name_for_error<sol::function>() == "function");
  CHECK(envy::detail::type_name_for_error<int>() == "number");
  CHECK(envy::detail::type_name_for_error<double>() == "number");
  CHECK(envy::detail::type_name_for_error<long>() == "number");
  CHECK(envy::detail::type_name_for_error<float>() == "number");
}
