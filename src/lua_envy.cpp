#include "lua_envy.h"

#include "embedded_lua.h"
#include "env_ops.h"
#include "lua_ctx/lua_envy_extract.h"
#include "lua_ctx/lua_envy_fetch.h"
#include "lua_ctx/lua_envy_file_ops.h"
#include "lua_ctx/lua_envy_json.h"
#include "lua_ctx/lua_envy_loadenv_bundle.h"
#include "lua_ctx/lua_envy_loadenv_spec.h"
#include "lua_ctx/lua_envy_module.h"
#include "lua_ctx/lua_envy_options.h"
#include "lua_ctx/lua_envy_package.h"
#include "lua_ctx/lua_envy_path.h"
#include "lua_ctx/lua_envy_product.h"
#include "lua_ctx/lua_envy_run.h"
#include "lua_ctx/lua_envy_toml.h"
#ifdef ENVY_FUNCTIONAL_TESTER
#include "lua_ctx/lua_envy_functional_test.h"
#endif
#include "platform.h"
#include "shell.h"
#include "sol_util.h"
#include "tui.h"
#include "util.h"

#include <sstream>

namespace envy {

void lua_envy_install(sol::state &lua, std::filesystem::path const &project_root) {
  // Override print to route through TUI
  lua["print"] = [](sol::variadic_args va) {
    std::ostringstream oss;
    bool first{ true };
    for (auto arg : va) {
      if (!first) oss << '\t';
      oss << luaL_tolstring(arg.lua_state(), arg.stack_index(), nullptr);
      lua_pop(arg.lua_state(), 1);  // Pop result from luaL_tolstring
      first = false;
    }
    tui::info("%s", oss.str().c_str());
  };

  auto envy_table{ lua.create_table() };  // envy table with logging functions
  envy_table["debug"] = [](std::string_view msg) { tui::debug("%s", msg.data()); };
  envy_table["info"] = [](std::string_view msg) { tui::info("%s", msg.data()); };
  envy_table["warn"] = [](std::string_view msg) { tui::warn("%s", msg.data()); };
  envy_table["error"] = [](std::string_view msg) { tui::error("%s", msg.data()); };
  envy_table["stdout"] = [](std::string_view msg) { tui::print_stdout("%s", msg.data()); };

  // envy.extend (load Lua code)
  sol::protected_function_result extend_result{
    sol_util_run_bytecode(lua, embedded::kLuaExtend)
  };
  if (extend_result.valid()) {
    envy_table["extend"] = extend_result;
  } else {
    sol::error err = extend_result;
    tui::error("Failed to load envy.extend: %s", err.what());
  }

  // envy.template (load Lua code)
  sol::protected_function_result template_result{
    sol_util_run_bytecode(lua, embedded::kLuaTemplate)
  };
  if (template_result.valid()) {
    envy_table["template"] = template_result;
  } else {
    sol::error err = template_result;
    tui::error("Failed to load envy.template: %s", err.what());
  }

  envy_table["EXE_EXT"] = platform::exe_suffix();
  envy_table["PLATFORM"] = platform::os_name();
  envy_table["ARCH"] = platform::arch_name();
  envy_table["PLATFORM_ARCH"] =
      std::string{ platform::os_name() } + "-" + std::string{ platform::arch_name() };
  if (!project_root.empty()) {
    envy_table["PROJECT_ROOT"] = util_normalized_path(project_root);
  }

  // Install module functions
  lua_envy_path_install(envy_table);
  lua_envy_file_ops_install(envy_table);
  lua_envy_run_install(envy_table);
  lua_envy_extract_install(envy_table);
  lua_envy_fetch_install(envy_table);
  lua_envy_package_install(envy_table);
  lua_envy_product_install(envy_table);
  lua_envy_loadenv_spec_install(envy_table);
  lua_envy_loadenv_install(envy_table);
  lua_envy_loadenv_bundle_install_refusal(envy_table);
  lua_envy_options_install(envy_table);
  lua_envy_json_install(envy_table);
  lua_envy_toml_install(envy_table);
#ifdef ENVY_FUNCTIONAL_TESTER
  lua_envy_functional_test_install(envy_table);
#endif

  lua["envy"] = envy_table;

  // Spec and manifest Lua read the environment as it was before any `envy activate`, the
  // same one CI sees, so a shell's activation can never steer what a package builds.
  if (sol::optional<sol::table> os_tbl = lua["os"]) {
    (*os_tbl)["getenv"] = [](std::string const &name) -> sol::optional<std::string> {
      if (auto const *v{ env_get(env_pristine(), name) }) { return *v; }
      return sol::nullopt;
    };
    // Both spawn with the live environment, which can be an activated shell's.
    (*os_tbl)["execute"] = [] {
      throw_runtime_error({ "os.execute is unavailable; use envy.run" });
    };
  }
  if (sol::optional<sol::table> io_tbl = lua["io"]) {
    (*io_tbl)["popen"] = [] {
      throw_runtime_error({ "io.popen is unavailable; use envy.run (capture = true)" });
    };
  }

  // Register all shell constants on all platforms; runtime validation rejects incompatible
  // shells
  sol::table shell_tbl{ lua.create_table_with(
      "BASH",
      static_cast<int>(shell_choice::bash),
      "SH",
      static_cast<int>(shell_choice::sh),
      "CMD",
      static_cast<int>(shell_choice::cmd),
      "POWERSHELL",
      static_cast<int>(shell_choice::powershell)) };
  lua["ENVY_SHELL"] = shell_tbl;
}

}  // namespace envy
