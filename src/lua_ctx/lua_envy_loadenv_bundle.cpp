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

// Where the caller's own globals live. An imported fragment assigns into the sandbox
// envy.import gave it, so the root manifest's _G would not have them.
sol::table caller_scope(sol::this_environment const &te, sol::state_view lua) {
  return te.env ? sol::table{ *te.env } : sol::table{ lua.globals() };
}

// The alias in `table`, parsed against the file that declared it. Nullopt when the
// table has no such key, so the caller can fall back.
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
  // A sol reference keyed on this state, so it must not outlive it: taken and dropped
  // inside this call rather than handed back to a caller that unwinds past the state.
  sol::table envy_table = lua_state["envy"];
  envy_table["loadenv_bundle"] = [c, root = std::move(root_path)](
                                     std::string const &alias,
                                     std::string const &module_path,
                                     sol::this_environment te,
                                     sol::this_state L) -> sol::table {
    std::string const scope{ "bundle '" + alias + "'" };
    std::string const subpath{ lua_module_subpath(module_path, kFn, scope) };

    sol::state_view lua{ L };
    std::filesystem::path const caller{ lua_module_caller_file(L, kFn) };

    // Lookup order and anchoring both mirror a literal `bundle = "alias"` entry (see
    // manifest_parse_ctx::find_alias): the calling file's own BUNDLES wins, since
    // that file wrote the reference, and the root manifest's is the fallback. Each
    // is parsed against the file that declared it, so a relative `source` resolves
    // where it was written -- a raw read, because a fragment's sandbox falls through
    // to the root's globals and would otherwise adopt the root's table as its own.
    auto const src{ [&]() -> std::optional<pkg_cfg::bundle_source> {
      sol::table const own{ caller_scope(te, lua) };
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

    return lua_module_load(lua, full_path, kFn, module_path);
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
