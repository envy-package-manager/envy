#include "lua_envy_path.h"

#include "lua_envy_module.h"
#include "sol_util.h"
#include "util.h"

#include <filesystem>
#include <string>

namespace envy {
namespace {

// Drops trailing separators, keeping a root ("/", "C:\"); parent_path drops all of them.
std::string trimmed_path(std::filesystem::path const &p) {
  return util_normalized_path(p.has_relative_path() && !p.has_filename() ? p.parent_path()
                                                                         : p);
}

std::string path_trim(sol::stack_object path) {
  return trimmed_path(
      std::filesystem::path{ sol_util_string_arg(path, "envy.path.trim") });
}

}  // namespace

void lua_envy_path_install(sol::table &envy_table) {
  sol::table path_table{ envy_table.lua_state(), sol::create };

  // envy.path.join(...) - Variadic path joining; one argument comes back trimmed
  path_table["join"] = [](sol::variadic_args va) -> std::string {
    std::filesystem::path result;
    for (auto const &arg : va) {
      if (!arg.is<std::string>()) {
        throw_runtime_error({ "envy.path.join: all arguments must be strings" });
      }
      std::string const part{ arg.as<std::string>() };
      if (result.empty()) {
        result = part;
      } else {
        result /= part;
      }
    }
    return va.size() == 1 ? trimmed_path(result) : util_normalized_path(result);
  };

  path_table.set_function("trim", &path_trim);  // envy.path.trim(path)

  // envy.path.basename(path) - Extract filename with extension
  path_table["basename"] = [](std::string const &path_str) -> std::string {
    return std::filesystem::path{ path_str }.filename().string();
  };

  // envy.path.dirname(path) - Extract parent directory path
  path_table["dirname"] = [](std::string const &path_str) -> std::string {
    return util_normalized_path(std::filesystem::path{ path_str }.parent_path());
  };

  // envy.path.stem(path) - Extract filename without extension
  path_table["stem"] = [](std::string const &path_str) -> std::string {
    return std::filesystem::path{ path_str }.stem().string();
  };

  // envy.path.extension(path) - Extract file extension with leading dot
  path_table["extension"] = [](std::string const &path_str) -> std::string {
    return std::filesystem::path{ path_str }.extension().string();
  };

  envy_table["path"] = path_table;

  // envy.abspath(path) - Resolve relative path against calling script's directory
  envy_table.set_function("abspath", [](sol::this_state L, std::string const &path_str) {
    std::filesystem::path const anchor{
      lua_module_caller_file(L, "envy.abspath").parent_path()
    };
    return util_normalized_path(util_absolute_path(path_str, anchor));
  });
}

}  // namespace envy
