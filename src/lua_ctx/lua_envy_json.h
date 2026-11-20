#pragma once

#include "sol/sol.hpp"

namespace envy {

// Install envy.json.* into the envy table
void lua_envy_json_install(sol::table &envy_table);

}  // namespace envy
