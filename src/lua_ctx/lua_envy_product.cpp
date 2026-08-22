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

    // Every outcome — allowed or refused — reports the same event, so trace once and
    // build the exception from the same message rather than repeating both per path.
    auto const trace{ [&](std::string const &provider_identity,
                          pkg_phase needed_by,
                          bool allowed,
                          std::string const &reason) {
      ENVY_TRACE(lua_ctx_product_access,
                 consumer->cfg->identity,
                 .target = product_name,
                 .provider = provider_identity,
                 .current_phase = current_phase,
                 .needed_by = needed_by,
                 .allowed = allowed,
                 .reason = reason);
    } };
    auto const refuse{ [&](std::string const &provider_identity,
                           pkg_phase needed_by,
                           std::string msg) {
      trace(provider_identity, needed_by, false, msg);
      return std::runtime_error{ std::move(msg) };
    } };

    pkg::product_dependency const dep{ [&] {
      {  // A declared product dependency wins. Copy under deps_mutex — the
         // resolution loop writes provider concurrently.
        std::lock_guard const deps_lock(consumer->deps_mutex);
        auto const it{ consumer->product_dependencies.find(product_name) };
        if (it != consumer->product_dependencies.end()) { return it->second; }
      }

      // Otherwise the project-wide registry, so a package that already depends on the
      // provider by identity can name its products without restating `product =`. The
      // dependency edge stays mandatory: it is what drove the provider through
      // install, and nothing else makes its payload readable at this point.
      pkg *const provider{ ctx->eng ? ctx->eng->find_product_provider(product_name)
                                    : nullptr };
      if (!provider) {
        throw refuse("",
                     pkg_phase::none,
                     "envy.product: pkg '" + consumer->cfg->identity +
                         "' does not declare product dependency on '" + product_name +
                         "'");
      }

      // dependencies is keyed by bare identity while pkg_key includes options, so a
      // hit can name a different package than the registry's provider — a debug
      // variant's edge does not order a release variant's payload.
      auto const edge{ [&]() -> std::optional<pkg_phase> {
        std::lock_guard const deps_lock(consumer->deps_mutex);
        auto const it{ consumer->dependencies.find(provider->cfg->identity) };
        return it == consumer->dependencies.end() || it->second.p != provider
                   ? std::nullopt
                   : std::optional{ it->second.needed_by };
      }() };
      if (!edge) {
        throw refuse(provider->cfg->identity,
                     pkg_phase::none,
                     "envy.product: '" + provider->cfg->identity +
                         "' provides product '" + product_name + "', but pkg '" +
                         consumer->cfg->identity +
                         "' does not depend on it — declare it as a dependency");
      }

      return pkg::product_dependency{ .name = product_name,
                                      .needed_by = *edge,
                                      .provider = provider };
    }() };

    std::string const provider_identity{ dep.provider ? dep.provider->cfg->identity
                                                      : std::string{} };

    if (current_phase < dep.needed_by) {
      throw refuse(provider_identity,
                   dep.needed_by,
                   "envy.product: product '" + product_name + "' needed_by '" +
                       std::string(pkg_phase_name(dep.needed_by)) +
                       "' but accessed during '" +
                       std::string(pkg_phase_name(current_phase)) + "'");
    }

    if (!dep.provider) {
      throw refuse(provider_identity,
                   dep.needed_by,
                   "envy.product: product '" + product_name +
                       "' provider not resolved for pkg '" + consumer->cfg->identity +
                       "'");
    }

    if (!dep.constraint_identity.empty() &&
        dep.provider->cfg->identity != dep.constraint_identity) {
      throw refuse(provider_identity,
                   dep.needed_by,
                   "envy.product: product '" + product_name + "' must come from '" +
                       dep.constraint_identity + "', but provider is '" +
                       dep.provider->cfg->identity + "'");
    }

    std::string const value{ product_util_resolve(dep.provider, product_name) };
    trace(provider_identity, dep.needed_by, true, value);
    return value;
  };
}

}  // namespace envy
