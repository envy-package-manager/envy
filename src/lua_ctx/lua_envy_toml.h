#pragma once

#include "sol/sol.hpp"

namespace envy {

// Install envy.toml.* into the envy table
void lua_envy_toml_install(sol::table &envy_table);

}  // namespace envy
