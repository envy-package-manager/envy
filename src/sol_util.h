#pragma once

#include "sol/sol.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace envy {

using sol_state_ptr = std::unique_ptr<sol::state>;
sol_state_ptr sol_util_make_lua_state();  // with std libs

// Owns a Lua state plus the mutex serializing access to it. lock() is the only path
// to the state, so unsynchronized cross-thread access is structurally impossible.
// Policy: acquire the accessor once at the entry point of any Lua interaction, hold
// it for the interaction's full extent (including protected_function calls), and
// pass sol views/objects down the call stack — never re-lock deeper in.
class sol_state_guard {
 public:
  class accessor {
   public:
    accessor(std::mutex &m, sol::state *s) : lock_{ m }, state_{ s } {}
    explicit operator bool() const { return state_ != nullptr; }
    sol::state &operator*() const { return *state_; }
    sol::state *operator->() const { return state_; }

   private:
    std::unique_lock<std::mutex> lock_;
    sol::state *state_;
  };

  sol_state_guard() = default;
  sol_state_guard(std::nullptr_t) {}
  sol_state_guard(sol_state_ptr state) : state_{ std::move(state) } {}

  accessor lock() const { return { mutex_, state_.get() }; }

  void set(sol_state_ptr state) {
    std::lock_guard const l{ mutex_ };
    state_ = std::move(state);
  }

 private:
  mutable std::mutex mutex_;
  sol_state_ptr state_;
};

namespace detail {

template <typename T>
constexpr std::string_view type_name_for_error() {
  if constexpr (std::is_same_v<T, bool>) {
    return "boolean";
  } else if constexpr (std::is_same_v<T, std::string>) {
    return "string";
  } else if constexpr (std::is_same_v<T, sol::table>) {
    return "table";
  } else if constexpr (std::is_same_v<T, sol::protected_function> ||
                       std::is_same_v<T, sol::function>) {
    return "function";
  } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
    return "number";
  } else {
    return "value";
  }
}

}  // namespace detail

// Render a table as "{k=v, ...}" for diagnostics; string values over 40 chars elide.
std::string sol_util_dump_table(sol::table const &tbl);

// Out of line so the message building never lands in a template instantiation.
[[noreturn]] void sol_util_throw_wrong_type(std::string_view context,
                                            std::string_view key,
                                            std::string_view expected);
[[noreturn]] void sol_util_throw_missing(std::string_view context, std::string_view key);

template <typename T>
std::optional<T> sol_util_get_optional(sol::table const &table,
                                       std::string_view key,
                                       std::string_view context) {
  sol::optional<sol::object> obj = table[key];
  if (!obj || !obj->valid() || obj->get_type() == sol::type::lua_nil) {
    return std::nullopt;
  }

  if (!obj->is<T>()) {
    sol_util_throw_wrong_type(context, key, detail::type_name_for_error<T>());
  }

  return obj->as<T>();
}

template <typename T>
T sol_util_get_required(sol::table const &table,
                        std::string_view key,
                        std::string_view context) {
  sol::optional<sol::object> obj = table[key];
  if (!obj || !obj->valid() || obj->get_type() == sol::type::lua_nil) {
    sol_util_throw_missing(context, key);
  }

  if (!obj->is<T>()) {
    sol_util_throw_wrong_type(context, key, detail::type_name_for_error<T>());
  }

  return obj->as<T>();
}

template <typename T>
T sol_util_get_or_default(sol::table const &table,
                          std::string_view key,
                          T const &default_value,
                          std::string_view context) {
  auto opt{ sol_util_get_optional<T>(table, key, context) };
  return opt.value_or(default_value);
}

// Read an optional array-of-strings field; empty when the key is absent. Throws when
// the value isn't a table or holds anything but non-empty strings.
std::vector<std::string> sol_util_get_string_list(sol::table const &table,
                                                  std::string_view key,
                                                  std::string_view context);

}  // namespace envy
