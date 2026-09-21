#include "lua_envy_module.h"

#include "util.h"

#include <algorithm>
#include <stdexcept>

namespace envy {

namespace {

namespace fs = std::filesystem;

std::string prefix(std::string_view fn) { return std::string{ fn } + ": "; }

// Lua's own name for a value's type, so a refusal reads the way the interpreter's
// own errors do.
char const *lua_type_name(sol::object const &o) {
  return lua_typename(o.lua_state(), static_cast<int>(o.get_type()));
}

}  // namespace

std::string lua_module_subpath(std::string const &module_path,
                               std::string_view fn,
                               std::string const &scope) {
  auto const reject{ [&](std::string const &why) {
    return std::runtime_error(prefix(fn) + "invalid module path '" + module_path +
                              "' for " + scope + ": " + why);
  } };

  if (module_path.empty()) { throw reject("path is empty"); }
  if (module_path.front() == '.' || module_path.back() == '.') {
    throw reject("a leading or trailing '.' does not separate modules");
  }
  if (module_path.find_first_of("/\\") != std::string::npos) {
    throw reject("write Lua dot syntax ('lib.common'), not path separators");
  }
  if (module_path.find("..") != std::string::npos) {
    throw reject("'..' cannot reach outside " + scope);
  }

  std::string subpath{ module_path };
  std::replace(subpath.begin(), subpath.end(), '.', '/');
  return subpath;
}

fs::path lua_module_path_under(fs::path const &root,
                               std::string const &subpath,
                               std::string_view fn,
                               std::string const &module_path,
                               std::string const &scope) {
  fs::path const base{ root.lexically_normal() };
  fs::path const full{ (base / (subpath + ".lua")).lexically_normal() };
  if (auto const rel{ full.lexically_relative(base) };
      rel.empty() || *rel.begin() == "..") {
    throw std::runtime_error(prefix(fn) + "module '" + module_path +
                             "' resolves outside " + scope + ": " + full.string());
  }
  return full;
}

sol::table lua_module_load(sol::state_view lua,
                           fs::path const &full_path,
                           std::string_view fn,
                           std::string const &module_path) {
  if (!fs::exists(full_path)) {
    throw std::runtime_error(prefix(fn) + "file not found: " + full_path.string());
  }

  auto const bytes{ util_load_file(full_path) };
  std::string const content{ reinterpret_cast<char const *>(bytes.data()), bytes.size() };

  sol::load_result chunk{ lua.load(content, full_path.string()) };
  if (!chunk.valid()) {
    sol::error const err = chunk;
    throw std::runtime_error(prefix(fn) + "load error: " + err.what());
  }

  // Assigned globals land here rather than in the caller's; the stdlib stays visible
  // through the metatable.
  sol::environment env{ lua, sol::create, lua.globals() };
  sol::protected_function fnc{ chunk };
  sol::set_environment(env, fnc);

  sol::protected_function_result const result{ fnc() };
  if (!result.valid()) {
    sol::error const err = result;
    throw std::runtime_error(prefix(fn) + "exec error: " + err.what());
  }

  sol::object const ret{ result.return_count() ? result.get<sol::object>(0)
                                               : sol::object{} };
  if (!ret.valid() || ret.get_type() == sol::type::lua_nil) { return env; }
  if (!ret.is<sol::table>()) {
    throw std::runtime_error(prefix(fn) + "module '" + module_path + "' returned a " +
                             lua_type_name(ret) + "; a module returns a table or nothing");
  }
  return ret.as<sol::table>();
}

fs::path lua_module_caller_file(lua_State *L, std::string_view fn, caller_path how) {
  sol::state_view lua{ L };
  // Copy-init, not braces: MSVC reads a braced sol proxy as an initializer list.
  sol::table const info = lua["debug"]["getinfo"](2, "S");  // 2 = caller of this C fn
  sol::optional<std::string> const source = info["source"];
  if (!source) {
    throw std::runtime_error(prefix(fn) + "cannot determine caller's source file");
  }

  std::string_view s{ *source };
  if (!s.empty() && s.front() == '@') { s.remove_prefix(1); }  // file-source prefix
  fs::path const p{ s };
  return how == caller_path::CANONICAL ? util_canonical_path(p) : fs::absolute(p);
}

void lua_envy_loadenv_install(sol::table &envy_table) {
  envy_table["loadenv"] = [](std::string const &module_path,
                             sol::this_state L) -> sol::table {
    constexpr std::string_view kFn{ "envy.loadenv" };
    if (module_path.empty()) {
      throw std::runtime_error(std::string{ kFn } + ": path must be a non-empty string");
    }

    // Looser than its siblings on purpose: this path is anchored on the caller's own
    // directory, not inside a dependency, and a separator in it has always just
    // worked. Containment is not looser -- `operator/` adopts an absolute subpath
    // whole, so only the assertion below keeps `/tmp/x` from loading /tmp/x.lua.
    std::string subpath{ module_path };
    std::replace(subpath.begin(), subpath.end(), '.', '/');

    fs::path const dir{ lua_module_caller_file(L, kFn).parent_path() };
    fs::path const full_path{ lua_module_path_under(dir,
                                                    subpath,
                                                    kFn,
                                                    module_path,
                                                    "the calling file's directory") };
    return lua_module_load(sol::state_view{ L }, full_path, kFn, module_path);
  };
}

}  // namespace envy
