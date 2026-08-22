#include "lua_envy_run.h"

#include "engine.h"
#include "lua_phase_context.h"
#include "lua_shell.h"
#include "pkg.h"
#include "shell.h"
#include "sol_util.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace envy {
namespace {

std::string format_run_error(std::string_view script,
                             int exit_code,
                             std::optional<int> signal,
                             std::string const &stdout_str,
                             std::string const &stderr_str) {
  std::ostringstream oss;

  if (signal) {
    oss << "envy.run: shell script terminated by signal " << *signal;
  } else {
    oss << "envy.run: command failed with exit code " << exit_code;
  }

  oss << "\nCommand: " << script << '\n';

  if (!stdout_str.empty()) {
    oss << "\n--- stdout ---\n" << stdout_str;
    if (!stdout_str.ends_with('\n')) { oss << '\n'; }
  }

  if (!stderr_str.empty()) {
    oss << "\n--- stderr ---\n" << stderr_str;
    if (!stderr_str.ends_with('\n')) { oss << '\n'; }
  }

  return oss.str();
}

// Join array of strings into single script with newlines
std::string join_script_array(sol::table const &tbl) {
  std::ostringstream oss;
  bool first{ true };
  for (auto const &[key, value] : tbl) {
    if (!value.is<std::string>()) {
      throw std::runtime_error("envy.run: script array elements must be strings");
    }
    if (!first) { oss << '\n'; }
    oss << value.as<std::string>();
    first = false;
  }
  return oss.str();
}

}  // namespace

void lua_envy_run_install(sol::table &envy_table) {
  envy_table["run"] = [](sol::object script_obj,
                         sol::optional<sol::object> opts_obj,
                         sol::this_state L) -> sol::table {
    // Parse script argument - string or array of strings
    std::string script;
    if (script_obj.is<std::string>()) {
      script = script_obj.as<std::string>();
    } else if (script_obj.is<sol::table>()) {
      script = join_script_array(script_obj.as<sol::table>());
    } else {
      throw std::runtime_error(
          "envy.run: first argument must be a string or array of strings");
    }
    std::string_view const script_view{ script };

    // Validate options argument if provided
    if (opts_obj && opts_obj->valid() && opts_obj->get_type() != sol::type::lua_nil &&
        !opts_obj->is<sol::table>()) {
      throw std::runtime_error("envy.run: second argument must be a table (options)");
    }

    sol::optional<sol::table> opts_table;
    if (opts_obj && opts_obj->is<sol::table>()) {
      opts_table = opts_obj->as<sol::table>();
    }

    // Get phase context for default shell, run_dir, engine
    phase_context const *ctx{ lua_phase_context_get(L) };
    pkg *p{ ctx ? ctx->p : nullptr };
    resolved_shell shell{ pkg_default_shell(p) };

    std::optional<std::filesystem::path> cwd;
    shell_env_t env{ shell_getenv() };

    if (opts_table) {
      sol::table opts{ *opts_table };

      sol::optional<std::string> cwd_str = opts["cwd"];
      if (cwd_str) { cwd = std::filesystem::path{ *cwd_str }; }

      sol::optional<sol::table> env_table = opts["env"];
      if (env_table) {
        for (auto const &[key, value] : *env_table) {
          if (key.is<std::string>() && value.is<std::string>()) {
            env[key.as<std::string>()] = value.as<std::string>();
          }
        }
      }

      sol::optional<sol::object> shell_obj = opts["shell"];
      if (shell_obj && shell_obj->valid()) {
        shell = parse_shell_config_from_lua(*shell_obj, "envy.run");
      }
    }

    // Resolve cwd: use phase's run_dir (stored in registry), fall back to stage_dir
    std::filesystem::path const base_dir{
      ctx && ctx->run_dir
          ? *ctx->run_dir
          : (p && p->lock ? p->lock->stage_dir() : std::filesystem::current_path())
    };

    if (!cwd) {
      cwd = base_dir;
    } else if (!cwd->is_absolute()) {
      cwd = base_dir / *cwd;  // Resolve relative to base_dir
    }

    bool quiet{ false };
    bool capture{ false };
    bool check{ true };
    bool interactive{ false };
    if (opts_table) {
      sol::table opts{ *opts_table };
      quiet = sol_util_get_or_default<bool>(opts, "quiet", false, "envy.run");
      capture = sol_util_get_or_default<bool>(opts, "capture", false, "envy.run");
      check = sol_util_get_or_default<bool>(opts, "check", true, "envy.run");
      interactive = sol_util_get_or_default<bool>(opts, "interactive", false, "envy.run");
    }

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;

    shell_run_cfg cfg{
      .on_stdout_line = [&](std::string_view line) { stdout_stream << line << '\n'; },
      .on_stderr_line = [&](std::string_view line) { stderr_stream << line << '\n'; },
      .cwd = cwd,
      .env = std::move(env),
      .shell = shell,
      .check = check
    };

    std::optional<tui::interactive_mode_guard> guard;
    if (interactive) {
      guard.emplace();
      if (!quiet && p) {
        std::string const flat{ util_flatten_script_with_semicolons(script_view) };
        std::fprintf(stderr, "[%s] %s\n", p->cfg->identity.c_str(), flat.c_str());
        std::fflush(stderr);
      }
      if (!quiet) {
        cfg.on_output_line = [&](std::string_view line) {
          std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
          std::fflush(stderr);
        };
      }
    }

    engine *eng{ ctx ? ctx->eng : nullptr };
    bool const use_progress{ p && p->tui_section && eng && !quiet && !interactive };

    shell_result const result{ use_progress ? tui_actions::run_shell_with_progress(
                                                  script_view,
                                                  p->tui_section,
                                                  p->cfg->identity,
                                                  eng->cache_root(),
                                                  std::move(cfg))
                                            : shell_run(script_view, std::move(cfg)) };

    std::string const stdout_str{ stdout_stream.str() };
    std::string const stderr_str{ stderr_stream.str() };

    if (result.signal) {
      auto const err{ format_run_error(script_view,
                                       result.exit_code,
                                       result.signal,
                                       stdout_str,
                                       stderr_str) };
      tui::error("%s", err.c_str());
      throw std::runtime_error(err);
    }

    if (check && result.exit_code != 0) {
      auto const err{ format_run_error(script_view,
                                       result.exit_code,
                                       std::nullopt,
                                       stdout_str,
                                       stderr_str) };
      tui::error("%s", err.c_str());
      throw std::runtime_error(err);
    }

    sol::state_view lua_view{ L };
    sol::table return_table{ lua_view.create_table() };
    return_table["exit_code"] = result.exit_code;
    if (capture) {
      return_table["stdout"] = stdout_str;
      return_table["stderr"] = stderr_str;
    } else {
      return_table["stdout"] = sol::lua_nil;
      return_table["stderr"] = sol::lua_nil;
    }

    return return_table;
  };
}

}  // namespace envy
