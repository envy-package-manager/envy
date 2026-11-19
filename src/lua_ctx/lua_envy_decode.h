#pragma once

#include "sol/sol.hpp"

namespace envy {

// Install envy.json.decode and envy.toml.decode into the envy table
void lua_envy_decode_install(sol::table &envy_table);

}  // namespace envy
