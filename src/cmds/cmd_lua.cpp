#include "cmd_lua.h"

#include "lua_envy.h"
#include "sol_util.h"

#include "cli_parse.h"
#include "sol/sol.hpp"

#include <memory>

namespace envy {

cli_cmd &cmd_lua::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("lua", "Execute Lua script") };
  sub.pos("script", c.script_path, "Lua script file to execute").required().check_file();
  return sub;
}

cmd_lua::cmd_lua(cmd_lua::cfg cfg,
                 std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_lua::execute() {
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua);

  sol::protected_function_result result =
      lua->safe_script_file(cfg_.script_path.string(), sol::script_pass_on_error);
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error(err.what());
  }
}

}  // namespace envy
