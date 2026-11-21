#include "lua_envy_functional_test.h"

#include "util.h"

#include <filesystem>
#include <string_view>

namespace envy {
namespace {

// "libcxx" throws from inside libc++'s dylib, where macOS -fno-rtti code can't catch it as
// std::exception; anything else throws from envy.
void test_throw(std::string_view kind) {
  if (kind == "libcxx") { std::filesystem::create_directory(std::filesystem::path{}); }
  throw_runtime_error({ "test_throw: thrown by envy" });
}

int test_throw_past_lua(lua_State *L) {
  test_throw(luaL_checkstring(L, 1));
  return 0;
}

}  // namespace

void lua_envy_functional_test_install(sol::table &envy_table) {
  envy_table.set_function("test_throw", test_throw);  // sol's trampoline, like envy.*
  lua_State *const L{ envy_table.lua_state() };  // raw C function: unwinds to the C++ caller
  envy_table.push();
  lua_pushcfunction(L, test_throw_past_lua);
  lua_setfield(L, -2, "test_throw_past_lua");
  lua_pop(L, 1);
}

}  // namespace envy
