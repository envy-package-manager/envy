#include "phase_install.h"

#include "cache.h"
#include "engine.h"
#include "lua_ctx/lua_phase_context.h"
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_env.h"
#include "product_expand.h"
#include "shell.h"
#include "trace.h"
#include "tree_hash.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"
#include "vendor.h"

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
    throw_runtime_error({ "Failed to enumerate directory ", dir, ": ", ec.message() });
  }

  std::filesystem::directory_iterator end_iter;
  for (; it != end_iter; ++it) { return true; }

  return false;
}

// `L`: the package's Lua state, locked by the caller.
void run_shell_install(std::string_view script,
                       std::filesystem::path const &cwd,
                       std::filesystem::path const &tmp_dir,
                       pkg *p,
                       std::filesystem::path const &cache_root,
                       lua_State *L) {
  tui::debug("install: shell script");
  product_run_phase_script(p,
                           pkg_phase::pkg_install,
                           "INSTALL",
                           "Install",
                           script,
                           cwd,
                           tmp_dir,
                           L,
                           p->tui_section,
                           cache_root);
}

void run_programmatic_install(sol::protected_function install_func,
                              std::filesystem::path const &fetch_dir,
                              std::filesystem::path const &stage_dir,
                              std::filesystem::path const &install_dir,
                              std::filesystem::path const &tmp_dir,
                              std::string const &identity,
                              engine &eng,
                              pkg *p) {
  tui::debug("install: install function");

  // Set up Lua registry context for envy.* functions
  phase_context_guard ctx_guard{ &eng,      p,       install_func.lua_state(),
                                 stage_dir, nullptr, tmp_dir };

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
      throw_runtime_error({ "install function for ",
                            identity,
                            " must return nil or string, got ",
                            sol::type_name(lua.lua_state(), result_type) });
    }

    // Returned string: spawn fresh shell with manifest defaults
    run_shell_install(result_obj.as<std::string>(),
                      stage_dir,
                      tmp_dir,
                      p,
                      eng.cache_root(),
                      lua.lua_state());
  }
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
    // A leftover here would be renamed over, so the promotion stops if it is still
    // there; everything else about the delete is the shared path's business.
    if (auto const ec{ tree_remove_insisting(install_dir) }) {
      throw std::filesystem::filesystem_error("install: cannot clear", install_dir, ec);
    }
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

  // INSTALL may read fetch/ directly, as STAGE does. The check is repeated here because a
  // process can open the files again while STAGE runs.
  tui_actions::await_release_tracked(lock->fetch_dir(), p->tui_section, p->cfg->identity);

  auto const lua_acc{ p->lua.lock() };
  sol::state_view lua_view{ *lua_acc };
  sol::object install_obj{ lua_view["INSTALL"] };

  bool const marked_complete{ [&] {
    if (!install_obj.valid()) { return promote_stage_to_install(lock.get()); }
    if (install_obj.is<std::string>()) {
      run_shell_install(install_obj.as<std::string>(),
                        lock->stage_dir(),
                        lock->tmp_dir(),
                        p,
                        eng.cache_root(),
                        lua_view.lua_state());
      promote_stage_to_install(lock.get());  // a script that left pkg/ empty (`chmod +x`)
    } else if (install_obj.is<sol::protected_function>()) {
      run_programmatic_install(install_obj.as<sol::protected_function>(),
                               lock->fetch_dir(),
                               lock->stage_dir(),
                               lock->install_dir(),
                               lock->tmp_dir(),
                               p->cfg->identity,
                               eng,
                               p);
    } else {
      throw_runtime_error(
          { "INSTALL field must be nil, string, or function for ", p->cfg->identity });
    }
    lock->mark_install_complete();
    return true;
  }() };
  if (!marked_complete) { return; }

  sol::object exportable_obj{ lua_view["EXPORTABLE"] };
  bool const exportable{ exportable_obj.valid() &&
                         exportable_obj.get_type() == sol::type::boolean &&
                         exportable_obj.as<bool>() };
  if (!exportable) { lock->mark_preserve_fetch(); }

  p->pkg_path = final_pkg_path;

  // Stamp while the entry is still ours and the payload untouched, but only for what
  // this run vendors. The vendor phase backfills a cache hit or a depot import.
  if (auto const *plan{ eng.vendor() }; plan && plan->find(p->key)) {
    vendor_pristine_hash(p->pkg_path, p->vendor_filter);
  }
}

}  // namespace envy
