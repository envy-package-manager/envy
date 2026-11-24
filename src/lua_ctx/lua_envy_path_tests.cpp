#include "lua_envy.h"
#include "sol_util.h"

#include "doctest.h"

#include <string>

namespace envy {
namespace {

// P(s) respells s's forward slashes in the platform separator, as envy.path returns them.
sol_state_ptr make_lua() {
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua, {});
  lua->safe_script(R"lua(
    S = package.config:sub(1, 1)
    function P(s) return (s:gsub("/", S)) end
  )lua");
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

}  // namespace

TEST_CASE("envy.path.trim drops trailing separators and keeps a root") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local t = envy.path.trim
    return t("/a/b/") == P("/a/b") and t("/a/b//") == P("/a/b") and t("/a/b") == P("/a/b")
      and t("a/") == "a" and t("./") == "." and t("/") == P("/") and t("") == ""
  )lua");
}

TEST_CASE("envy.path.trim follows the platform's separators") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local t = envy.path.trim
    if S == "\\" then
      return t("C:/") == "C:\\" and t("C:\\x\\") == "C:\\x" and t("C:/x/") == "C:\\x"
    end
    return t("a\\") == "a\\"  -- a backslash is a filename character off Windows
  )lua");
}

TEST_CASE("envy.path.trim and join keep Lua's UTF-8, never the ANSI code page") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local d = "/caf\u{e9}/\u{65e5}\u{672c}"
    return envy.path.trim(d .. "/") == P(d) and envy.path.join(d .. "/") == P(d)
      and envy.path.join(d, "na\u{ef}ve") == P(d .. "/na\u{ef}ve")
  )lua");
}

TEST_CASE("envy.path.trim raises a catchable error on a non-string, number included") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local ok1, err1 = pcall(envy.path.trim, {})
    local ok2, err2 = pcall(envy.path.trim, 42)
    local want = "envy.path.trim: expected a string, got "
    return not ok1 and tostring(err1):find(want .. "table", 1, true) ~= nil
      and not ok2 and tostring(err2):find(want .. "number", 1, true) ~= nil
  )lua");
}

TEST_CASE("envy.path.join trims one argument and keeps a joined trailing separator") {
  auto lua{ make_lua() };
  check_lua(*lua, R"lua(
    local j = envy.path.join
    return j("/a/b/") == P("/a/b") and j("/") == P("/") and j() == ""
      and j("/a/", "b/") == P("/a/b/") and j("/a/", "b") == P("/a/b")
  )lua");
}

}  // namespace envy
