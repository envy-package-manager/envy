#include "shell.h"
#include "platform.h"
#include "util.h"

#include "sol/sol.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <variant>
#include <vector>

namespace envy {

std::optional<std::filesystem::path> shell_find_executable(
    std::string_view name,
    std::string_view path_list,
    std::filesystem::path const *skip_dir) {
#ifdef _WIN32
  constexpr char kSep{ ';' };
  std::vector<std::string> const names{ [&] {
    auto const lower{ [](std::string_view s) {
      std::string out{ s };
      for (char &c : out) { c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
      return out;
    } };
    char const *env{ std::getenv("PATHEXT") };
    std::string const have{ lower(std::filesystem::path{ name }.extension().string()) };
    std::vector<std::string> out;
    std::string_view rest{ env && *env ? env : ".COM;.EXE;.BAT;.CMD" };
    while (!rest.empty()) {
      size_t const semi{ rest.find(';') };
      std::string const ext{ lower(rest.substr(0, semi)) };
      rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
      if (ext.empty()) { continue; }
      if (ext == have) { out.insert(out.begin(), std::string{ name }); }  // `tool.bat`
      out.push_back(std::string{ name } + ext);
    }
    return out;
  }() };
#else
  constexpr char kSep{ ':' };
  std::vector<std::string> const names{ std::string{ name } };
#endif

#ifdef _WIN32
  // A Store alias (python.exe in WindowsApps) is an empty reparse point: present and not a
  // directory is all CreateProcess needs. Entries decode as UTF-8, as the env reads.
  auto const runnable{ [](std::filesystem::path const &p) {
    std::error_code ec;
    auto const st{ std::filesystem::symlink_status(p, ec) };
    return !ec && std::filesystem::exists(st) && !std::filesystem::is_directory(st);
  } };
  auto const dir_of{ [](std::string_view e) {
    return std::filesystem::path{
      std::u8string_view{ reinterpret_cast<char8_t const *>(e.data()), e.size() }
    };
  } };
#else
  auto const runnable{ [](std::filesystem::path const &p) {
    return platform::can_execute_file(p);
  } };
  auto const dir_of{ [](std::string_view e) { return std::filesystem::path{ e }; } };
#endif

  for (std::string_view rest{ path_list }; !rest.empty();) {
    size_t const sep{ rest.find(kSep) };
    std::string_view entry{ rest.substr(0, sep) };
    rest = sep == std::string_view::npos ? std::string_view{} : rest.substr(sep + 1);
#ifdef _WIN32
    if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
      entry = entry.substr(1, entry.size() - 2);
    }
#endif
    if (entry.empty()) { continue; }  // the cwd: a hijack nobody opted into
    std::filesystem::path const dir{ dir_of(entry) };
    if (std::error_code ec; skip_dir && std::filesystem::equivalent(dir, *skip_dir, ec)) {
      continue;
    }
    for (auto const &n : names) {
      if (auto candidate{ dir / n }; runnable(candidate)) { return candidate; }
    }
  }
  return std::nullopt;
}

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
