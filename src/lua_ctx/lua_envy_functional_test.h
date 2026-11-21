#pragma once

#include "sol/sol.hpp"

namespace envy {

// Functional tester only: envy.test_throw(kind) and envy.test_throw_past_lua(kind).
void lua_envy_functional_test_install(sol::table &envy_table);

}  // namespace envy
