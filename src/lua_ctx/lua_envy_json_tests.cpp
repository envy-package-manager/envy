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

TEST_CASE("envy.json.decode nests containers in containers") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local v = envy.json.decode('[{"a": [1, {"b": [2, []]}, {}]}, [], {"c": {"d": 3}}, 4]')
    return #v == 4 and v[1].a[1] == 1 and v[1].a[2].b[1] == 2
      and next(v[1].a[2].b[2]) == nil
      and next(v[1].a[3]) == nil and next(v[2]) == nil and v[3].c.d == 3 and v[4] == 4
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

TEST_CASE("envy.json.decode nests on the heap, not the C stack") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local n = 200000
    local a = envy.json.decode(("["):rep(n) .. ("]"):rep(n))
    for _ = 1, n - 1 do a = a[1] end
    local o = envy.json.decode(('{"k":'):rep(n) .. "1" .. ("}"):rep(n))
    for _ = 1, n do o = o.k end
    return next(a) == nil and o == 1
  )lua");
  CHECK(lua_error(*lua, "return envy.json.decode(('{\"k\":'):rep(600000))")
            .find("envy.json.decode: nesting too deep") != std::string::npos);
}

TEST_CASE("envy.json.decode errors are catchable by pcall") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local ok, err = pcall(envy.json.decode, "x")
    return not ok and err:find("envy.json.decode: ", 1, true) ~= nil
  )lua");
}

TEST_CASE("envy.json.decode rejects non-string input") {
  auto lua{ make_lua() };
  CHECK(lua_error(*lua, "return envy.json.decode({})")
            .find("envy.json.decode: expected a string, got table") != std::string::npos);
  CHECK(lua_error(*lua, "return envy.json.decode(42)")
            .find("envy.json.decode: expected a string, got number") != std::string::npos);
}

TEST_CASE("envy.json.decode enforces RFC 8259 number grammar") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    for _, s in ipairs({ "01", "-01", "2.", "-.5", "0.e1", "2.e3", ".5", "+1", "1e",
                         "1e+", "-", "--1", "0x10", "1.5e", "Infinity", "NaN", "[01]",
                         "[1.]" }) do
      if pcall(envy.json.decode, s) then error("accepted " .. s) end
    end
    local v = envy.json.decode("[0, -0, 0.5, 1e10, 1E-3, -1.5e+2, 123]")
    return v[1] == 0 and v[2] == 0 and v[3] == 0.5 and v[4] == 1e10 and v[5] == 0.001
      and v[6] == -150 and v[7] == 123 and math.type(v[7]) == "integer"
  )lua");
}

TEST_CASE("envy.json.decode requires UTF-8 and skips a BOM") {
  auto lua{ make_lua() };
  CHECK(lua_error(*lua, "return envy.json.decode('\"\\255\"')")
            .find("envy.json.decode: invalid UTF-8") != std::string::npos);
  check_lua(*lua, "return envy.json.decode('\\239\\187\\191[1]')[1] == 1");
}

TEST_CASE("envy.json.encode writes scalars") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local e = envy.json.encode
    return e(nil) == "null" and e(true) == "true" and e(false) == "false" and e(42) == "42"
      and e(-7) == "-7" and e(1.5) == "1.5" and e(1.0) == "1.0" and e(0.1) == "0.1"
      and e(-0.0) == "-0.0" and e(1e300) == "1e+300"
      and e(math.mininteger) == "-9223372036854775808"
      and e('a"\\\n\t\1\127\u{e9}') == '"a\\"\\\\\\n\\t\\u0001\\u007F\u{e9}"'
  )lua");
}

TEST_CASE("envy.json.encode sorts object keys and honors array tags") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local e = envy.json.encode
    return e({ b = 1, a = { 2, 3 }, c = {} }) == '{"a":[2,3],"b":1,"c":{}}'
      and e(envy.json.array()) == "[]" and e(envy.json.array({ 1 })) == "[1]"
      and e(envy.json.decode("[]")) == "[]" and e(envy.json.decode("{}")) == "{}"
      and e({ { {} } }) == "[[{}]]" and e(envy.json.decode("[1,null,2]")) == "[1,null,2]"
  )lua");
}

TEST_CASE("envy.json.encode pretty-prints with indent") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local s = envy.json.encode({ name = "x", list = { 1, { a = true } }, none = {} },
                               { indent = 2 })
    return s == '{\n  "list": [\n    1,\n    {\n      "a": true\n    }\n  ],\n'
             .. '  "name": "x",\n  "none": {}\n}'
  )lua");
}

TEST_CASE("envy.json round-trips documents") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local text = '{"a":[1,2.5,"x",true,false,{}],"b":{"c":[[]],"d":"\\u00e9\\n"},"e":-3}'
    return envy.json.encode(envy.json.decode(text)) == text:gsub("\\u00e9", "\u{e9}")
  )lua");
}

TEST_CASE("envy.json.encode nests on the heap, not the C stack") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local n, root = 200000, {}
    local t = root
    for _ = 1, n - 1 do t[1] = {}; t = t[1] end
    local s = envy.json.encode(root)
    return s == ("["):rep(n - 1) .. "{}" .. ("]"):rep(n - 1)
  )lua");
}

TEST_CASE("envy.json.encode rejects what JSON cannot hold") {
  auto lua{ make_lua() };
  auto const err{ [&](char const *chunk) { return lua_error(*lua, chunk); } };
  CHECK(err("return envy.json.encode({ a = { print } })")
            .find("envy.json.encode: cannot encode a function at a[1]") !=
        std::string::npos);
  CHECK(err("return envy.json.encode({ x = 1/0 })")
            .find("envy.json.encode: JSON has no inf or nan at x") != std::string::npos);
  CHECK(err("local t = {}; t.self = t; return envy.json.encode(t)")
            .find("envy.json.encode: table contains itself at self") != std::string::npos);
  CHECK(err("return envy.json.encode({ 1, a = 2 })")
            .find("table mixes array indices and string keys") != std::string::npos);
  CHECK(err("return envy.json.encode({ [1] = 1, [3] = 3 })").find("array has holes") !=
        std::string::npos);
  CHECK(err("return envy.json.encode({ [true] = 1 })")
            .find("table keys must be strings or array indices") != std::string::npos);
  CHECK(err("return envy.json.encode('\\255')").find("string is not valid UTF-8") !=
        std::string::npos);
  CHECK(err("return envy.json.encode({}, { indnet = 2 })").find("unknown option") !=
        std::string::npos);
  CHECK(err("return envy.json.encode({}, { indent = 2.5 })")
            .find("indent must be an integer from 0 to 10") != std::string::npos);
}

}  // namespace envy
