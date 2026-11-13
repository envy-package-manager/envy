#include "lua_envy_subproject.h"

#include "envy_release.h"
#include "lua_envy.h"
#include "lua_envy_module.h"
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

constexpr char kFn[]{ "envy.subproject" };
constexpr char kProjectRootKey[]{ "PROJECT_ROOT" };

bool has_field(sol::table const &t, char const *key) {
  sol::object const o{ t[key] };
  return o.valid() && o.get_type() != sol::type::lua_nil;
}

// Stamp the subproject's own paths and aliases onto what it declared, so a superproject
// can splice the entries into its PACKAGES untouched. Set-if-absent, so a nested
// subproject's inner tags survive being re-tagged by the outer one.
void tag_declarations(sol::table const &env, std::string const &base) {
  // Raw: the sandbox falls through to the superproject's globals, so a subproject that
  // assigns neither would otherwise get its superproject's tables stamped.
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

// Globals read only from the root state. PACKAGES and BUNDLES are read back out of the
// sandbox; PACKAGE_DEPOTS only accelerates, so leaving one unadopted is harmless.
constexpr char const *kRootOnlyGlobals[]{ "DEFAULT_SHELL", "VENDOR_ROOT" };

sol::table subproject_list(sol::state_view lua) {
  if (sol::object const obj{ lua.registry()[ENVY_SUBPROJECT_ENVS_RIDX] };
      obj.is<sol::table>()) {
    return obj.as<sol::table>();
  }
  sol::table const list{ lua.create_table() };
  lua.registry()[ENVY_SUBPROJECT_ENVS_RIDX] = list;
  return list;
}

// Every subproject loaded so far, in load order: its manifest's path and its globals.
void for_each_subproject(sol::state_view lua,
                         function_ref<void(std::string const &, sol::table const &)> fn) {
  sol::object const obj{ lua.registry()[ENVY_SUBPROJECT_ENVS_RIDX] };
  if (!obj.is<sol::table>()) { return; }
  sol::table const list{ obj.as<sol::table>() };
  for (size_t i{ 1 }, n{ list.size() }; i <= n; ++i) {
    if (sol::object const e{ list[i] }; e.is<sol::table>()) {
      sol::table const entry{ e.as<sol::table>() };
      fn(entry.get<std::string>("path"), entry.get<sol::table>("env"));
    }
  }
}

// Advisory only, and only against the root pin: a subproject asking for a newer envy
// than the one running cannot be satisfied, since bootstrap already chose the binary from
// the root header. Older is a warning -- it still runs.
void check_version_agreement(std::optional<std::string> const &root_version,
                             envy_meta const &sub,
                             fs::path const &resolved) {
  if (!root_version || !sub.version || *sub.version == *root_version) { return; }
  if (envy_release_version_less(*root_version, *sub.version)) {
    throw_runtime_error({ kFn,
                          ": ",
                          resolved,
                          " requires envy ",
                          *sub.version,
                          ", but the root manifest pins ",
                          *root_version });
  }
  tui::warn("%s: %s pins envy %s; the root manifest pins %s",
            kFn,
            resolved.string().c_str(),
            sub.version->c_str(),
            root_version->c_str());
}

}  // namespace

void lua_envy_subproject_install(sol::state &lua,
                                 std::optional<std::string> const &root_version,
                                 fs::path const &root_path) {
  sol::table envy_table = lua["envy"];
  envy_table["IS_SUBPROJECT"] = false;  // each subproject's own envy shadows it

  // Files currently executing, innermost last: the root manifest plus every subproject
  // above this one. Membership is the cycle test.
  auto const chain{ std::make_shared<std::vector<fs::path>>(
      std::vector<fs::path>{ util_canonical_path(root_path) }) };

  envy_table.set_function(
      "subproject",
      [chain, root_version](sol::this_state L, std::string const &arg) -> sol::table {
        sol::state_view lua_view{ L };
        // Canonical: the cycle test compares resolved paths, not spellings.
        fs::path const parent{ lua_module_caller_file(L, kFn, caller_path::CANONICAL_) };

        // A project is a directory holding envy.lua, as discovery has it; never a file.
        fs::path const resolved{ [&] {
          fs::path const arg_path{ arg };
          fs::path const dir{ arg_path.is_absolute()
                                  ? arg_path
                                  : (parent.parent_path() / arg_path).lexically_normal() };
          if (fs::exists(dir) && !fs::is_directory(dir)) {
            throw_runtime_error(
                { kFn, ": '", arg, "' is not a directory holding an envy.lua" });
          }
          fs::path const manifest{ dir / "envy.lua" };
          if (!fs::is_regular_file(manifest)) {
            throw_runtime_error(
                { kFn, ": '", arg, "' not found (resolved to ", manifest, ")" });
          }
          return util_canonical_path(manifest);
        }() };
        std::string const project_root{ util_normalized_path(resolved.parent_path()) };

        if (auto const it{ std::ranges::find(*chain, resolved) }; it != chain->end()) {
          std::string cycle;
          for (auto i{ it }; i != chain->end(); ++i) { cycle += i->string() + " -> "; }
          throw_runtime_error({ kFn, ": subproject cycle: ", cycle, resolved });
        }

        auto const content{ util_load_file(resolved) };
        check_version_agreement(
            root_version,
            parse_envy_meta(
                { reinterpret_cast<char const *>(content.data()), content.size() }),
            resolved);

        // Sandbox over _G, as envy.loadenv builds: assigned globals land here. Its envy
        // and later PROJECT_ROOT sit on a layer between, so pairs() never yields them.
        sol::table const shared_envy = lua_view.globals()["envy"];
        sol::environment own_envy{ lua_view, sol::create, shared_envy };
        own_envy["IS_SUBPROJECT"] = true;
        own_envy[kProjectRootKey] = project_root;
        sol::environment scope{ lua_view, sol::create, lua_view.globals() };
        scope["envy"] = own_envy;
        sol::environment const env{ lua_view, sol::create, scope };

        sol::protected_function const loadfile{ lua_view["loadfile"] };
        sol::protected_function_result load_res{ loadfile(resolved.string(), "t", env) };
        if (!load_res.valid()) {
          sol::error const err = load_res;
          throw_runtime_error({ kFn, ": ", err.what() });
        }
        sol::object const chunk{ load_res.get<sol::object>(0) };
        if (!chunk.is<sol::protected_function>()) {
          throw_runtime_error(
              { kFn,
                ": cannot load ",
                resolved,
                ": ",
                (load_res.return_count() > 1 ? load_res.get<std::string>(1)
                                             : std::string{ "unknown error" }) });
        }

        chain->push_back(resolved);
        struct chain_pop {
          std::vector<fs::path> *chain;
          ~chain_pop() { chain->pop_back(); }
        } const pop{ chain.get() };

        if (sol::protected_function_result const run{
                chunk.as<sol::protected_function>()() };
            !run.valid()) {
          sol::error const err = run;
          throw_runtime_error({ kFn, ": ", resolved, ": ", err.what() });
        }

        // The superproject reads `sub.PROJECT_ROOT`; a global of that name would hide it.
        if (sol::object const own{ env.raw_get<sol::object>(kProjectRootKey) };
            own.valid() && own.get_type() != sol::type::lua_nil) {
          throw_runtime_error({ kFn,
                                ": ",
                                resolved,
                                " assigns PROJECT_ROOT, which envy sets on its table" });
        }
        scope[kProjectRootKey] = project_root;

        // Remembered for the root-only-globals check once the root chunk has finished,
        // and for the fetch-function lookup into this subproject's BUNDLES.
        sol::table list{ subproject_list(lua_view) };
        list[list.size() + 1] =
            lua_view.create_table_with("path", resolved.string(), "env", env);

        tag_declarations(env, util_normalized_path(resolved));

        ENVY_TRACE(subproject_loaded,
                   "",
                   .path = resolved.string(),
                   .parent = parent.string());
        return env;
      });
}

void lua_envy_subproject_validate_root_globals(sol::state_view lua) {
  for_each_subproject(lua, [&](std::string const &path, sol::table const &env) {
    for (char const *name : kRootOnlyGlobals) {
      sol::object const declared{ env.raw_get<sol::object>(name) };
      if (!declared.valid() || declared.get_type() == sol::type::lua_nil) { continue; }
      // Adopted: the root spliced the very value the subproject declared. Anything else
      // -- unset, or a different value -- means nothing will ever read this one.
      if (declared == sol::object{ lua.globals()[name] }) { continue; }
      throw_runtime_error(
          { kFn,
            ": ",
            path,
            " sets ",
            name,
            ", which is read only from the root manifest; assign it there (e.g. ",
            name,
            " = envy.subproject(...).",
            name,
            ")" });
    }
  });
}

std::vector<sol::table> lua_envy_subproject_bundle_tables(sol::state_view lua) {
  std::vector<sol::table> tables;
  for_each_subproject(lua, [&](std::string const &, sol::table const &env) {
    if (sol::object const b{ env.raw_get<sol::object>("BUNDLES") }; b.is<sol::table>()) {
      tables.push_back(b.as<sol::table>());
    }
  });
  return tables;
}

}  // namespace envy
