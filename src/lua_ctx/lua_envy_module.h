#pragma once

#include "sol/sol.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace envy {

// Lua dot syntax ("lib.common") to a relative file path ("lib/common"), refusing
// every spelling that could name something outside the root before the dots become
// separators: `.etc.passwd` built an absolute path, which `operator/` adopts whole.
// `fn` is the envy function to blame; `scope` names what the path could not leave.
std::string lua_module_subpath(std::string const &module_path,
                               std::string_view fn,
                               std::string const &scope);

// `root / <subpath>.lua`, having asserted it stays under `root`. The dot-syntax check
// forbids every escape a module path can spell; this covers the only other input.
std::filesystem::path lua_module_path_under(std::filesystem::path const &root,
                                            std::string const &subpath,
                                            std::string_view fn,
                                            std::string const &module_path,
                                            std::string const &scope);

// Execute `full_path` in a fresh sandbox whose __index is the state's globals, and
// hand back what the module returned: its table if it returned one, else the globals
// it assigned. `require` gives the return value, which is what every module written
// since Lua 5.1 hands back -- returning the globals instead handed that shape an
// empty table, and the failure surfaced somewhere else entirely.
sol::table lua_module_load(sol::state_view lua,
                           std::filesystem::path const &full_path,
                           std::string_view fn,
                           std::string const &module_path);

// How a caller's path is spelled once found. ABSOLUTE is enough to anchor a lookup
// beside it; CANONICAL is what comparing two spellings of one file needs, which is
// envy.import's cycle test.
enum class caller_path { ABSOLUTE, CANONICAL };

// The Lua file that called us. Never bare: a chunk name can be a lone filename,
// which names the CWD.
std::filesystem::path lua_module_caller_file(lua_State *L,
                                             std::string_view fn,
                                             caller_path how = caller_path::ABSOLUTE);

// envy.loadenv(module): a module path resolved against the calling file's directory.
void lua_envy_loadenv_install(sol::table &envy_table);

}  // namespace envy
