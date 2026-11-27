#include "phase_stage.h"

#include "cache.h"
#include "engine.h"
#include "extract.h"
#include "lua_ctx/lua_envy_extract.h"
#include "lua_ctx/lua_phase_context.h"
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "pkg.h"
#include "pkg_env.h"
#include "product_expand.h"
#include "shell.h"
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

// Straight into pkg/ only when nothing downstream works on the tree: BUILD and INSTALL
// run in work/stage/ in every form, strings included, so either one needs it there.
std::filesystem::path determine_stage_destination(sol::state_view lua,
                                                  cache::scoped_entry_lock const *lock) {
  bool const has_custom_phases{ lua["STAGE"].is<sol::protected_function>() ||
                                sol::object{ lua["BUILD"] }.valid() ||
                                sol::object{ lua["INSTALL"] }.valid() };

  tui::debug(has_custom_phases ? "stage: extracting to stage dir (custom install)"
                               : "stage: extracting to install dir");

  return has_custom_phases ? lock->stage_dir() : lock->install_dir();
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
  phase_context_guard ctx_guard{ &eng,      p,       stage_func.lua_state(),
                                 stage_dir, nullptr, tmp_dir };

  sol::state_view lua{ stage_func.lua_state() };
  sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

  call_lua_function_with_enriched_errors(p, "STAGE", [&]() {
    return stage_func(util_path_with_separator(fetch_dir),
                      util_path_with_separator(stage_dir),
                      util_path_with_separator(tmp_dir),
                      opts);
  });
}

// `L`: the package's Lua state, locked by the caller.
void run_shell_stage(std::string_view script,
                     std::filesystem::path const &dest_dir,
                     pkg *p,
                     std::filesystem::path const &cache_root,
                     lua_State *L) {
  tui::debug("stage: shell script");
  product_run_phase_script(p,
                           pkg_phase::pkg_stage,
                           "STAGE",
                           "Stage",
                           script,
                           dest_dir,
                           p->lock->tmp_dir(),
                           L,
                           p->tui_section,
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
  // Every file in fetch/ is released before STAGE reads it, whichever path put it there
  // (fetch, envy.commit_fetch, a depot import, an earlier run). This runs before the Lua
  // lock is taken, so the package's Lua state stays free during a wait.
  tui_actions::await_release_tracked(lock->fetch_dir(), p->tui_section, identity);

  auto const lua_acc{ p->lua.lock() };
  sol::state_view lua_view{ *lua_acc };
  std::filesystem::path const stage_dir{ determine_stage_destination(lua_view, lock) };

  sol::object stage_obj{ lua_view["STAGE"] };

  if (!fetch_dir_has_files(lock->fetch_dir())) { return; }

  if (!stage_obj.valid()) {
    extract_all_archives(lock->fetch_dir(), stage_dir, {}, identity, p->tui_section);
  } else if (stage_obj.is<std::string>()) {
    auto const script_str{ stage_obj.as<std::string>() };
    run_shell_stage(script_str, stage_dir, p, eng.cache_root(), lua_view.lua_state());
  } else if (stage_obj.is<sol::protected_function>()) {
    run_programmatic_stage(stage_obj.as<sol::protected_function>(),
                           lock->fetch_dir(),
                           stage_dir,
                           lock->tmp_dir(),
                           identity,
                           eng,
                           p);
  } else if (stage_obj.is<sol::table>()) {
    extract_all_archives(
        lock->fetch_dir(),
        stage_dir,
        lua_envy_extract_parse_opts(stage_obj.as<sol::table>(), "STAGE of " + identity),
        identity,
        p->tui_section);
  } else {
    throw_runtime_error(
        { "STAGE field must be nil, string, table, or function for ", identity });
  }
}

}  // namespace envy
