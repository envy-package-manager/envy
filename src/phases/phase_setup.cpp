#include "phase_setup.h"

#include "blake3_util.h"
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

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace envy {

namespace {

// pkg_dir argument for pair verbs: the installed payload path for cache-managed
// packages, nil for user-managed (no payload exists).
sol::object make_pkg_dir_arg(sol::state_view lua, pkg *p) {
  return (p->type == pkg_type::CACHE_MANAGED && !p->pkg_path.empty())
             ? sol::object{ lua, sol::in_place, util_path_with_separator(p->pkg_path) }
             : sol::object{ sol::lua_nil };
}

// Run a pair CHECK shell command; exit 0 = satisfied. Non-zero is normal flow
// (work needed), not an error.
bool run_pair_check_command(pkg *p, std::string_view cmd, std::string const &context) {
  shell_run_cfg cfg;
  cfg.env = shell_getenv();
  cfg.on_stdout_line = [](std::string_view) {};
  cfg.on_stderr_line = [](std::string_view) {};
  cfg.cwd = pkg_cfg::compute_project_root(p->cfg);
  cfg.shell = pkg_default_shell(p);

  shell_result const result{ [&] {
    try {
      return shell_run(cmd, cfg);
    } catch (std::exception const &e) {
      throw std::runtime_error(context + " command failed for " + p->cfg->identity + ": " +
                               e.what());
    }
  }() };

  tui::debug("setup: %s check exit=%d (%s)",
             context.c_str(),
             result.exit_code,
             result.exit_code == 0 ? "satisfied" : "not satisfied");
  return result.exit_code == 0;
}

}  // namespace

// Run a pair's CHECK verb. Returns true when the host state is already satisfied.
// Function form: CHECK(pkg_dir, options) -> boolean | string (string runs as shell).
// The Lua lock is released before any shell command runs so concurrent pair nodes
// sharing this package's state serialize only on Lua execution, not shell time.
// Not in anonymous namespace so tests can call it.
bool run_pair_check(pkg *p, engine &eng, std::string const &name) {
  std::string const context{ "SETUP." + name + ".CHECK" };

  std::optional<bool> verdict;
  std::optional<std::string> cmd;
  {
    auto const lua_acc{ p->lua.lock() };
    sol::state_view lua{ *lua_acc };
    sol::object verb{ lua["SETUP"][name]["CHECK"] };

    if (verb.is<std::string>()) {
      cmd = verb.as<std::string>();
    } else if (!verb.is<sol::protected_function>()) {
      throw std::runtime_error(context + " must be a function or string for " +
                               p->cfg->identity);
    } else {
      std::filesystem::path const project_root{ pkg_cfg::compute_project_root(p->cfg) };
      phase_context_guard ctx_guard{ &eng, p, lua.lua_state(), project_root };
      sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

      sol::object result_obj{ call_lua_function_with_enriched_errors(p, context, [&]() {
        return verb.as<sol::protected_function>()(make_pkg_dir_arg(lua, p), opts);
      }) };

      if (result_obj.is<bool>()) {
        verdict = result_obj.as<bool>();
      } else if (result_obj.is<std::string>()) {
        cmd = result_obj.as<std::string>();
      } else {
        throw std::runtime_error(context + " for " + p->cfg->identity +
                                 " must return boolean or string, got " +
                                 sol::type_name(lua.lua_state(), result_obj.get_type()));
      }
    }
  }

  return verdict ? *verdict : run_pair_check_command(p, *cmd, context);
}

// Run a pair's INSTALL verb against the host (cwd = project_root). Shell output
// lands in `section` (the pair node's TUI section), labeled with `log_identity`.
// Function form: INSTALL(pkg_dir, options) -> nil | string (string runs as shell).
// The Lua lock is released before any shell script runs (see run_pair_check).
// Not in anonymous namespace so tests can call it.
void run_pair_install(pkg *p,
                      engine &eng,
                      std::string const &name,
                      tui::section_handle section,
                      std::string const &log_identity) {
  std::string const context{ "SETUP." + name + ".INSTALL" };
  std::filesystem::path const project_root{ pkg_cfg::compute_project_root(p->cfg) };

  std::optional<std::string> script;
  {
    auto const lua_acc{ p->lua.lock() };
    sol::state_view lua{ *lua_acc };
    sol::object verb{ lua["SETUP"][name]["INSTALL"] };

    if (verb.is<std::string>()) {
      script = verb.as<std::string>();
    } else if (!verb.is<sol::protected_function>()) {
      throw std::runtime_error(context + " must be a function or string for " +
                               p->cfg->identity);
    } else {
      phase_context_guard ctx_guard{ &eng, p, lua.lua_state(), project_root };
      sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

      sol::object result_obj{ call_lua_function_with_enriched_errors(p, context, [&]() {
        return verb.as<sol::protected_function>()(make_pkg_dir_arg(lua, p), opts);
      }) };

      sol::type const result_type{ result_obj.get_type() };
      if (result_type != sol::type::none && result_type != sol::type::lua_nil) {
        if (!result_obj.is<std::string>()) {
          throw std::runtime_error(context + " for " + p->cfg->identity +
                                   " must return nil or string, got " +
                                   sol::type_name(lua.lua_state(), result_type));
        }
        script = result_obj.as<std::string>();
      }
    }
  }

  if (script) {
    tui_actions::run_phase_shell_script(*script,
                                        "Setup",
                                        project_root,
                                        log_identity,
                                        pkg_default_shell(p),
                                        section,
                                        eng.cache_root());
  }
}

// Effective selection: union of explicit names from all referrers, closed
// transitively over DEPENDS (selecting a pair implies its prerequisites).
// No defaults — an empty selection runs nothing, any package type. Sorted for
// deterministic node creation; unknown explicit names are hard errors.
// Marks the selection consumed: a later merge that adds names is an error.
// Not in anonymous namespace so tests can call it.
std::vector<std::string> compute_selected_pairs(pkg *p) {
  std::unordered_set<std::string> selected;
  {
    std::lock_guard const deps_lock(p->deps_mutex);
    selected = p->setup_selected;
    p->setup_selection_consumed = true;
  }

  for (auto const &name : selected) {
    if (!p->setup_pairs.contains(name)) {
      throw std::runtime_error(
          "Unknown setup pair '" + name + "' selected for " + p->cfg->identity +
          " (spec defines " +
          (p->setup_pairs.empty() ? "no SETUP pairs" : "different SETUP pair names") +
          ")");
    }
  }

  // Transitive DEPENDS closure — parse validation guarantees targets exist
  std::vector<std::string> work{ selected.begin(), selected.end() };
  while (!work.empty()) {
    std::string const name{ std::move(work.back()) };
    work.pop_back();
    for (auto const &dep : p->setup_pairs.at(name).depends) {
      if (selected.insert(dep).second) { work.push_back(dep); }
    }
  }

  std::vector<std::string> sorted{ selected.begin(), selected.end() };
  std::ranges::sort(sorted);
  return sorted;
}

// One pair: double-check lock pattern shared with legacy user-managed packages.
// The lock entry is ephemeral (mark_user_managed) — purged on release, never
// marked complete; the CHECK verb is the only re-run gate. `p` is the declaring
// (parent) package; `section`/`log_identity` come from the pair task so shell
// output is attributed per-pair.
void run_setup_pair(pkg *p,
                    engine &eng,
                    std::string const &name,
                    tui::section_handle section,
                    std::string const &log_identity) {
  auto const &pair_platforms{ p->setup_pairs.at(name).platforms };
  if (!pair_platforms.empty() &&
      !util_platform_matches(pair_platforms, platform::os_name(), platform::arch_name())) {
    tui::debug("setup: pair '%s' skipped (platform mismatch)", name.c_str());
    return;
  }

  if (run_pair_check(p, eng, name)) {
    tui::debug("setup: pair '%s' satisfied (pre-lock)", name.c_str());
    return;
  }

  // Serialize concurrent envy processes on this (package, pair). The Lua lock is
  // never held across this acquisition (lock order: cache lock, then p->lua).
  std::string const lock_key{ p->cfg->format_key() + "|setup:" + name };
  auto const digest{ blake3_hash(lock_key.data(), lock_key.size()) };
  std::string const hash_prefix{ util_bytes_to_hex(digest.data(), 8) };

  auto cache_result{ p->cache_ptr->ensure_pkg(
      p->cfg->identity,
      platform::os_name(),
      platform::arch_name(),
      hash_prefix,
      tui_actions::lock_wait_spinner(section, log_identity, "setup:" + name)) };
  if (!cache_result.lock) {
    // Pair entries are always purged on release; a completed entry means a
    // stale/corrupt cache. Mirror legacy user-managed behavior: warn and skip.
    tui::warn("setup: unexpected completed cache entry for pair '%s' at %s",
              name.c_str(),
              cache_result.pkg_path.string().c_str());
    return;
  }
  cache_result.lock->mark_user_managed();

  if (run_pair_check(p, eng, name)) {  // Race: another process completed the work
    tui::debug("setup: pair '%s' satisfied (post-lock)", name.c_str());
    return;  // Lock destructor purges the ephemeral entry
  }

  tui::debug("setup: running pair '%s' install (check failed)", name.c_str());
  run_pair_install(p, eng, name, section, log_identity);
}

void run_setup_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::pkg_setup,
                                       std::chrono::steady_clock::now() };

  if (p->type != pkg_type::CACHE_MANAGED && p->type != pkg_type::USER_MANAGED) { return; }

  auto const selected{ compute_selected_pairs(p) };
  if (selected.empty()) { return; }  // Explicit-only: no selection, nothing to do

  // Selected pairs become single-step engine tasks: unrelated pairs run in
  // parallel, DEPENDS sequences siblings via edges. The engine waits for every
  // pair and aggregates failures so one bad pair doesn't mask the others.
  eng.run_setup_pairs_for(p, selected);
}

}  // namespace envy
