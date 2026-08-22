#include "phase_install.h"

#include "cache.h"
#include "engine.h"
#include "lua_ctx/lua_phase_context.h"
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "platform.h"
#include "shell.h"
#include "trace.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace envy {
namespace {

bool directory_has_entries(std::filesystem::path const &dir) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || ec) { return false; }

  std::filesystem::directory_iterator it{ dir, ec };
  if (ec) {
    throw std::runtime_error("Failed to enumerate directory " + dir.string() + ": " +
                             ec.message());
  }

  std::filesystem::directory_iterator end_iter;
  for (; it != end_iter; ++it) { return true; }

  return false;
}

bool run_shell_install(std::string_view script,
                       std::filesystem::path const &install_dir,
                       cache::scoped_entry_lock *lock,
                       std::string const &identity,
                       resolved_shell shell,
                       tui::section_handle tui_section,
                       std::filesystem::path const &cache_root) {
  tui::debug("install: shell script");
  tui_actions::run_phase_shell_script(script,
                                      "Install",
                                      install_dir,
                                      identity,
                                      std::move(shell),
                                      tui_section,
                                      cache_root);

  if (lock) {
    lock->mark_install_complete();
    return true;
  }

  return false;
}

bool run_programmatic_install(sol::protected_function install_func,
                              cache::scoped_entry_lock *lock,
                              std::filesystem::path const &fetch_dir,
                              std::filesystem::path const &stage_dir,
                              std::filesystem::path const &install_dir,
                              std::filesystem::path const &tmp_dir,
                              std::string const &identity,
                              engine &eng,
                              pkg *p) {
  tui::debug("install: install function");

  // Set up Lua registry context for envy.* functions
  phase_context_guard ctx_guard{ &eng, p, install_func.lua_state(), stage_dir };

  sol::state_view lua{ install_func.lua_state() };
  sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

  sol::object result_obj{ call_lua_function_with_enriched_errors(p, "INSTALL", [&]() {
    return install_func(util_path_with_separator(install_dir),
                        util_path_with_separator(stage_dir),
                        util_path_with_separator(fetch_dir),
                        util_path_with_separator(tmp_dir),
                        opts);
  }) };

  // Validate return type: must be nil or string
  sol::type const result_type{ result_obj.get_type() };
  if (result_type != sol::type::none && result_type != sol::type::lua_nil) {
    if (!result_obj.is<std::string>()) {
      throw std::runtime_error("install function for " + identity +
                               " must return nil or string, got " +
                               sol::type_name(lua.lua_state(), result_type));
    }

    // Returned string: spawn fresh shell with manifest defaults
    std::string const returned_script{ result_obj.as<std::string>() };
    return run_shell_install(returned_script,
                             stage_dir,
                             lock,
                             identity,
                             pkg_default_shell(p),
                             p->tui_section,
                             eng.cache_root());
  }

  // Function returned nil/none successfully - mark complete
  if (lock) {
    lock->mark_install_complete();
    return true;
  }

  return false;
}

bool promote_stage_to_install(cache::scoped_entry_lock *lock) {
  auto const install_dir{ lock->install_dir() };
  auto const stage_dir{ lock->stage_dir() };

  if (directory_has_entries(install_dir)) {
    tui::debug("install: install dir already populated — marking complete");
    lock->mark_install_complete();
    return true;
  }

  if (directory_has_entries(stage_dir)) {
    tui::debug("install: promoting staged files");
    std::filesystem::remove_all(install_dir);
    std::filesystem::create_directories(install_dir.parent_path());
    std::filesystem::rename(stage_dir, install_dir);
    lock->mark_install_complete();
    return true;
  }

  return false;
}

}  // namespace

void run_install_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::pkg_install,
                                       std::chrono::steady_clock::now() };

  if (!p->lock) { return; }  // cache hit — no work to do

  cache::scoped_entry_lock::ptr_t lock{ std::move(p->lock) };
  std::filesystem::path const final_pkg_path{ lock->install_dir() };

  platform::await_files_accessible(lock->fetch_dir());

  auto const lua_acc{ p->lua.lock() };
  sol::state_view lua_view{ *lua_acc };
  sol::object install_obj{ lua_view["INSTALL"] };
  bool marked_complete{ false };

  if (!install_obj.valid()) {
    marked_complete = promote_stage_to_install(lock.get());
  } else if (install_obj.is<std::string>()) {
    std::string script{ install_obj.as<std::string>() };
    marked_complete = run_shell_install(script,
                                        lock->stage_dir(),
                                        lock.get(),
                                        p->cfg->identity,
                                        pkg_default_shell(p),
                                        p->tui_section,
                                        eng.cache_root());
  } else if (install_obj.is<sol::protected_function>()) {
    marked_complete = run_programmatic_install(install_obj.as<sol::protected_function>(),
                                               lock.get(),
                                               lock->fetch_dir(),
                                               lock->stage_dir(),
                                               lock->install_dir(),
                                               lock->tmp_dir(),
                                               p->cfg->identity,
                                               eng,
                                               p);
  } else {
    throw std::runtime_error("INSTALL field must be nil, string, or function for " +
                             p->cfg->identity);
  }

  if (marked_complete && lock) {
    sol::object exportable_obj{ lua_view["EXPORTABLE"] };
    bool const exportable{ exportable_obj.valid() &&
                           exportable_obj.get_type() == sol::type::boolean &&
                           exportable_obj.as<bool>() };
    if (!exportable) { lock->mark_preserve_fetch(); }
  }

  if (marked_complete) { p->pkg_path = final_pkg_path; }
}

}  // namespace envy
