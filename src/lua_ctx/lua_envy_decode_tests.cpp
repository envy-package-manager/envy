#include "lua_envy.h"
#include "sol_util.h"

#include "doctest.h"

#include <string>

namespace envy {
namespace {

sol_state_ptr make_lua() {
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua, {});
  return lua;
}

// Runs a chunk that must return true; a false or an error fails with the chunk's message.
void check_lua(sol::state &lua, char const *chunk) {
  sol::protected_function_result const r{
    lua.safe_script(chunk, sol::script_pass_on_error)
  };
  if (!r.valid()) {
    sol::error const e = r;
    FAIL(std::string{ e.what() });
  }
  CHECK(r.get<bool>());
}

std::string lua_error(sol::state &lua, char const *chunk) {
  sol::protected_function_result const r{
    lua.safe_script(chunk, sol::script_pass_on_error)
  };
  REQUIRE_FALSE(r.valid());
  sol::error const e = r;
  return e.what();
}

}  // namespace

TEST_CASE("envy.json.decode maps JSON types onto Lua") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local v = envy.json.decode([[
      {"a": [1, 2.5, "x", true, false, null, {}], "b": {"c": "d"}} ]])
    return v.a[1] == 1 and math.type(v.a[1]) == "integer"
      and v.a[2] == 2.5 and v.a[3] == "x" and v.a[4] == true and v.a[5] == false
      and v.a[6] == nil and next(v.a[7]) == nil and v.b.c == "d"
  )lua");
}

TEST_CASE("envy.json.decode accepts scalar documents") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    return envy.json.decode('"s"') == "s" and envy.json.decode(" 42 ") == 42
      and envy.json.decode("null") == nil and envy.json.decode("true") == true
  )lua");
}

TEST_CASE("envy.json.decode keeps integers exact and floats float") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local v = envy.json.decode("[9007199254740993, -9223372036854775808, 1.0, 1e2]")
    return v[1] == 9007199254740993 and v[2] == math.mininteger
      and math.type(v[3]) == "float" and math.type(v[4]) == "float" and v[4] == 100
  )lua");
}

TEST_CASE("envy.json.decode decodes escapes and surrogate pairs to UTF-8") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    return envy.json.decode([["a\"\\\n\u00e9\ud83d\ude00"]]) == 'a"\\\n\u{e9}\u{1F600}'
  )lua");
}

TEST_CASE("envy.json.decode decodes upstream release indexes") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local releases = envy.json.decode([[
      [{"version": "go1.25.4", "stable": true, "files": [
        {"os": "darwin", "arch": "arm64", "kind": "archive",
         "filename": "go1.25.4.darwin-arm64.tar.gz", "sha256": "abc", "size": 61256178}]}]
    ]])
    local f = releases[1].files[1]
    return #releases == 1 and releases[1].stable and f.sha256 == "abc"
      and f.size == 61256178
  )lua");
}

TEST_CASE("envy.json.decode rejects malformed input") {
  auto lua{ make_lua() };
  CHECK(lua_error(*lua, "return envy.json.decode('{\"a\": }')")
            .find("envy.json.decode: syntax error at line 1") != std::string::npos);
  CHECK(lua_error(*lua, "return envy.json.decode('')").find("envy.json.decode:") !=
        std::string::npos);
  CHECK(lua_error(*lua, "return envy.json.decode('{} x')")
            .find("envy.json.decode: unexpected trailing characters at line 1") !=
        std::string::npos);
  CHECK(lua_error(*lua, "return envy.json.decode('42x')")
            .find("envy.json.decode: unexpected trailing") != std::string::npos);
  CHECK(lua_error(*lua, "return envy.json.decode('[1,]')").find("envy.json.decode:") !=
        std::string::npos);
}

TEST_CASE("envy.json.decode bounds nesting instead of overflowing the stack") {
  auto lua{ make_lua() };
  CHECK(lua_error(*lua, "return envy.json.decode(('['):rep(100000))")
            .find("envy.json.decode: nesting deeper than") != std::string::npos);
  CHECK(lua_error(*lua, "return envy.json.decode(('{\"a\":'):rep(100000))")
            .find("envy.json.decode: nesting deeper than") != std::string::npos);
  check_lua(*lua, R"lua(
    local v = envy.json.decode(("["):rep(100) .. ("]"):rep(100))
    for _ = 1, 99 do v = v[1] end
    return next(v) == nil
  )lua");
}

TEST_CASE("envy.toml.decode maps TOML types onto Lua") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local m = envy.toml.decode([=[
title = "x"
n = 9223372036854775807
f = 1.5
b = true
arr = [1, "two", [3]]
inf = inf
nan = nan
[[bin]]
name = "a"
[[bin]]
name = "b"
]=])
    return m.title == "x" and m.n == math.maxinteger and math.type(m.n) == "integer"
      and m.f == 1.5 and m.b == true and m.arr[1] == 1 and m.arr[2] == "two"
      and m.arr[3][1] == 3 and m.inf == math.huge and m.nan ~= m.nan
      and #m.bin == 2 and m.bin[1].name == "a" and m.bin[2].name == "b"
  )lua");
}

TEST_CASE("envy.toml.decode walks a Rust channel manifest") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local m = envy.toml.decode([=[
manifest-version = "2"
date = "2025-11-10"

[pkg.rust-std.target.x86_64-unknown-linux-gnu]
available = true
xz_url = "https://static.rust-lang.org/dist/rust-std-1.91.1-x86_64-linux.tar.xz"
xz_hash = "deadbeef"

[renames.rls]
to = "rls-preview"
]=])
    local entry = m.pkg["rust-std"].target["x86_64-unknown-linux-gnu"]
    return entry.available and entry.xz_hash == "deadbeef" and m.date == "2025-11-10"
      and m.renames.rls.to == "rls-preview" and m["manifest-version"] == "2"
  )lua");
}

TEST_CASE("envy.toml.decode renders date-times as RFC 3339 strings") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local m = envy.toml.decode([=[
odt = 1979-05-27T07:32:00Z
off = 1979-05-27 00:32:00.5-07:00
ldt = 1979-05-27T07:32:00.999999
ld = 1979-05-27
lt = 07:32:00
]=])
    return m.odt == "1979-05-27T07:32:00Z" and m.off == "1979-05-27T00:32:00.5-07:00"
      and m.ldt == "1979-05-27T07:32:00.999999" and m.ld == "1979-05-27"
      and m.lt == "07:32:00"
  )lua");
}

TEST_CASE("envy.toml.decode rejects malformed input") {
  auto lua{ make_lua() };
  CHECK(lua_error(*lua, "return envy.toml.decode('a = ')").find("envy.toml.decode:") !=
        std::string::npos);
  CHECK(lua_error(*lua, "return envy.toml.decode('a = 1\\na = 2')")
            .find("envy.toml.decode:") != std::string::npos);
  CHECK(lua_error(*lua, "return envy.toml.decode('a = \"\\255\"')")
            .find("envy.toml.decode: invalid UTF8") != std::string::npos);
  CHECK(lua_error(*lua, "return envy.toml.decode('a = ' .. ('['):rep(100000))")
            .find("envy.toml.decode:") != std::string::npos);
}

TEST_CASE("envy.json.decode and envy.toml.decode errors are catchable by pcall") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local jok, jerr = pcall(envy.json.decode, "x")
    local tok, terr = pcall(envy.toml.decode, "a = ")
    return not jok and jerr:find("envy.json.decode: ", 1, true) ~= nil
      and not tok and terr:find("envy.toml.decode: ", 1, true) ~= nil
  )lua");
}

TEST_CASE("envy.json.decode and envy.toml.decode reject non-string input") {
  auto lua{ make_lua() };
  CHECK(lua_error(*lua, "return envy.json.decode({})").find("envy.json.decode: ") !=
        std::string::npos);
  CHECK(lua_error(*lua, "return envy.toml.decode(nil)").find("envy.toml.decode: ") !=
        std::string::npos);
}

}  // namespace envy
