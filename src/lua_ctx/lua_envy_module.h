#pragma once

#include "sol/sol.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace envy {

// Lua dot syntax ("lib.common") to a relative path, refusing every spelling that could
// escape: `.etc.passwd` built an absolute path, which `operator/` adopts whole.
std::string lua_module_subpath(std::string const &module_path,
                               std::string_view fn,
                               std::string const &scope);

// `root / <subpath>.lua`, asserted to stay under `root` -- the other input to the join.
std::filesystem::path lua_module_path_under(std::filesystem::path const &root,
                                            std::string const &subpath,
                                            std::string_view fn,
                                            std::string const &module_path,
                                            std::string const &scope);

// Execute `full_path` in a sandbox and hand back what it returned: its table, or the
// globals it assigned when it returned nothing -- the rule `require` already teaches.
sol::table lua_module_load(sol::state_view lua,
                           std::filesystem::path const &full_path,
                           std::string_view fn,
                           std::string const &module_path);

enum class caller_path { ABSOLUTE_, CANONICAL_ };  // wingdi defines ABSOLUTE

// The Lua file that called us. Never bare: a chunk name can be a lone filename.
std::filesystem::path lua_module_caller_file(lua_State *L,
                                             std::string_view fn,
                                             caller_path how = caller_path::ABSOLUTE_);

// envy.loadenv(module): a module path resolved against the calling file's directory.
void lua_envy_loadenv_install(sol::table &envy_table);

}  // namespace envy
