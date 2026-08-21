#include "lua_envy_product.h"

#include "engine.h"
#include "lua_phase_context.h"
#include "pkg.h"
#include "pkg_phase.h"
#include "product_util.h"
#include "trace.h"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace envy {

void lua_envy_product_install(sol::table &envy_table) {
  // envy.product(name) -> path_or_value_string
  envy_table["product"] = [](std::string const &product_name,
                             sol::this_state L) -> std::string {
    phase_context const *ctx{ lua_phase_context_get(L) };
    pkg *consumer{ ctx ? ctx->p : nullptr };
    if (!consumer) {
      throw std::runtime_error("envy.product: not in phase context (missing pkg)");
    }
    if (product_name.empty()) {
      throw std::runtime_error("envy.product: product name cannot be empty");
    }

    pkg_phase const current_phase{ consumer->current_phase.load() };

    // Copy under deps_mutex - the resolution loop writes provider concurrently.
    auto const dep_opt{ [&]() -> std::optional<pkg::product_dependency> {
      std::lock_guard const deps_lock(consumer->deps_mutex);
      auto const dep_it{ consumer->product_dependencies.find(product_name) };
      if (dep_it == consumer->product_dependencies.end()) { return std::nullopt; }
      return dep_it->second;
    }() };
    // No declared product dependency: consult the project-wide registry so a
    // package that already depends on the provider by identity can name its
    // products without restating `product =` on the entry. The dependency edge
    // stays mandatory — it is what drove the provider through install, and
    // nothing else makes its payload readable at this point.
    auto const registry_dep{ [&]() -> std::optional<pkg::product_dependency> {
      if (dep_opt) { return std::nullopt; }
      pkg *const provider{ ctx->eng ? ctx->eng->find_product_provider(product_name)
                                    : nullptr };
      if (!provider) { return std::nullopt; }

      // dependencies is keyed by bare identity while pkg_key includes options, so a
      // hit can name a different package than the registry's provider — a debug
      // variant's edge does not order a release variant's payload.
      auto const edge_needed_by{ [&]() -> std::optional<pkg_phase> {
        std::lock_guard const deps_lock(consumer->deps_mutex);
        auto const it{ consumer->dependencies.find(provider->cfg->identity) };
        return it == consumer->dependencies.end() || it->second.p != provider
                   ? std::nullopt
                   : std::optional{ it->second.needed_by };
      }() };
      if (!edge_needed_by) {
        std::string const msg{ "envy.product: '" + provider->cfg->identity +
                               "' provides product '" + product_name + "', but pkg '" +
                               consumer->cfg->identity +
                               "' does not depend on it — declare it as a dependency" };
        ENVY_TRACE(lua_ctx_product_access,
                   consumer->cfg->identity,
                   .target = product_name,
                   .provider = provider->cfg->identity,
                   .current_phase = current_phase,
                   .needed_by = pkg_phase::none,
                   .allowed = false,
                   .reason = msg);
        throw std::runtime_error(msg);
      }

      return pkg::product_dependency{ .name = product_name,
                                      .needed_by = *edge_needed_by,
                                      .provider = provider,
                                      .constraint_identity = {} };
    }() };

    if (!dep_opt && !registry_dep) {
      std::string const msg{ "envy.product: pkg '" + consumer->cfg->identity +
                             "' does not declare product dependency on '" + product_name +
                             "'" };
      ENVY_TRACE(lua_ctx_product_access,
                 consumer->cfg->identity,
                 .target = product_name,
                 .provider = "",
                 .current_phase = current_phase,
                 .needed_by = pkg_phase::none,
                 .allowed = false,
                 .reason = msg);
      throw std::runtime_error(msg);
    }

    pkg::product_dependency const &dep{ dep_opt ? *dep_opt : *registry_dep };

    auto emit_access = [&](bool allowed, std::string const &reason) {
      std::string const provider_identity{ dep.provider ? dep.provider->cfg->identity
                                                        : std::string{} };
      ENVY_TRACE(lua_ctx_product_access,
                 consumer->cfg->identity,
                 .target = product_name,
                 .provider = provider_identity,
                 .current_phase = current_phase,
                 .needed_by = dep.needed_by,
                 .allowed = allowed,
                 .reason = reason);
    };

    if (current_phase < dep.needed_by) {
      std::string const msg{ "envy.product: product '" + product_name + "' needed_by '" +
                             std::string(pkg_phase_name(dep.needed_by)) +
                             "' but accessed during '" +
                             std::string(pkg_phase_name(current_phase)) + "'" };
      emit_access(false, msg);
      throw std::runtime_error(msg);
    }

    if (!dep.provider) {
      std::string const msg{ "envy.product: product '" + product_name +
                             "' provider not resolved for pkg '" +
                             consumer->cfg->identity + "'" };
      emit_access(false, msg);
      throw std::runtime_error(msg);
    }

    if (!dep.constraint_identity.empty() &&
        dep.provider->cfg->identity != dep.constraint_identity) {
      std::string const msg{ "envy.product: product '" + product_name +
                             "' must come from '" + dep.constraint_identity +
                             "', but provider is '" + dep.provider->cfg->identity + "'" };
      emit_access(false, msg);
      throw std::runtime_error(msg);
    }

    std::string const value{ product_util_resolve(dep.provider, product_name) };
    emit_access(true, value);
    return value;
  };
}

}  // namespace envy
