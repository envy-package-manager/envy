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
  sol::protected_function_result const r{ lua.safe_script(chunk,
                                                          sol::script_pass_on_error) };
  if (!r.valid()) {
    sol::error const e = r;
    FAIL(std::string{ e.what() });
  }
  CHECK(r.get<bool>());
}

std::string lua_error(sol::state &lua, char const *chunk) {
  sol::protected_function_result const r{ lua.safe_script(chunk,
                                                          sol::script_pass_on_error) };
  REQUIRE_FALSE(r.valid());
  sol::error const e = r;
  return e.what();
}

}  // namespace

TEST_CASE("envy.toml.decode maps TOML types onto Lua") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local m = envy.toml.decode([=[
title = "x"
n = 9223372036854775807
f = 1.5
b = true
arr = [1, "two", [3]]
mixed = [{ a = [1, { b = [2, []] }, {}] }, [], { c = { d = 3 } }, 4]
inf = inf
nan = nan
[x.y.z]
w = 1
[[bin]]
name = "a"
[[bin]]
name = "b"
]=])
    local x = m.mixed
    return m.title == "x" and m.n == math.maxinteger and math.type(m.n) == "integer"
      and m.f == 1.5 and m.b == true and m.arr[1] == 1 and m.arr[2] == "two"
      and m.arr[3][1] == 3 and m.inf == math.huge and m.nan ~= m.nan
      and #m.bin == 2 and m.bin[1].name == "a" and m.bin[2].name == "b"
      and #x == 4 and x[1].a[1] == 1 and x[1].a[2].b[1] == 2
      and next(x[1].a[2].b[2]) == nil
      and next(x[1].a[3]) == nil and next(x[2]) == nil and x[3].c.d == 3 and x[4] == 4
      and m.x.y.z.w == 1
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

TEST_CASE("envy.toml.decode errors are catchable by pcall") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local ok, err = pcall(envy.toml.decode, "a = ")
    return not ok and err:find("envy.toml.decode: ", 1, true) ~= nil
  )lua");
}

TEST_CASE("envy.toml.decode rejects non-string input") {
  auto lua{ make_lua() };
  CHECK(lua_error(*lua, "return envy.toml.decode(nil)")
            .find("envy.toml.decode: expected a string, got nil") != std::string::npos);
  check_lua(*lua, "return next(envy.toml.decode('')) == nil");
}

TEST_CASE("envy.toml.decode tags arrays so empty ones round-trip") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local m = envy.toml.decode("a = []\nb = {}\n")
    return getmetatable(m.a) ~= nil and getmetatable(m.b) == nil
      and envy.toml.encode(m) == "a = []\nb = {}\n"
  )lua");
}

TEST_CASE("envy.toml.encode writes key/values, sections and arrays of tables") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local s = envy.toml.encode({
      name = "x", n = 1, f = 1.5, on = true, list = envy.toml.array(), empty = {},
      pts = { { x = 1, y = 2 }, 3 }, inf = 1/0, ninf = -1/0,
      dep = { a = "1", b = { version = "2" } },
      bin = { { name = "a" }, { name = "b", path = "p" } },
      only = { sub = { k = 1 } },
    })
    return s == table.concat({
      'empty = {}', 'f = 1.5', 'inf = inf', 'list = []', 'n = 1', 'name = "x"',
      'ninf = -inf', 'on = true', 'pts = [{ x = 1, y = 2 }, 3]', '',
      '[[bin]]', 'name = "a"', '',
      '[[bin]]', 'name = "b"', 'path = "p"', '',
      '[dep]', 'a = "1"', '',
      '[dep.b]', 'version = "2"', '',
      '[only.sub]', 'k = 1', '' }, "\n")
  )lua");
}

TEST_CASE("envy.toml.encode quotes keys that are not bare") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local s = envy.toml.encode({ ["a b"] = 1, [""] = 2,
                                 ["x.y"] = { z = { ["q\"r"] = 3 } } })
    return s == '"" = 2\n"a b" = 1\n\n["x.y".z]\n"q\\"r" = 3\n'
  )lua");
}

TEST_CASE("envy.toml round-trips documents") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local text = table.concat({
      'date = "2025-11-10"', 'manifest-version = "2"', '',
      '[[pkg.rust.components]]', 'pkg = "rustc"', '',
      '[[pkg.rust.components]]', 'pkg = "cargo"', '',
      '[pkg.rust.target.x86_64-unknown-linux-gnu]', 'available = true',
      'xz_hash = "ab"', 'xz_url = "https://x/y.tar.xz"', '' }, "\n")
    local m = envy.toml.decode(text)
    return envy.toml.encode(m) == text
      and envy.toml.encode(envy.toml.decode(envy.toml.encode(m))) == text
  )lua");
}

TEST_CASE("envy.toml.encode nests inline values on the heap, not the C stack") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local n, root = 200000, {}
    local t = root
    for _ = 1, n - 1 do t[1] = {}; t = t[1] end
    return envy.toml.encode({ a = root }) == "a = " .. ("["):rep(n - 1) .. "{}"
      .. ("]"):rep(n - 1) .. "\n"
  )lua");
}

TEST_CASE("envy.toml.encode rejects what TOML cannot hold") {
  auto lua{ make_lua() };
  auto const err{ [&](char const *chunk) { return lua_error(*lua, chunk); } };
  CHECK(err("return envy.toml.encode(42)")
            .find("envy.toml.encode: a TOML document must be a table") !=
        std::string::npos);
  CHECK(err("return envy.toml.encode({ 1, 2 })")
            .find("a TOML document must be a table, not an array") != std::string::npos);
  CHECK(err("local t = { x = {} }; t.x.y = t; return envy.toml.encode(t)")
            .find("envy.toml.encode: table contains itself at x.y") != std::string::npos);
  CHECK(err("local t = { x = {} }; t.x.y = { t, 1 }; return envy.toml.encode(t)")
            .find("envy.toml.encode: table contains itself at x.y[1]") !=
        std::string::npos);
  CHECK(err("return envy.toml.encode({ a = { b = print } })")
            .find("envy.toml.encode: cannot encode a function at a.b") !=
        std::string::npos);
  CHECK(err("local a = envy.toml.array(); a[2] = 1; return envy.toml.encode({ a = a })")
            .find("envy.toml.encode: array has holes at a") != std::string::npos);
  CHECK(err("return envy.toml.encode({ ['\\255'] = 1 })")
            .find("string is not valid UTF-8") != std::string::npos);
}

}  // namespace envy
