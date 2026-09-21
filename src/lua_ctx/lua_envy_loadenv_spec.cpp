#include "lua_envy_loadenv_spec.h"

#include "bundle.h"
#include "engine.h"
#include "lua_envy_dep_util.h"
#include "lua_envy_module.h"
#include "lua_phase_context.h"
#include "pkg.h"
#include "pkg_phase.h"
#include "trace.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace envy {
namespace { constexpr std::string_view kFn{ "envy.loadenv_spec" }; }  // namespace

void lua_envy_loadenv_spec_install(sol::table &envy_table) {
  // envy.loadenv_spec(identity, module_path) -> table
  // Load Lua code from a declared dependency into a sandboxed environment
  // module_path uses Lua dot syntax (e.g., "lib.helpers" -> "lib/helpers.lua")
  envy_table["loadenv_spec"] = [](std::string const &identity,
                                  std::string const &module_path,
                                  sol::this_state L) -> sol::table {
    std::string const scope{ "dependency '" + identity + "'" };
    std::string const subpath{ lua_module_subpath(module_path, kFn, scope) };
    // Verify we're in a phase context (not global scope)
    phase_context const *ctx{ lua_phase_context_get(L) };
    pkg *consumer{ ctx ? ctx->p : nullptr };
    if (!consumer) {
      throw std::runtime_error(
          "envy.loadenv_spec: can only be called within phase functions, not at global "
          "scope");
    }

    engine *eng{ ctx->eng };
    if (!eng) { throw std::runtime_error("envy.loadenv_spec: missing engine context"); }

    pkg_phase const current_phase{ consumer->current_phase.load() };

    auto emit_access = [&](bool allowed, pkg_phase needed_by, std::string const &reason) {
      ENVY_TRACE(lua_ctx_loadenv_spec_access,
                 consumer->cfg->identity,
                 .target = identity,
                 .subpath = module_path,  // original dot syntax
                 .current_phase = current_phase,
                 .needed_by = needed_by,
                 .allowed = allowed,
                 .reason = reason);
    };

    auto const edge{ find_direct_dependency(consumer, identity) };
    if (!edge) {
      std::string const msg{ "envy.loadenv_spec: pkg '" + consumer->cfg->identity +
                             "' has no dependency on '" + identity + "'" };
      emit_access(false, pkg_phase::none, msg);
      throw std::runtime_error(msg);
    }

    pkg_phase const first_needed_by{ edge->needed_by };
    if (current_phase < first_needed_by) {
      std::string const msg{ "envy.loadenv_spec: dependency '" + identity +
                             "' needed_by '" +
                             std::string(pkg_phase_name(first_needed_by)) +
                             "' but accessed during '" +
                             std::string(pkg_phase_name(current_phase)) + "'" };
      emit_access(false, first_needed_by, msg);
      throw std::runtime_error(msg);
    }

    std::string const &canonical_id{ edge->identity };
    pkg const *dep{ edge->p };

    // Where to load from, and the bundle behind it: the dependency is a bundle itself,
    // a spec out of one, or an atomic spec -- which has no bundle and loads from its own
    // cache directory.
    auto const [load_root, bundle_id]{
      [&]() -> std::pair<std::filesystem::path, std::string> {
        bool const whole_bundle{ dep->type == pkg_type::BUNDLE_ONLY };
        if (!whole_bundle && !dep->cfg->bundle_identity) {
          if (!dep->spec_file_path.has_value() || dep->spec_file_path->empty()) {
            throw std::runtime_error("envy.loadenv_spec: spec '" + identity +
                                     "' has no spec_file_path");
          }
          return { dep->spec_file_path->parent_path(), std::string{} };
        }
        std::string id{ whole_bundle ? canonical_id : *dep->cfg->bundle_identity };
        bundle const *b{ eng->find_bundle(id) };
        if (!b) {
          throw std::runtime_error("envy.loadenv_spec: bundle '" + id +
                                   "' not found in registry for '" + identity + "'");
        }
        return { b->cache_path, std::move(id) };
      }()
    };

    std::filesystem::path const full_path{
      lua_module_path_under(load_root, subpath, kFn, module_path, scope)
    };
    // No alias: this loader resolves by identity, so the caller never wrote one.
    lua_module_bundle const from{ bundle_id, {}, load_root };
    sol::table const module{ lua_module_load(sol::state_view{ L },
                                             full_path,
                                             kFn,
                                             module_path,
                                             bundle_id.empty() ? nullptr : &from) };
    emit_access(true, first_needed_by, full_path.string());
    return module;
  };
}

}  // namespace envy
