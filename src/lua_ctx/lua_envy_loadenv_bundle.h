#pragma once

#include "sol/sol.hpp"

#include <filesystem>

namespace envy {

class cache;

// envy.loadenv_bundle(alias, module) on a manifest's Lua state: `c` materializes the
// bundle mid-global-scope, `root_path` anchors an alias a fragment inherited.
void lua_envy_loadenv_bundle_install(sol::state &lua,
                                     cache *c,
                                     std::filesystem::path root_path);

// The refusal every other state gets: a spec is pointed at envy.loadenv_spec.
void lua_envy_loadenv_bundle_install_refusal(sol::table &envy_table);

}  // namespace envy
