#include "lua_envy_path.h"

#include "util.h"

#include <filesystem>
#include <string>

namespace envy {

void lua_envy_path_install(sol::table &envy_table) {
  sol::table path_table{ envy_table.lua_state(), sol::create };

  // envy.path.join(...) - Variadic path joining
  path_table["join"] = [](sol::variadic_args va) -> std::string {
    std::filesystem::path result;
    for (auto const &arg : va) {
      if (!arg.is<std::string>()) {
        throw std::runtime_error("envy.path.join: all arguments must be strings");
      }
      std::string const part{ arg.as<std::string>() };
      if (result.empty()) {
        result = part;
      } else {
        result /= part;
      }
    }
    return util_normalized_path(result);
  };

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
    std::string const source{ [&] {
      sol::state_view lua{ L };
      sol::table const info = lua["debug"]["getinfo"](2, "S");
      sol::optional<std::string> const source_opt = info["source"];
      if (!source_opt) {
        throw std::runtime_error("envy.abspath: cannot determine caller's source file");
      }

      std::string s{ *source_opt };
      if (!s.empty() && s[0] == '@') { s = s.substr(1); }  // Strip "@" file-source prefix

      return s;
    }() };

    std::filesystem::path const anchor{ std::filesystem::path{ source }.parent_path() };
    if (anchor.empty()) {
      throw std::runtime_error("envy.abspath: cannot determine script directory");
    }

    return util_normalized_path(util_absolute_path(path_str, anchor));
  });
}

}  // namespace envy
