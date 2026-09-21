#include "lua_envy_loadenv_bundle.h"

#include "bundle.h"
#include "cache.h"
#include "lua_envy_module.h"
#include "trace.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace envy {
namespace {

constexpr std::string_view kFn{ "envy.loadenv_bundle" };

// Where the caller's BUNDLES lives. An imported fragment assigns its globals into
// the sandbox envy.import gave it, so the root manifest's _G would not have them --
// and the fragment's own aliases are the ones its own call means.
sol::table caller_scope(sol::this_environment const &te, sol::state_view lua) {
  return te.env ? sol::table{ *te.env } : sol::table{ lua.globals() };
}

}  // namespace

void lua_envy_loadenv_bundle_install(sol::state &lua_state, cache *c) {
  // A sol reference keyed on this state, so it must not outlive it: taken and dropped
  // inside this call rather than handed back to a caller that unwinds past the state.
  sol::table envy_table = lua_state["envy"];
  envy_table["loadenv_bundle"] = [c](std::string const &alias,
                                     std::string const &module_path,
                                     sol::this_environment te,
                                     sol::this_state L) -> sol::table {
    std::string const scope{ "bundle '" + alias + "'" };
    std::string const subpath{ lua_module_subpath(module_path, kFn, scope) };

    sol::state_view lua{ L };
    // The calling file, not the root manifest: an imported fragment's relative
    // `source` anchors on the fragment, and this runs before envy.import has had a
    // chance to stamp ENVY_BASE onto what it declared.
    std::filesystem::path const caller{ lua_module_caller_file(L, kFn) };
    auto const bundles{ bundle::parse_aliases(caller_scope(te, lua)["BUNDLES"],
                                              pkg_decl_origin{ caller }) };

    auto const found{ bundles.find(alias) };
    if (found == bundles.end()) {
      throw std::runtime_error(std::string{ kFn } + ": no bundle alias '" + alias +
                               "' in the BUNDLES table of " + caller.string() +
                               "; declare it above the call, since a manifest is read "
                               "top to bottom");
    }

    std::filesystem::path const root{ bundle_materialize_bare(found->second, c, kFn) };
    std::filesystem::path const full_path{
      lua_module_path_under(root, subpath, kFn, module_path, scope)
    };

    ENVY_TRACE(lua_ctx_loadenv_bundle,
               "",
               .alias = alias,
               .target = found->second.bundle_identity,
               .subpath = module_path,  // original dot syntax
               .root = root.string());

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
