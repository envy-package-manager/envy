#include "phase_stage.h"

#include "cache.h"
#include "engine.h"
#include "extract.h"
#include "lua_ctx/lua_phase_context.h"
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "pkg.h"
#include "platform.h"
#include "shell.h"
#include "sol_util.h"
#include "trace.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace envy {
namespace {

bool fetch_dir_has_files(std::filesystem::path const &fetch_dir) {
  if (!std::filesystem::exists(fetch_dir)) { return false; }
  for (auto const &entry : std::filesystem::directory_iterator(fetch_dir)) {
    if (!entry.is_regular_file()) { continue; }
    if (entry.path().filename() == "envy-complete") { continue; }
    return true;
  }
  return false;
}

std::filesystem::path determine_stage_destination(sol::state_view lua,
                                                  cache::scoped_entry_lock const *lock) {
  sol::object stage_obj{ lua["STAGE"] };
  sol::object build_obj{ lua["BUILD"] };
  sol::object install_obj{ lua["INSTALL"] };

  bool const has_custom_phases{ stage_obj.is<sol::protected_function>() ||
                                build_obj.is<sol::protected_function>() ||
                                install_obj.is<sol::protected_function>() };

  std::filesystem::path const dest_dir{ has_custom_phases ? lock->stage_dir()
                                                          : lock->install_dir() };

  tui::debug(has_custom_phases ? "stage: extracting to stage dir (custom install)"
                               : "stage: extracting to install dir");

  return dest_dir;
}

extract_options parse_stage_options(sol::table const &stage_tbl, std::string const &key) {
  extract_options opts;

  if (auto strip{ sol_util_get_optional<int>(stage_tbl, "strip", key) }) {
    if (*strip < 0) {
      throw std::runtime_error("stage.strip must be non-negative for " + key);
    }
    opts.strip_components = *strip;
  }

  opts.selectors = sol_util_get_string_list(stage_tbl, "only", key);
  if (opts.selectors.empty() && stage_tbl["only"].valid()) {
    throw std::runtime_error("stage.only must list at least one path for " + key);
  }

  return opts;
}

void run_programmatic_stage(sol::protected_function stage_func,
                            std::filesystem::path const &fetch_dir,
                            std::filesystem::path const &stage_dir,
                            std::filesystem::path const &tmp_dir,
                            std::string const &identity,
                            engine &eng,
                            pkg *p) {
  tui::debug("stage: stage function");

  // Set up Lua registry context for envy.* functions (run_dir = stage_dir)
  phase_context_guard ctx_guard{ &eng, p, stage_func.lua_state(), stage_dir };

  sol::state_view lua{ stage_func.lua_state() };
  sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

  call_lua_function_with_enriched_errors(p, "STAGE", [&]() {
    return stage_func(util_path_with_separator(fetch_dir),
                      util_path_with_separator(stage_dir),
                      util_path_with_separator(tmp_dir),
                      opts);
  });
}

void run_shell_stage(std::string_view script,
                     std::filesystem::path const &dest_dir,
                     std::string const &identity,
                     resolved_shell shell,
                     tui::section_handle tui_section,
                     std::filesystem::path const &cache_root) {
  tui::debug("stage: shell script");
  tui_actions::run_phase_shell_script(script,
                                      "Stage",
                                      dest_dir,
                                      identity,
                                      std::move(shell),
                                      tui_section,
                                      cache_root);
}

}  // namespace

void run_stage_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::pkg_stage,
                                       std::chrono::steady_clock::now() };

  cache::scoped_entry_lock *lock{ p->lock.get() };
  if (!lock) { return; }  // cache hit

  std::string const &identity{ p->cfg->identity };
  auto const lua_acc{ p->lua.lock() };
  sol::state_view lua_view{ *lua_acc };
  std::filesystem::path const stage_dir{ determine_stage_destination(lua_view, lock) };

  sol::object stage_obj{ lua_view["STAGE"] };

  if (!fetch_dir_has_files(lock->fetch_dir())) { return; }

  platform::await_files_accessible(lock->fetch_dir());

  if (!stage_obj.valid()) {
    extract_all_archives(lock->fetch_dir(), stage_dir, {}, identity, p->tui_section);
  } else if (stage_obj.is<std::string>()) {
    auto const script_str{ stage_obj.as<std::string>() };
    run_shell_stage(script_str,
                    stage_dir,
                    identity,
                    pkg_default_shell(p),
                    p->tui_section,
                    eng.cache_root());
  } else if (stage_obj.is<sol::protected_function>()) {
    run_programmatic_stage(stage_obj.as<sol::protected_function>(),
                           lock->fetch_dir(),
                           stage_dir,
                           lock->tmp_dir(),
                           identity,
                           eng,
                           p);
  } else if (stage_obj.is<sol::table>()) {
    extract_all_archives(lock->fetch_dir(),
                         stage_dir,
                         parse_stage_options(stage_obj.as<sol::table>(), identity),
                         identity,
                         p->tui_section);
  } else {
    throw std::runtime_error("STAGE field must be nil, string, table, or function for " +
                             identity);
  }
}

}  // namespace envy
