#include "sol_util.h"

#include "embedded_lua.h"
#include "util.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

sol::protected_function_result sol_util_run_bytecode(sol::state_view lua,
                                                     std::span<unsigned char const> bytecode) {
  return lua.safe_script(std::string_view{ reinterpret_cast<char const *>(bytecode.data()),
                                           bytecode.size() },
                         sol::script_pass_on_error,
                         "=bytecode",  // unused: the bytecode carries its own chunk name
                         sol::load_mode::binary);
}

sol_state_ptr sol_util_make_lua_state() {
  auto lua{ std::make_unique<sol::state>() };
  lua->open_libraries(sol::lib::base,
                      sol::lib::package,
                      sol::lib::coroutine,
                      sol::lib::string,
                      sol::lib::os,
                      sol::lib::math,
                      sol::lib::table,
                      sol::lib::debug,
                      sol::lib::bit32,
                      sol::lib::io);

  if (sol::protected_function_result const r{
          sol_util_run_bytecode(*lua, embedded::kLuaPrelude) };
      !r.valid()) {
    throw_runtime_error({ "Lua prelude: ", r.get<sol::error>().what() });
  }

  return lua;
}

void sol_util_throw_wrong_type(std::string_view context,
                               std::string_view key,
                               std::string_view expected) {
  throw_runtime_error({ context, ": ", key, " must be a ", expected });
}

void sol_util_throw_missing(std::string_view context, std::string_view key) {
  throw_runtime_error({ context, ": ", key, " is required" });
}

std::string sol_util_dump_table(sol::table const &tbl) {
  std::string result{ "{" };
  bool first{ true };
  for (auto const &pair : tbl) {
    if (!first) { result += ", "; }
    first = false;

    if (pair.first.is<std::string>()) {
      result += pair.first.as<std::string>();
    } else if (pair.first.is<int>()) {
      result += "[" + std::to_string(pair.first.as<int>()) + "]";
    } else {
      result += "?";
    }

    result += "=";
    if (pair.second.is<std::string>()) {
      std::string val{ pair.second.as<std::string>() };
      if (val.size() > 40) { val = val.substr(0, 37) + "..."; }
      result += "\"" + val + "\"";
    } else if (pair.second.is<sol::table>()) {
      result += "{...}";
    } else {
      result += sol::type_name(pair.first.lua_state(), pair.second.get_type());
    }
  }
  result += "}";
  return result;
}

std::vector<std::string> sol_util_get_string_list(sol::table const &table,
                                                  std::string_view key,
                                                  std::string_view context) {
  auto const list{ sol_util_get_optional<sol::table>(table, key, context) };
  if (!list) { return {}; }

  std::vector<std::string> values;
  values.reserve(list->size());
  for (std::size_t i{ 1 }; i <= list->size(); ++i) {
    sol::object const elem{ (*list)[i] };
    if (!elem.is<std::string>() || elem.as<std::string>().empty()) {
      throw_runtime_error({ context, ": ", key, "[", i, "] must be a non-empty string" });
    }
    values.push_back(elem.as<std::string>());
  }
  return values;
}

std::string_view sol_util_string_arg(sol::stack_object arg, std::string_view fn) {
  lua_State *const L{ arg.lua_state() };
  int const type{ lua_type(L, arg.stack_index()) };
  if (type != LUA_TSTRING) {
    throw_runtime_error({ fn, ": expected a string, got ", lua_typename(L, type) });
  }
  std::size_t len;
  char const *const s{ lua_tolstring(L, arg.stack_index(), &len) };
  return { s, len };
}

void sol_util_reject_unknown_keys(sol::table const &table,
                                  std::span<std::string_view const> allowed,
                                  std::string_view context) {
  for (auto const &[key, value] : table) {
    if (!key.is<std::string>() || value.get_type() == sol::type::lua_nil) { continue; }
    std::string const name{ key.as<std::string>() };
    if (std::ranges::find(allowed, name) != allowed.end()) { continue; }

    std::string list;
    for (auto const &a : allowed) {
      if (!list.empty()) { list += ", "; }
      list += a;
    }
    throw_runtime_error(
        { context, ": unknown key '", name, "'; allowed keys are ", list });
  }
}

}  // namespace envy
