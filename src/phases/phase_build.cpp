#include "phase_build.h"

#include "cache.h"
#include "engine.h"
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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace envy {
namespace {

// `L`: the package's Lua state, locked by the caller.
void execute_build_script(std::string_view script,
                          std::filesystem::path const &cwd,
                          pkg *p,
                          std::filesystem::path const &cache_root,
                          lua_State *L) {
  product_run_phase_script(p,
                           pkg_phase::pkg_build,
                           "BUILD",
                           "Build",
                           script,
                           cwd,
                           p->lock->tmp_dir(),
                           L,
                           p->tui_section,
                           cache_root);
}

void run_programmatic_build(sol::protected_function build_func,
                            std::filesystem::path const &install_dir,
                            std::filesystem::path const &fetch_dir,
                            std::filesystem::path const &stage_dir,
                            std::filesystem::path const &tmp_dir,
                            engine &eng,
                            pkg *p) {
  tui::debug("build: build function");

  // Set up Lua registry context for envy.* functions (run_dir = stage_dir)
  phase_context_guard ctx_guard{ &eng,      p,       build_func.lua_state(),
                                 stage_dir, nullptr, tmp_dir };

  sol::state_view lua{ build_func.lua_state() };
  sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

  sol::protected_function_result build_result{ call_lua_function_with_enriched_errors(
      p,
      "BUILD",
      [&]() {
        return build_func(util_path_with_separator(install_dir),
                          util_path_with_separator(stage_dir),
                          util_path_with_separator(fetch_dir),
                          util_path_with_separator(tmp_dir),
                          opts);
      }) };

  // If function returned a string, execute it via shell
  if (build_result.return_count() > 0) {
    sol::object return_value = build_result;
    if (return_value.is<std::string>()) {
      std::string const script{ return_value.as<std::string>() };
      execute_build_script(script, stage_dir, p, eng.cache_root(), lua.lua_state());
    }
  }
}

void run_shell_build(std::string_view script,
                     std::filesystem::path const &stage_dir,
                     pkg *p,
                     std::filesystem::path const &cache_root,
                     lua_State *L) {
  tui::debug("build: shell script");
  execute_build_script(script, stage_dir, p, cache_root, L);
}

}  // namespace

void run_build_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::pkg_build,
                                       std::chrono::steady_clock::now() };
  if (!p->lock) { return; }  // cache hit

  auto const lua_acc{ p->lua.lock() };
  sol::state_view lua_view{ *lua_acc };
  sol::object build_obj{ lua_view["BUILD"] };

  if (!build_obj.valid()) {
    // no BUILD field
  } else if (build_obj.is<std::string>()) {
    std::string const script{ build_obj.as<std::string>() };
    run_shell_build(script,
                    p->lock->stage_dir(),
                    p,
                    eng.cache_root(),
                    lua_view.lua_state());
  } else if (build_obj.is<sol::protected_function>()) {
    run_programmatic_build(build_obj.as<sol::protected_function>(),
                           p->lock->install_dir(),
                           p->lock->fetch_dir(),
                           p->lock->stage_dir(),
                           p->lock->tmp_dir(),
                           eng,
                           p);
  } else {
    throw_runtime_error(
        { "BUILD field must be nil, string, or function for ", p->cfg->identity });
  }
}

}  // namespace envy
