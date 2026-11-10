#pragma once

#include <filesystem>
#include <string>

namespace envy {

// Extract the IDENTITY field from a spec file by executing it in a temporary Lua state.
// Returns the IDENTITY string or throws std::runtime_error on:
// - File not found
// - Lua parse/execution error
// - IDENTITY field missing or not a string
// - IDENTITY field empty
//
// If package_path_root is non-empty, configures Lua's package.path to enable
// bundle-local requires (e.g., for specs that use require("lib.helpers")).
// project_root becomes envy.PROJECT_ROOT, as lua_envy_install documents.
std::string extract_spec_identity(std::filesystem::path const &spec_path,
                                  std::filesystem::path const &package_path_root = {},
                                  std::filesystem::path const &project_root = {});

}  // namespace envy
