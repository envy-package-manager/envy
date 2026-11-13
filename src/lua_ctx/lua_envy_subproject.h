#pragma once

#include "sol/sol.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace envy {

// Install envy.subproject, and envy.IS_SUBPROJECT = false, into the root manifest's Lua
// state. Manifest scope only: composing manifests is a load-time act.
//
// Bootstrap boundary: the root manifest's header is the sole bootstrap authority.
// `root_version` is compared against a subproject's '@envy version' and nothing else is
// read from its header -- no re-exec, no cache root, no deploy.
void lua_envy_subproject_install(sol::state &lua,
                                 std::optional<std::string> const &root_version,
                                 std::filesystem::path const &root_path);

// BUNDLES tables of every subproject loaded so far, in load order. A custom-fetch bundle a
// subproject declares lives there, not in the root globals, so the manifest's
// fetch-function lookup searches these after its own.
std::vector<sol::table> lua_envy_subproject_bundle_tables(sol::state_view lua);

// Throw if a subproject declared a root-only global -- one read only from the root state,
// DEFAULT_SHELL or VENDOR_ROOT -- that the root globals did not end up holding. A
// superproject may splice such a declaration up (`DEFAULT_SHELL =
// envy.subproject("sub").DEFAULT_SHELL`); leaving it in the sandbox drops it silently.
// Call once, after the root chunk has run.
void lua_envy_subproject_validate_root_globals(sol::state_view lua);

}  // namespace envy
