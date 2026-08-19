#include "sol_util.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

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

  // Override error() and assert() to automatically include stack traces
  lua->script(R"lua(
do
  local orig_error = error
  local orig_assert = assert

  _G.error = function(message, level)
    level = (level or 1) + 1
    return orig_error(debug.traceback(tostring(message), level), 0)
  end

  _G.assert = function(condition, message, ...)
    if not condition then
      message = message or "assertion failed"
      return orig_assert(false, debug.traceback(tostring(message), 2))
    end
    return condition, message, ...
  end
end
)lua");

  return lua;
}

void sol_util_throw_wrong_type(std::string_view context,
                               std::string_view key,
                               std::string_view expected) {
  throw std::runtime_error(std::string(context) + ": " + std::string(key) +
                           " must be a " + std::string(expected));
}

void sol_util_throw_missing(std::string_view context, std::string_view key) {
  throw std::runtime_error(std::string(context) + ": " + std::string(key) +
                           " is required");
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
      throw std::runtime_error(std::string(context) + ": " + std::string(key) + "[" +
                               std::to_string(i) + "] must be a non-empty string");
    }
    values.push_back(elem.as<std::string>());
  }
  return values;
}

}  // namespace envy
