#include "lua_envy_loadenv_bundle.h"

#include "bundle.h"
#include "cache.h"
#include "lua_envy_module.h"
#include "trace.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace envy {
namespace {

constexpr std::string_view kFn{ "envy.loadenv_bundle" };

// The alias in `table`, parsed against `origin`. Nullopt lets the caller fall back.
std::optional<pkg_cfg::bundle_source> find_alias(sol::object const &table,
                                                 std::string const &alias,
                                                 pkg_decl_origin const &origin) {
  auto const aliases{ bundle::parse_aliases(table, origin) };
  auto const it{ aliases.find(alias) };
  return it == aliases.end() ? std::nullopt : std::optional{ it->second };
}

}  // namespace

void lua_envy_loadenv_bundle_install(sol::state &lua_state,
                                     cache *c,
                                     std::filesystem::path root_path) {
  sol::table envy_table = lua_state["envy"];  // a state-keyed ref: must not outlive it
  envy_table["loadenv_bundle"] = [c, root = std::move(root_path)](
                                     std::string const &alias,
                                     std::string const &module_path,
                                     sol::this_environment te,
                                     sol::this_state L) -> sol::table {
    std::string const scope{ "bundle '" + alias + "'" };
    std::string const subpath{ lua_module_subpath(module_path, kFn, scope) };

    sol::state_view lua{ L };
    sol::table const own{ lua_module_caller_scope(te, lua) };
    std::filesystem::path const caller{ lua_module_caller_file(L, kFn) };

    // Order and anchoring mirror manifest_parse_ctx::find_alias: own BUNDLES, then the
    // root's, each against its writer. Raw, or a sandbox adopts the root's as its own.
    auto const src{ [&]() -> std::optional<pkg_cfg::bundle_source> {
      if (sol::object const declared{ own.raw_get<sol::object>("BUNDLES") };
          declared.valid() && declared.get_type() != sol::type::lua_nil) {
        if (auto found{ find_alias(declared, alias, pkg_decl_origin{ caller }) }) {
          return found;
        }
      }
      return find_alias(lua.globals()["BUNDLES"], alias, pkg_decl_origin{ root });
    }() };

    if (!src) {
      throw std::runtime_error(std::string{ kFn } + ": no bundle alias '" + alias +
                               "' in the BUNDLES table of " + caller.string() +
                               "; declare it above the call, since a manifest is read "
                               "top to bottom");
    }

    std::filesystem::path const bundle_root{ bundle_materialize_bare(*src, c, kFn) };
    std::filesystem::path const full_path{
      lua_module_path_under(bundle_root, subpath, kFn, module_path, scope)
    };

    ENVY_TRACE(lua_ctx_loadenv_bundle,
               "",
               .alias = alias,
               .target = src->bundle_identity,
               .subpath = module_path,  // original dot syntax
               .root = bundle_root.string());

    lua_module_bundle const from{ src->bundle_identity, alias, bundle_root };
    return lua_module_load(lua, full_path, kFn, module_path, own, &from);
  };
}

void lua_envy_loadenv_bundle_install_refusal(sol::table &envy_table) {
  envy_table["loadenv_bundle"] = [](sol::variadic_args) -> sol::table {
    throw std::runtime_error(
        std::string{ kFn } +
        ": manifest scope only; a spec reaches a bundle it declared with "
        "envy.loadenv_spec(identity, module)");
  };
}

}  // namespace envy
