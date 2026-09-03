#pragma once

#include "sol/sol.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

// Install envy.import into a manifest Lua state. Manifest scope only: composing
// manifests is a load-time act, so no spec state gets it.
//
// Bootstrap boundary: the root manifest's header is the sole bootstrap authority.
// `root_version` is compared against an imported manifest's '@envy version' and
// nothing else is read from its header -- no re-exec, no cache root, no deploy.
void lua_envy_import_install(sol::state &lua,
                             std::optional<std::string> const &root_version,
                             std::filesystem::path const &root_path);

// BUNDLES tables of every manifest imported so far, in import order. A custom-fetch
// bundle an imported manifest declares lives there, not in the root globals, so the
// manifest's fetch-function lookup searches these after its own.
std::vector<sol::table> lua_envy_import_bundle_tables(sol::state_view lua);

}  // namespace envy
