#include "lua_envy_options.h"

#include "lua_envy.h"

#include "doctest.h"

namespace envy {
namespace {

struct lua_fixture {
  sol::state lua;
  sol::table schema;

  lua_fixture() {
    lua.open_libraries(sol::lib::base, sol::lib::string);
    sol::table envy_table{ lua.create_table() };
    lua_envy_options_install(envy_table);
    lua["envy"] = envy_table;
    schema = lua.create_table();
  }

  sol::object make_opts(std::string const &lua_expr) {
    auto result{ lua.safe_script("return " + lua_expr, sol::script_pass_on_error) };
    REQUIRE(result.valid());
    sol::object obj{ result };
    lua.registry()[ENVY_OPTIONS_RIDX] = obj;
    return obj;
  }

  sol::object nil_opts() {
    sol::object obj{ sol::lua_nil };
    lua.registry()[ENVY_OPTIONS_RIDX] = sol::lua_nil;
    return obj;
  }
};

// -- Table form: required --

TEST_CASE("OPTIONS: required option present succeeds") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["required"] = true;
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: required option missing throws") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["required"] = true;
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{}") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("'version' is required"));
}

TEST_CASE("OPTIONS: required false and missing succeeds") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["required"] = false;
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{}") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: optional option missing succeeds") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{}") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

// -- Table form: unknown options --

TEST_CASE("OPTIONS: unknown option rejected") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1\", foo = \"bar\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("unknown option 'foo'"));
}

TEST_CASE("OPTIONS: no opts with empty schema succeeds") {
  lua_fixture f;
  sol::object opts{ f.make_opts("{}") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: extra option with no schema keys") {
  lua_fixture f;
  sol::object opts{ f.make_opts("{ x = 1 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("unknown option 'x'"));
}

// -- Table form: semver --

TEST_CASE("OPTIONS: semver valid string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.2.3\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver valid with prerelease") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.2.3-rc.1\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver invalid string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"not.a.version\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not valid semver"));
}

TEST_CASE("OPTIONS: semver rejects number") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = 123 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'semver', got number"));
}

TEST_CASE("OPTIONS: semver not required and missing") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{}") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

// -- Table form: semver range --

TEST_CASE("OPTIONS: semver range pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 <2.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.5.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range boundary inclusive") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 <2.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.0.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range boundary exclusive") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 <2.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"2.0.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range below") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 <2.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"0.9.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range invalid range string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">>>bad";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.0.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("invalid range"));
}

TEST_CASE("OPTIONS: semver range multiple constraints") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 <2.0.0 >=1.2.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.1.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range with equal") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "=1.5.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.5.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range with equal mismatch") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "=1.5.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.5.1\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range trailing garbage rejected") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 garbage";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"2.0.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("has invalid range '>=1.0.0 garbage'"));
}

TEST_CASE("OPTIONS: semver range trailing whitespace pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 ";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"2.0.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range or second set pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 <2.0.0 || >=3.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"3.0.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range or gap") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0 <2.0.0 || >=3.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"2.5.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range prerelease pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0-alpha <2.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.0.0-alpha1\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range caret pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "^1.2.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.9.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range caret next major") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "^1.2.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"2.0.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range tilde next minor") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "~1.2.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.3.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range x-range pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "1.x";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.9.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: semver range not equal mismatch") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "!=1.5.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.5.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: semver range hyphen rejected") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  constraint["range"] = "1.0.0 - 2.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.5.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("invalid range"));
}

// -- Table form: numeric range --

TEST_CASE("OPTIONS: numeric range integer pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=1 <10";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 5 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: numeric range integer fail") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=1 <10";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 15 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: numeric range float pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=0.0 <=1.0";
  f.schema["ratio"] = constraint;

  sol::object opts{ f.make_opts("{ ratio = 0.5 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: numeric range float fail") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=0.0 <=1.0";
  f.schema["ratio"] = constraint;

  sol::object opts{ f.make_opts("{ ratio = 1.5 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: numeric range string-typed number pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=1 <10";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = \"5\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: numeric range non-numeric string rejects") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=1 <10";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = \"abc\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not numeric"));
}

TEST_CASE("OPTIONS: numeric range boundary inclusive") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=5";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 5 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: numeric range boundary exclusive") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">5";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 5 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: numeric range invalid token") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=abc";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 5 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("invalid numeric range token"));
}

// -- Table form: custom validate --

TEST_CASE("OPTIONS: validate function nil return") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) end");
  sol::table constraint{ f.lua.create_table() };
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"install\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: validate function true return") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) return true end");
  sol::table constraint{ f.lua.create_table() };
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"install\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: validate function string return") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) return \"bad mode\" end");
  sol::table constraint{ f.lua.create_table() };
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"install\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("validation failed: bad mode"));
}

TEST_CASE("OPTIONS: validate function false return") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) return false end");
  sol::table constraint{ f.lua.create_table() };
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"install\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("validation failed"));
}

TEST_CASE("OPTIONS: validate function lua error") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) error(\"boom\") end");
  sol::table constraint{ f.lua.create_table() };
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"install\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("boom"));
}

TEST_CASE("OPTIONS: validate function receives correct value") {
  lua_fixture f;
  f.lua.safe_script(
      "function my_validator(v) assert(v == \"expected\", \"wrong: \" .. tostring(v)) "
      "end");
  sol::table constraint{ f.lua.create_table() };
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"expected\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

// -- Table form: combined constraints --

TEST_CASE("OPTIONS: required + semver + range all pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["required"] = true;
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"2.0.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: required + semver fails on semver") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["required"] = true;
  constraint["type"] = "semver";
  constraint["range"] = ">=1.0.0";
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"bad\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not valid semver"));
}

TEST_CASE("OPTIONS: required + range + validate, validate runs after range") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) return \"custom error\" end");
  sol::table constraint{ f.lua.create_table() };
  constraint["range"] = ">=1 <100";
  constraint["validate"] = f.lua["my_validator"];
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 5 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("custom error"));
}

TEST_CASE("OPTIONS: multiple options all valid") {
  lua_fixture f;
  sol::table version_c{ f.lua.create_table() };
  version_c["required"] = true;
  sol::table mode_c{ f.lua.create_table() };
  f.schema["version"] = version_c;
  f.schema["mode"] = mode_c;

  sol::object opts{ f.make_opts("{ version = \"1\", mode = \"x\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: multiple options one invalid") {
  lua_fixture f;
  sol::table version_c{ f.lua.create_table() };
  version_c["required"] = true;
  sol::table mode_c{ f.lua.create_table() };
  f.schema["version"] = version_c;
  f.schema["mode"] = mode_c;

  sol::object opts{ f.make_opts("{ mode = \"x\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("'version' is required"));
}

// -- Table form: schema errors --

TEST_CASE("OPTIONS: schema entry not a table") {
  lua_fixture f;
  f.schema["version"] = "bad";

  sol::object opts{ f.make_opts("{ version = \"1\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be a table"));
}

TEST_CASE("OPTIONS: schema type not a string throws") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = true;
  f.schema["name"] = constraint;

  sol::object opts{ f.make_opts("{ name = \"hello\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("schema 'type' must be a string"));
}

TEST_CASE("OPTIONS: schema choices not a table throws") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["choices"] = "bad";
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"x\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("schema 'choices' must be a table"));
}

// -- Function form: return values (tested via run_options in phase_spec_fetch) --
// These test envy.options() from Lua

TEST_CASE("OPTIONS: envy.options() succeeds with valid opts") {
  lua_fixture f;
  f.make_opts("{ version = \"1.0\" }");

  f.lua.safe_script(R"lua(
    IDENTITY = "test@v1"
    envy.options({ version = { required = true } })
  )lua");
}

TEST_CASE("OPTIONS: envy.options() fails with missing required") {
  lua_fixture f;
  f.make_opts("{}");

  CHECK_THROWS_WITH(f.lua.safe_script(R"lua(
    IDENTITY = "test@v1"
    envy.options({ version = { required = true } })
  )lua"),
                    doctest::Contains("'version' is required"));
}

TEST_CASE("OPTIONS: envy.options() rejects unknown opts") {
  lua_fixture f;
  f.make_opts("{ version = \"1\", typo = \"x\" }");

  CHECK_THROWS_WITH(f.lua.safe_script(R"lua(
    IDENTITY = "test@v1"
    envy.options({ version = {} })
  )lua"),
                    doctest::Contains("unknown option 'typo'"));
}

TEST_CASE("OPTIONS: function does validation after envy.options") {
  lua_fixture f;
  f.make_opts("{ version = \"1.0\" }");

  CHECK_THROWS_WITH(f.lua.safe_script(R"lua(
    IDENTITY = "test@v1"
    envy.options({ version = { required = true } })
    error("post-schema error")
  )lua"),
                    doctest::Contains("post-schema error"));
}

TEST_CASE("OPTIONS: nil opts with no required fields succeeds") {
  lua_fixture f;
  f.nil_opts();

  f.lua.safe_script(R"lua(
    IDENTITY = "test@v1"
    envy.options({ version = {} })
  )lua");
}

TEST_CASE("OPTIONS: nil opts with required field throws") {
  lua_fixture f;
  sol::object opts{ f.nil_opts() };
  sol::table constraint{ f.lua.create_table() };
  constraint["required"] = true;
  f.schema["version"] = constraint;

  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("'version' is required"));
}

// -- Table form: type constraint --

TEST_CASE("OPTIONS: type string accepts string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  f.schema["name"] = constraint;

  sol::object opts{ f.make_opts("{ name = \"hello\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type string rejects number") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  f.schema["name"] = constraint;

  sol::object opts{ f.make_opts("{ name = 42 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'string', got number"));
}

TEST_CASE("OPTIONS: type string rejects boolean") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  f.schema["name"] = constraint;

  sol::object opts{ f.make_opts("{ name = true }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'string', got boolean"));
}

TEST_CASE("OPTIONS: type int accepts integer") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "int";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 42 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type int rejects float") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "int";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 3.14 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'int', got float"));
}

TEST_CASE("OPTIONS: type int rejects string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "int";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = \"hello\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'int', got string"));
}

TEST_CASE("OPTIONS: type float accepts float") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "float";
  f.schema["ratio"] = constraint;

  sol::object opts{ f.make_opts("{ ratio = 3.14 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type float accepts integer") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "float";
  f.schema["ratio"] = constraint;

  sol::object opts{ f.make_opts("{ ratio = 42 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type float rejects string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "float";
  f.schema["ratio"] = constraint;

  sol::object opts{ f.make_opts("{ ratio = \"hello\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'float', got string"));
}

TEST_CASE("OPTIONS: type boolean accepts true") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "boolean";
  f.schema["flag"] = constraint;

  sol::object opts{ f.make_opts("{ flag = true }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type boolean rejects string true") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "boolean";
  f.schema["flag"] = constraint;

  sol::object opts{ f.make_opts("{ flag = \"true\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'boolean', got string"));
}

TEST_CASE("OPTIONS: type table accepts map") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "table";
  f.schema["data"] = constraint;

  sol::object opts{ f.make_opts("{ data = { a = 1 } }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type table accepts array") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "table";
  f.schema["data"] = constraint;

  sol::object opts{ f.make_opts("{ data = { 1, 2 } }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type table rejects string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "table";
  f.schema["data"] = constraint;

  sol::object opts{ f.make_opts("{ data = \"hello\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'table', got string"));
}

TEST_CASE("OPTIONS: type list accepts sequential array") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "list";
  f.schema["items"] = constraint;

  sol::object opts{ f.make_opts("{ items = { \"a\", \"b\" } }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type list accepts empty table") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "list";
  f.schema["items"] = constraint;

  sol::object opts{ f.make_opts("{ items = {} }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type list rejects non-sequential table") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "list";
  f.schema["items"] = constraint;

  sol::object opts{ f.make_opts("{ items = { a = 1 } }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("non-sequential table"));
}

TEST_CASE("OPTIONS: type list rejects string") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "list";
  f.schema["items"] = constraint;

  sol::object opts{ f.make_opts("{ items = \"hello\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'list'"));
}

TEST_CASE("OPTIONS: invalid type string throws") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "widget";
  f.schema["thing"] = constraint;

  sol::object opts{ f.make_opts("{ thing = \"x\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("unknown type 'widget'"));
}

TEST_CASE("OPTIONS: type checked before choices") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  sol::table choices{ f.lua.create_table() };
  choices.add("a", "b");
  constraint["choices"] = choices;
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = 42 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'string'"));
}

TEST_CASE("OPTIONS: type checked before validate") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) return \"custom\" end");
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = 42 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'string'"));
}

// -- Table form: choices constraint --

TEST_CASE("OPTIONS: string choices accepts valid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  sol::table choices{ f.lua.create_table() };
  choices.add("install", "extract");
  constraint["choices"] = choices;
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"install\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: string choices rejects invalid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  sol::table choices{ f.lua.create_table() };
  choices.add("install", "extract");
  constraint["choices"] = choices;
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"debug\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not in {install, extract}"));
}

TEST_CASE("OPTIONS: number choices accepts valid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  sol::table choices{ f.lua.create_table() };
  choices.add(1, 2, 3);
  constraint["choices"] = choices;
  f.schema["level"] = constraint;

  sol::object opts{ f.make_opts("{ level = 2 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: number choices rejects invalid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  sol::table choices{ f.lua.create_table() };
  choices.add(1, 2, 3);
  constraint["choices"] = choices;
  f.schema["level"] = constraint;

  sol::object opts{ f.make_opts("{ level = 5 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not in {"));
}

TEST_CASE("OPTIONS: type + choices together") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  sol::table choices{ f.lua.create_table() };
  choices.add("a", "b");
  constraint["choices"] = choices;
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"a\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type mismatch caught before choices") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  sol::table choices{ f.lua.create_table() };
  choices.add("a", "b");
  constraint["choices"] = choices;
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = 42 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("must be type 'string'"));
}

TEST_CASE("OPTIONS: list + choices element-wise validation") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "list";
  sol::table choices{ f.lua.create_table() };
  choices.add("a", "b", "c");
  constraint["choices"] = choices;
  f.schema["tools"] = constraint;

  sol::object opts{ f.make_opts("{ tools = { \"a\", \"b\" } }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: list + choices element rejection with index") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "list";
  sol::table choices{ f.lua.create_table() };
  choices.add("a", "b", "c");
  constraint["choices"] = choices;
  f.schema["tools"] = constraint;

  sol::object opts{ f.make_opts("{ tools = { \"a\", \"b\", \"bad\" } }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("element 'bad' at index 3 not in {a, b, c}"));
}

TEST_CASE("OPTIONS: choices checked before validate") {
  lua_fixture f;
  f.lua.safe_script("function my_validator(v) return \"custom\" end");
  sol::table constraint{ f.lua.create_table() };
  sol::table choices{ f.lua.create_table() };
  choices.add("a", "b");
  constraint["choices"] = choices;
  constraint["validate"] = f.lua["my_validator"];
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"c\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not in {a, b}"));
}

// -- Table form: combined type + choices + required --

TEST_CASE("OPTIONS: required + type + choices all pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["required"] = true;
  constraint["type"] = "string";
  sol::table choices{ f.lua.create_table() };
  choices.add("x", "y");
  constraint["choices"] = choices;
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{ mode = \"x\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: optional absent with type + choices passes") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "string";
  sol::table choices{ f.lua.create_table() };
  choices.add("x", "y");
  constraint["choices"] = choices;
  f.schema["mode"] = constraint;

  sol::object opts{ f.make_opts("{}") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type int + numeric range pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "int";
  constraint["range"] = ">=1 <10";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 5 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type int + numeric range fail") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "int";
  constraint["range"] = ">=1 <10";
  f.schema["count"] = constraint;

  sol::object opts{ f.make_opts("{ count = 15 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: type float + numeric range pass") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "float";
  constraint["range"] = ">=0.0 <=1.0";
  f.schema["ratio"] = constraint;

  sol::object opts{ f.make_opts("{ ratio = 0.5 }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type float + numeric range fail") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "float";
  constraint["range"] = ">=0.0 <=1.0";
  f.schema["ratio"] = constraint;

  sol::object opts{ f.make_opts("{ ratio = 1.5 }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("does not satisfy range"));
}

TEST_CASE("OPTIONS: boolean choices accepts valid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  sol::table choices{ f.lua.create_table() };
  choices.add(true);
  constraint["choices"] = choices;
  f.schema["flag"] = constraint;

  sol::object opts{ f.make_opts("{ flag = true }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: boolean choices rejects invalid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  sol::table choices{ f.lua.create_table() };
  choices.add(true);
  constraint["choices"] = choices;
  f.schema["flag"] = constraint;

  sol::object opts{ f.make_opts("{ flag = false }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not in {true}"));
}

TEST_CASE("OPTIONS: empty list + choices passes") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "list";
  sol::table choices{ f.lua.create_table() };
  choices.add("a", "b");
  constraint["choices"] = choices;
  f.schema["items"] = constraint;

  sol::object opts{ f.make_opts("{ items = {} }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type semver + choices accepts valid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  sol::table choices{ f.lua.create_table() };
  choices.add("1.0.0", "2.0.0");
  constraint["choices"] = choices;
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"1.0.0\" }") };
  CHECK_NOTHROW(validate_options_schema(f.schema, opts, "test@v1"));
}

TEST_CASE("OPTIONS: type semver + choices rejects invalid") {
  lua_fixture f;
  sol::table constraint{ f.lua.create_table() };
  constraint["type"] = "semver";
  sol::table choices{ f.lua.create_table() };
  choices.add("1.0.0", "2.0.0");
  constraint["choices"] = choices;
  f.schema["version"] = constraint;

  sol::object opts{ f.make_opts("{ version = \"3.0.0\" }") };
  CHECK_THROWS_WITH(validate_options_schema(f.schema, opts, "test@v1"),
                    doctest::Contains("not in {1.0.0, 2.0.0}"));
}

}  // namespace
}  // namespace envy
