#pragma once

#include "sol/sol.hpp"

#include <string_view>

namespace envy {

// Functional tester only: envy.test_throw(kind). Named so the LuaLS stub check skips them.
inline constexpr std::string_view kLuaEnvyFunctionalTestHooks[]{ "test_throw" };

void lua_envy_functional_test_install(sol::table &envy_table);

}  // namespace envy
