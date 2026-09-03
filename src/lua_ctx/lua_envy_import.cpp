#include "lua_envy_import.h"

#include "envy_release.h"
#include "lua_envy.h"
#include "manifest.h"
#include "pkg_cfg.h"
#include "trace.h"
#include "tui.h"
#include "util.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace envy {

namespace {

namespace fs = std::filesystem;

// The Lua file that called us, absolute. Level 2 is the caller of a C function (level
// 1 being the C function itself), the same walk envy.abspath makes. Absolute because a
// chunk name can be a bare filename, which names the CWD as envy.loadenv reads it too.
fs::path caller_file(lua_State *L) {
  sol::state_view lua{ L };
  sol::table const info{ lua["debug"]["getinfo"](2, "S") };
  sol::optional<std::string> const source{ info["source"] };
  if (!source) {
    throw std::runtime_error("envy.import: cannot determine caller's source file");
  }

  std::string_view s{ *source };
  if (!s.empty() && s.front() == '@') { s.remove_prefix(1); }  // file-source prefix
  return util_canonical_path(s);
}

bool has_field(sol::table const &t, char const *key) {
  sol::object const o{ t[key] };
  return o.valid() && o.get_type() != sol::type::lua_nil;
}

// Stamp the imported manifest's own paths and aliases onto what it declared, so a
// superproject can splice the entries into its PACKAGES untouched. Set-if-absent, so
// a nested import's inner tags survive being re-tagged by the outer one.
void tag_declarations(sol::table const &env, std::string const &base) {
  // Raw: the sandbox falls through to the importing manifest's globals, so a fragment
  // that assigns neither would otherwise get its importer's tables stamped.
  sol::object const bundles{ env.raw_get<sol::object>("BUNDLES") };

  if (sol::object const pkgs{ env.raw_get<sol::object>("PACKAGES") };
      pkgs.is<sol::table>()) {
    sol::table const t{ pkgs.as<sol::table>() };
    for (size_t i{ 1 }, n{ t.size() }; i <= n; ++i) {
      sol::object const e{ t[i] };
      if (!e.is<sol::table>()) { continue; }  // shorthand string carries no paths
      sol::table entry{ e.as<sol::table>() };
      if (!has_field(entry, kEnvyBaseKey)) { entry[kEnvyBaseKey] = base; }
      if (bundles.is<sol::table>() && !has_field(entry, kEnvyBundlesKey)) {
        entry[kEnvyBundlesKey] = bundles;
      }
    }
  }

  if (bundles.is<sol::table>()) {
    for (auto const &[key, value] : bundles.as<sol::table>()) {
      if (!value.is<sol::table>()) { continue; }  // parse_aliases rejects it by name
      sol::table decl{ value.as<sol::table>() };
      if (!has_field(decl, kEnvyBaseKey)) { decl[kEnvyBaseKey] = base; }
    }
  }
}

sol::table imported_bundles_list(sol::state_view lua) {
  if (sol::object const obj{ lua.registry()[ENVY_IMPORTS_RIDX] }; obj.is<sol::table>()) {
    return obj.as<sol::table>();
  }
  sol::table const list{ lua.create_table() };
  lua.registry()[ENVY_IMPORTS_RIDX] = list;
  return list;
}

// Advisory only, and only against the root pin: an imported manifest asking for a
// newer envy than the one running cannot be satisfied, since bootstrap already chose
// the binary from the root header. Older is a warning -- it still runs.
void check_version_agreement(std::optional<std::string> const &root_version,
                             envy_meta const &imported,
                             fs::path const &resolved) {
  if (!root_version || !imported.version || *imported.version == *root_version) { return; }
  if (envy_release_version_less(*root_version, *imported.version)) {
    throw std::runtime_error("envy.import: " + resolved.string() + " requires envy " +
                             *imported.version + ", but the root manifest pins " +
                             *root_version);
  }
  tui::warn("envy.import: %s pins envy %s; the root manifest pins %s",
            resolved.string().c_str(),
            imported.version->c_str(),
            root_version->c_str());
}

}  // namespace

void lua_envy_import_install(sol::state &lua,
                             std::optional<std::string> const &root_version,
                             fs::path const &root_path) {
  sol::table envy_table{ lua["envy"] };

  // Files currently executing, innermost last: the root manifest plus every import
  // above this one. Membership is the cycle test.
  auto const chain{ std::make_shared<std::vector<fs::path>>(
      std::vector<fs::path>{ util_canonical_path(root_path) }) };

  envy_table.set_function(
      "import",
      [chain, root_version](sol::this_state L, std::string const &arg) -> sol::table {
        sol::state_view lua_view{ L };
        fs::path const importer{ caller_file(L) };

        fs::path const resolved{ [&] {
          fs::path const arg_path{ arg };
          fs::path r{ arg_path.is_absolute()
                          ? arg_path
                          : (importer.parent_path() / arg_path).lexically_normal() };
          if (fs::is_directory(r)) { r /= "envy.lua"; }
          if (!fs::exists(r)) {
            throw std::runtime_error("envy.import: '" + arg + "' not found (resolved to " +
                                     r.string() + ")");
          }
          return util_canonical_path(r);
        }() };

        if (auto const it{ std::ranges::find(*chain, resolved) }; it != chain->end()) {
          std::string cycle;
          for (auto i{ it }; i != chain->end(); ++i) { cycle += i->string() + " -> "; }
          throw std::runtime_error("envy.import: import cycle: " + cycle +
                                   resolved.string());
        }

        auto const content{ util_load_file(resolved) };
        check_version_agreement(
            root_version,
            parse_envy_meta(
                { reinterpret_cast<char const *>(content.data()), content.size() }),
            resolved);

        // Sandbox with the stdlib visible, as envy.loadenv builds: assigned globals
        // land here rather than in the importing manifest's.
        sol::table env{ lua_view.create_table() };
        env[sol::metatable_key] =
            lua_view.create_table_with("__index", lua_view.globals());
        env["ENVY_IMPORTER"] = util_normalized_path(importer);

        sol::protected_function const loadfile{ lua_view["loadfile"] };
        sol::protected_function_result load_res{ loadfile(resolved.string(), "t", env) };
        if (!load_res.valid()) {
          sol::error const err{ load_res };
          throw std::runtime_error("envy.import: " + std::string{ err.what() });
        }
        sol::object const chunk{ load_res.get<sol::object>(0) };
        if (!chunk.is<sol::protected_function>()) {
          throw std::runtime_error("envy.import: cannot load " + resolved.string() + ": " +
                                   (load_res.return_count() > 1
                                        ? load_res.get<std::string>(1)
                                        : std::string{ "unknown error" }));
        }

        chain->push_back(resolved);
        struct chain_pop {
          std::vector<fs::path> *chain;
          ~chain_pop() { chain->pop_back(); }
        } const pop{ chain.get() };

        if (sol::protected_function_result const run{
                chunk.as<sol::protected_function>()() };
            !run.valid()) {
          sol::error const err{ run };
          throw std::runtime_error("envy.import: " + resolved.string() + ": " +
                                   std::string{ err.what() });
        }

        tag_declarations(env, util_normalized_path(resolved));
        if (sol::object const bundles{ env.raw_get<sol::object>("BUNDLES") };
            bundles.is<sol::table>()) {
          sol::table list{ imported_bundles_list(lua_view) };
          list[list.size() + 1] = bundles;
        }

        ENVY_TRACE(manifest_imported,
                   "",
                   .path = resolved.string(),
                   .importer = importer.string());
        return env;
      });
}

std::vector<sol::table> lua_envy_import_bundle_tables(sol::state_view lua) {
  std::vector<sol::table> tables;
  if (sol::object const obj{ lua.registry()[ENVY_IMPORTS_RIDX] }; obj.is<sol::table>()) {
    sol::table const list{ obj.as<sol::table>() };
    for (size_t i{ 1 }, n{ list.size() }; i <= n; ++i) {
      if (sol::object const e{ list[i] }; e.is<sol::table>()) {
        tables.push_back(e.as<sol::table>());
      }
    }
  }
  return tables;
}

}  // namespace envy
