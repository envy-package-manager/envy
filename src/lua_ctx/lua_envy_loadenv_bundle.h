#pragma once

#include "sol/sol.hpp"

#include <filesystem>

namespace envy {

class cache;

// Install envy.loadenv_bundle(alias, module) for real, on a manifest's Lua state.
// `c` materializes the bundle during the manifest's own global scope, which is
// earlier than the engine fetches bundles -- the manifest's PACKAGES are built from
// what the helper hands back, so nothing downstream exists yet to fetch it.
// A null `c` means the command loaded this manifest without one, and says so.
// `root_path` anchors an alias an imported fragment inherited rather than wrote.
void lua_envy_loadenv_bundle_install(sol::state &lua,
                                     cache *c,
                                     std::filesystem::path root_path);

// The refusal every other Lua state gets, so a spec calling it is pointed at
// envy.loadenv_spec instead of at a nil field.
void lua_envy_loadenv_bundle_install_refusal(sol::table &envy_table);

}  // namespace envy
