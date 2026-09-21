#include "lua_envy.h"

#include "lua_ctx/lua_envy_extract.h"
#include "lua_ctx/lua_envy_fetch.h"
#include "lua_ctx/lua_envy_file_ops.h"
#include "lua_ctx/lua_envy_loadenv_spec.h"
#include "lua_ctx/lua_envy_module.h"
#include "lua_ctx/lua_envy_options.h"
#include "lua_ctx/lua_envy_package.h"
#include "lua_ctx/lua_envy_path.h"
#include "lua_ctx/lua_envy_product.h"
#include "lua_ctx/lua_envy_run.h"
#include "platform.h"
#include "shell.h"
#include "tui.h"

#include <sstream>

namespace envy {
namespace {

constexpr char kEnvyExtendLua[] = R"lua(
return function(target, ...)
  if type(target) ~= "table" then
    error("envy.extend: first argument must be a table", 2)
  end
  for i = 1, select("#", ...) do
    local list = select(i, ...)
    if type(list) ~= "table" then
      error("envy.extend: argument " .. (i + 1) .. " must be a table", 2)
    end
    for _, item in ipairs(list) do
      target[#target + 1] = item
    end
  end
  return target
end
)lua";

constexpr char kEnvyTemplateLua[] = R"lua(
return function(str, values)
  if type(str) ~= "string" then
    error("envy.template: first argument must be a string", 2)
  end
  if type(values) ~= "table" then
    error("envy.template: second argument must be a table", 2)
  end

  local function normalize_key(raw)
    local trimmed = raw:match("^%s*(.-)%s*$")
    if not trimmed or trimmed == "" then
      error("envy.template: placeholder cannot be empty", 2)
    end
    if not trimmed:match("^[%a_][%w_]*$") then
      error("envy.template: placeholder '" .. trimmed .. "' contains invalid characters", 2)
    end
    return trimmed
  end

  local function ensure_pairs(str)
    local open_count = 0
    local i = 1
    while i <= #str do
      if str:sub(i, i+1) == "{{" then
        open_count = open_count + 1
        i = i + 2
      elseif str:sub(i, i+1) == "}}" then
        open_count = open_count - 1
        if open_count < 0 then
          error("envy.template: unmatched '}}' at position " .. i, 2)
        end
        i = i + 2
      else
        i = i + 1
      end
    end
    if open_count > 0 then
      error("envy.template: unmatched '{{' (missing closing '}}')", 2)
    end
  end

  ensure_pairs(str)

  local function replacer(token)
    local key = normalize_key(token)
    local value = values[key]
    if value == nil then
      error("envy.template: missing value for placeholder '" .. key .. "'", 2)
    end
    return tostring(value)
  end

  return (str:gsub("{{(.-)}}", replacer))
end
)lua";

}  // namespace

void lua_envy_install(sol::state &lua) {
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
    lua.safe_script(kEnvyExtendLua, sol::script_pass_on_error)
  };
  if (extend_result.valid()) {
    envy_table["extend"] = extend_result;
  } else {
    sol::error err = extend_result;
    tui::error("Failed to load envy.extend: %s", err.what());
  }

  // envy.template (load Lua code)
  sol::protected_function_result template_result{
    lua.safe_script(kEnvyTemplateLua, sol::script_pass_on_error)
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
  lua_envy_options_install(envy_table);

  lua["envy"] = envy_table;

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
