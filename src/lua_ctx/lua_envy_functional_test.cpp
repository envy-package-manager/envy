#include "lua_envy_functional_test.h"

#include "util.h"

#include <string>
#include <string_view>

namespace envy {
namespace {

// "stdlib" throws from inside the C++ runtime's compiled code (libc++.dylib on macOS,
// which -fno-rtti code can't catch as std::exception); anything else throws from envy.
void test_throw(std::string_view kind) {
  if (kind == "stdlib") { static_cast<void>(std::stoi("envy.test_throw")); }
  throw_runtime_error({ "test_throw: thrown by envy" });
}

}  // namespace

void lua_envy_functional_test_install(sol::table &envy_table) {
  envy_table.set_function("test_throw", test_throw);
}

}  // namespace envy
