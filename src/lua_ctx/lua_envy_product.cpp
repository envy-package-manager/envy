#include "lua_envy_product.h"

#include "engine.h"
#include "env_ops.h"
#include "lua_phase_context.h"
#include "lua_shell.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_env.h"
#include "pkg_phase.h"
#include "product_util.h"
#include "shell.h"
#include "trace.h"
#include "util.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>

namespace envy {

namespace {

// Who asked for which product, and what it resolved to. Every outcome -- allowed or
// refused -- reports the same event, so `trace` and `refuse` build both from one place.
struct product_access {
  pkg *consumer;
  std::string const &product_name;
  char const *fn;  // "envy.product" or "envy.product_command", prefixing every message
  pkg_phase current_phase;
  pkg *provider{ nullptr };
  pkg_phase needed_by{ pkg_phase::none };

  void trace(bool allowed, std::string const &reason) const {
    ENVY_TRACE(lua_ctx_product_access,
               consumer->cfg->identity,
               .target = product_name,
               .provider = provider ? provider->cfg->identity : std::string{},
               .current_phase = current_phase,
               .needed_by = needed_by,
               .allowed = allowed,
               .reason = reason);
  }

  std::runtime_error refuse(std::string const &why) const {
    std::string msg{ std::string{ fn } + ": " + why };
    trace(false, msg);
    return std::runtime_error{ std::move(msg) };
  }
};

// The provider `product_name` names for the calling package, refused unless a direct edge
// to it has reached its needed_by.
product_access authorize(char const *fn,
                         std::string const &product_name,
                         phase_context const *ctx) {
  pkg *const consumer{ ctx ? ctx->p : nullptr };
  if (!consumer) { throw_runtime_error({ fn, ": not in phase context (missing pkg)" }); }
  if (product_name.empty()) {
    throw_runtime_error({ fn, ": product name cannot be empty" });
  }

  product_access a{ .consumer = consumer,
                    .product_name = product_name,
                    .fn = fn,
                    .current_phase = consumer->current_phase.load() };

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
      throw a.refuse("pkg '" + consumer->cfg->identity +
                     "' does not declare product dependency on '" + product_name + "'");
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
      a.provider = provider;
      throw a.refuse("'" + provider->cfg->identity + "' provides product '" +
                     product_name + "', but pkg '" + consumer->cfg->identity +
                     "' does not depend on it — declare it as a dependency");
    }

    return pkg::product_dependency{ .name = product_name,
                                    .needed_by = *edge,
                                    .provider = provider };
  }() };

  a.provider = dep.provider;
  a.needed_by = dep.needed_by;

  if (a.current_phase < dep.needed_by) {
    throw a.refuse("product '" + product_name + "' needed_by '" +
                   std::string(pkg_phase_name(dep.needed_by)) + "' but accessed during '" +
                   std::string(pkg_phase_name(a.current_phase)) + "'");
  }

  if (!dep.provider) {
    throw a.refuse("product '" + product_name + "' provider not resolved for pkg '" +
                   consumer->cfg->identity + "'");
  }

  if (!dep.constraint_identity.empty() &&
      dep.provider->cfg->identity != dep.constraint_identity) {
    throw a.refuse("product '" + product_name + "' must come from '" +
                   dep.constraint_identity + "', but provider is '" +
                   dep.provider->cfg->identity + "'");
  }

  // The declarer: the edge's package, or for a weak fallback one in its closure --
  // finished whenever the edge's package is. A pin to a package that lacks it is refused.
  try {
    if (pkg *const owner{ product_util_owner(consumer, product_name) }) {
      a.provider = owner;
    }
  } catch (std::exception const &e) { throw a.refuse(e.what()); }
  return a;
}

// The project's bin dir, whose wrappers a product's own program must be searched past.
std::filesystem::path bin_dir(engine const *eng) {
  manifest const *const m{ eng ? eng->get_manifest() : nullptr };
  if (!m || !m->meta.bin) { return {}; }
  std::error_code ec;
  auto bin{ std::filesystem::canonical(eng->project_root() / *m->meta.bin, ec) };
  return ec ? std::filesystem::path{} : bin;
}

// The shell a product command is quoted for: the one named, else the phase's own, as
// envy.run picks it. A custom shell has no quoting envy knows, so it must be named.
shell_choice command_shell(sol::optional<sol::object> const &shell_obj,
                           phase_context const *ctx) {
  resolved_shell const shell{ shell_obj && shell_obj->valid()
                                  ? parse_shell_config_from_lua(*shell_obj, "shell")
                              : ctx->builtin_shell ? shell_resolve_default(nullptr)
                                                   : pkg_default_shell(ctx->p) };
  if (auto const *choice{ std::get_if<shell_choice>(&shell) }) { return *choice; }
  throw_runtime_error({ "the shell is custom; pass an ENVY_SHELL constant to quote for" });
}

}  // namespace

void lua_envy_product_install(sol::table &envy_table) {
  // envy.product(name) -> path_or_value_string
  envy_table["product"] = [](std::string const &product_name,
                             sol::this_state L) -> std::string {
    auto const a{ authorize("envy.product", product_name, lua_phase_context_get(L)) };
    if (product_util_is_command(a.provider, product_name)) {
      throw a.refuse("product '" + product_name + "' is a command, not a path; use " +
                     "envy.product_command");
    }
    std::string const value{ product_util_resolve(a.provider, product_name) };
    a.trace(true, value);
    return value;
  };

  // envy.product_command(name, [shell]) -> command line quoted for that shell
  envy_table["product_command"] = [](std::string const &product_name,
                                     sol::optional<sol::object> shell_obj,
                                     sol::this_state L) -> std::string {
    phase_context const *ctx{ lua_phase_context_get(L) };
    auto const a{ authorize("envy.product_command", product_name, ctx) };
    std::string const command{ [&] {
      try {
        auto const root{ pkg_env_root(ctx) };  // pkg_state_dir hangs where ENV's does
        if (!root) { throw std::runtime_error{ "the phase has no scratch dir" }; }
        std::string program;
        auto argv{ product_util_run_argv(a.provider, product_name, *root, &program) };
        // A bare program resolves on the phase's PATH from its run dir, as `--run`
        // resolves it on the tool's: never a PowerShell alias (curl), a cmd builtin
        // (start), or the product's own wrapper by that name.
        if (!util_path_from_utf8(argv.front()).has_parent_path()) {
          shell_env_t const env{ pkg_env_for_context(ctx) };
          std::string const *const path{ env_get(env, "PATH") };
          auto const bin{ bin_dir(ctx->eng) };
          auto found{ product_util_find_program(
              argv.front(),
              program,
              path ? *path : std::string{},
              lua_phase_context_resolve(L, ".").lexically_normal(),
              bin.empty() ? nullptr : &bin) };
          if (!found) { throw std::runtime_error{ "'" + argv.front() + "' not on PATH" }; }
          argv.front() = std::move(*found);
        }
        return shell_quote_argv(argv, command_shell(shell_obj, ctx));
      } catch (std::exception const &e) { throw a.refuse(e.what()); }
    }() };
    a.trace(true, command);
    return command;
  };
}

}  // namespace envy
