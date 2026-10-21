#include "shell.h"
#include "util.h"

#include "sol/sol.hpp"

#include <filesystem>
#include <stdexcept>
#include <variant>
#include <vector>

namespace envy {

resolved_shell shell_resolve_default(default_shell_cfg_t const *cfg) {
  if (cfg && *cfg) {
    return envy::visit(
        match{ [](shell_choice choice) { return resolved_shell{ choice }; },
               [](custom_shell const &custom) {
                 return envy::visit(
                     [](auto const &custom_cfg) { return resolved_shell{ custom_cfg }; },
                     custom);
               } },
        **cfg);
  }

#if defined(_WIN32)
  return shell_choice::powershell;
#else
  return shell_choice::bash;
#endif
}

shell_choice shell_parse_choice(std::optional<std::string_view> value) {
#if defined(_WIN32)
  if (!value || value->empty()) { return shell_choice::powershell; }
  if (*value == "powershell") { return shell_choice::powershell; }
  if (*value == "cmd") { return shell_choice::cmd; }
  throw std::invalid_argument("shell option must be 'powershell' or 'cmd' on Windows");
#else
  if (!value || value->empty()) { return shell_choice::bash; }
  if (*value == "bash") { return shell_choice::bash; }
  if (*value == "sh") { return shell_choice::sh; }
  throw std::invalid_argument("shell option must be 'bash' or 'sh' on POSIX");
#endif
}

namespace {

template <typename T>
void validate_shell_config(T const &shell_cfg) {
  if (shell_cfg.argv.empty()) {
    throw_runtime_error({ "Custom shell argv must be non-empty" });
  }

  auto const shell_path{ std::filesystem::path{ shell_cfg.argv[0] } };

  // Check if shell executable exists
  std::error_code ec;
  if (!std::filesystem::exists(shell_path, ec)) {
    throw_runtime_error({ "Custom shell executable not found: ", shell_path });
  }

  // Check if it's a regular file (not a directory)
  if (!std::filesystem::is_regular_file(shell_path, ec)) {
    throw_runtime_error({ "Custom shell path is not a regular file: ", shell_path });
  }

  // Note: We don't check executable permission on Windows (no reliable portable way)
  // On POSIX, we could use std::filesystem::perms, but that's also unreliable
  // (doesn't account for ACLs, setuid, etc.). Better to fail at execution time.
}

}  // namespace

void shell_validate_custom(custom_shell const &cfg) {
  envy::visit([](auto const &shell_cfg) { validate_shell_config(shell_cfg); }, cfg);
}

custom_shell shell_parse_custom_from_lua(sol::table const &tbl) {
  // Check for 'file' and 'inline' keys
  sol::object file_obj{ tbl["file"] };
  sol::object inline_obj{ tbl["inline"] };

  bool const has_file{ file_obj.valid() && file_obj.get_type() != sol::type::lua_nil };
  bool const has_inline{ inline_obj.valid() &&
                         inline_obj.get_type() != sol::type::lua_nil };

  if (has_file && has_inline) {
    throw_runtime_error(
        { "custom shell table cannot have both 'file' and 'inline' keys" });
  }
  if (!has_file && !has_inline) {
    throw_runtime_error({ "custom shell table must have either 'file' or 'inline' key" });
  }

  if (has_file) {
    // Parse file mode - handle string shorthand: file = "/path" → file = {"/path"}
    std::vector<std::string> argv;

    if (file_obj.is<std::string>()) {
      argv.push_back(file_obj.as<std::string>());
    } else if (file_obj.is<sol::table>()) {
      sol::table arr{ file_obj.as<sol::table>() };
      size_t const len{ arr.size() };
      if (len == 0) {
        throw_runtime_error(
            { "file mode argv must be non-empty (at least shell executable path)" });
      }
      for (size_t i{ 1 }; i <= len; ++i) {
        sol::object elem{ arr[i] };
        if (!elem.is<std::string>()) {
          throw_runtime_error({ "file mode argv must contain only strings" });
        }
        argv.push_back(elem.as<std::string>());
      }
    } else {
      throw_runtime_error({ "'file' key must be a string (path) or array of strings" });
    }

    // Get 'ext' field (required for file mode)
    sol::object ext_obj{ tbl["ext"] };
    if (!ext_obj.is<std::string>()) {
      throw_runtime_error({ "file mode requires 'ext' field (e.g., \".sh\", \".tcl\")" });
    }
    std::string ext{ ext_obj.as<std::string>() };

    return custom_shell_file{ std::move(argv), std::move(ext) };
  } else {
    // Parse inline mode
    std::vector<std::string> argv;

    if (inline_obj.is<sol::table>()) {
      sol::table arr{ inline_obj.as<sol::table>() };
      size_t const len{ arr.size() };
      if (len == 0) {
        throw_runtime_error(
            { "inline mode argv must be non-empty (at least shell executable path)" });
      }
      for (size_t i{ 1 }; i <= len; ++i) {
        sol::object elem{ arr[i] };
        if (!elem.is<std::string>()) {
          throw_runtime_error({ "inline mode argv must contain only strings" });
        }
        argv.push_back(elem.as<std::string>());
      }
    } else {
      throw_runtime_error({ "'inline' key must be an array of strings" });
    }

    return custom_shell_inline{ std::move(argv) };
  }
}

}  // namespace envy
