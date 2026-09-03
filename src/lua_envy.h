#pragma once

#include "sol/sol.hpp"

namespace envy {

// Lua registry indices for envy runtime state
inline constexpr int ENVY_OPTIONS_RIDX = 100;    // Options table for phase execution
inline constexpr int ENVY_PHASE_CTX_RIDX = 103;  // Phase context pointer for envy.* APIs
inline constexpr int ENVY_IMPORTS_RIDX = 104;    // BUNDLES tables of imported manifests

// Install envy globals, platform constants, and custom functions into Lua state
void lua_envy_install(sol::state &lua);

}  // namespace envy
