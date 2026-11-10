#include "pkg_env.h"

#include "cache.h"
#include "engine.h"
#include "lua_ctx/lua_phase_context.h"
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "phases/phase_check.h"
#include "pkg.h"
#include "platform.h"
#include "tui.h"
#include "util.h"

#include "sol/sol.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace envy {

std::filesystem::path pkg_predicted_path(pkg *p) {
  return p->cache_ptr->compute_pkg_path(p->cfg->identity,
                                        p->display,
                                        platform::os_name(),
                                        platform::arch_name(),
                                        pkg_cache_hash(*p).substr(0, 16));
}

std::optional<env_ops> pkg_env_eval(pkg *p,
                                    std::optional<std::filesystem::path> const &pkg_path,
                                    std::filesystem::path const &root) {
  if (!p->has_env) { return std::nullopt; }

  auto const lua_acc{ p->lua.lock() };
  if (!lua_acc) { return std::nullopt; }
  sol::state_view lua{ *lua_acc };
  sol::object const env_obj{ lua["ENV"] };
  std::string const context{ "spec '" + p->cfg->identity + "'" };
  if (env_obj.get_type() != sol::type::function) {
    return env_ops_parse(env_obj, context);
  }

  sol::object const path_arg{
    pkg_path ? sol::make_object(lua, util_path_with_separator(*pkg_path))
             : sol::make_object(lua, sol::lua_nil)
  };
  sol::object const opts{ lua.registry()[ENVY_OPTIONS_RIDX] };
  sol::object const result{ call_lua_function_with_enriched_errors(p, "ENV", [&] {
    return env_obj.as<sol::protected_function>()(path_arg,
                                                 util_path_with_separator(root),
                                                 opts);
  }) };

  if (!result.valid() || result.get_type() == sol::type::lua_nil) { return env_ops{}; }
  return env_ops_parse(result, context);
}

shell_env_t pkg_env_for_phase(pkg *p, pkg_phase phase, std::filesystem::path const &root) {
  shell_env_t env{ env_pristine() };

  // Breadth-first from the edges this phase waits on, each package at its nearest depth.
  // Every gated dependency is satisfied, so its whole closure has finished too.
  std::unordered_map<pkg *, int> depth;
  std::vector<pkg *> frontier{ [&] {
    std::vector<pkg *> out;
    std::lock_guard const deps_lock(p->deps_mutex);
    for (auto const &[_, dep] : p->dependencies) {
      if (dep.needed_by <= phase) { out.push_back(dep.p); }
    }
    return out;
  }() };
  for (int d{ 1 }; !frontier.empty(); ++d) {
    std::vector<pkg *> next;
    for (pkg *q : frontier) {
      if (!depth.emplace(q, d).second) { continue; }
      std::lock_guard const deps_lock(q->deps_mutex);
      for (auto const &[_, dep] : q->dependencies) { next.push_back(dep.p); }
    }
    frontier = std::move(next);
  }

  std::vector<std::pair<int, pkg *>> order;
  for (auto const &[q, d] : depth) {
    if (q->has_env) { order.emplace_back(d, q); }
  }
  util_sort(order, [](auto const &a, auto const &b) {
    return a.first != b.first ? a.first > b.first
                              : a.second->key.canonical() < b.second->key.canonical();
  });

  for (auto const &[d, q] : order) {
    std::optional<std::filesystem::path> const pkg_path{
      q->type == pkg_type::CACHE_MANAGED && !q->pkg_path.empty()
          ? std::optional{ q->pkg_path }
          : std::nullopt
    };
    if (auto const ops{ pkg_env_eval(q, pkg_path, root) }) {
      tui::debug("env: %s (depth %d)", q->cfg->identity.c_str(), d);
      env_ops_apply(env, *ops);
    }
  }
  return env;
}

shell_env_t pkg_env_for_context(phase_context const *ctx) {
  if (!ctx || !ctx->p) { return env_pristine(); }
  if (ctx->env_root) {
    return pkg_env_for_phase(ctx->p, ctx->p->current_phase.load(), *ctx->env_root);
  }
  if (ctx->lock) {
    return pkg_env_for_phase(ctx->p, ctx->p->current_phase.load(), ctx->lock->tmp_dir());
  }
  return env_pristine();
}

}  // namespace envy
