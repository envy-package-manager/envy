#pragma once

#include "sol/sol.hpp"

#include <filesystem>

namespace envy {

// Lua registry indices for envy runtime state
inline constexpr int ENVY_OPTIONS_RIDX = 100;    // Options table for phase execution
inline constexpr int ENVY_PHASE_CTX_RIDX = 103;  // Phase context pointer for envy.* APIs
inline constexpr int ENVY_SUBPROJECT_BUNDLES_RIDX = 104;  // BUNDLES tables of subprojects
inline constexpr int ENVY_SUBPROJECT_ENVS_RIDX = 105;  // {path, env} of every subproject

// Install envy globals, platform constants, and custom functions into Lua state.
// project_root becomes envy.PROJECT_ROOT; empty leaves it nil (no project: `envy lua`).
void lua_envy_install(sol::state &lua, std::filesystem::path const &project_root);

}  // namespace envy
