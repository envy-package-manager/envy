#include "lua_envy_options.h"

#include "lua_envy.h"
#include "util.h"

#include "nanosemver.h"

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace envy {
namespace {

struct range_constraint {
  enum class op { ge, gt, le, lt, eq };
  op comparison;
  double value;
};

std::vector<range_constraint> parse_numeric_range(std::string_view range_str) {
  std::vector<range_constraint> constraints;
  std::string_view sv{ range_str };

  while (!sv.empty()) {
    while (!sv.empty() && sv.front() == ' ') { sv.remove_prefix(1); }
    if (sv.empty()) { break; }

    range_constraint::op comparison;
    if (sv.starts_with(">=")) {
      comparison = range_constraint::op::ge;
      sv.remove_prefix(2);
    } else if (sv.starts_with(">")) {
      comparison = range_constraint::op::gt;
      sv.remove_prefix(1);
    } else if (sv.starts_with("<=")) {
      comparison = range_constraint::op::le;
      sv.remove_prefix(2);
    } else if (sv.starts_with("<")) {
      comparison = range_constraint::op::lt;
      sv.remove_prefix(1);
    } else if (sv.starts_with("==")) {
      comparison = range_constraint::op::eq;
      sv.remove_prefix(2);
    } else {
      throw_runtime_error({ "invalid numeric range token near '", sv, "'" });
    }

    while (!sv.empty() && sv.front() == ' ') { sv.remove_prefix(1); }

    // Find end of number token
    auto end{ sv.begin() };
    bool has_dot{ false };
    if (end != sv.end() && (*end == '-' || *end == '+')) { ++end; }
    while (end != sv.end() &&
           ((*end >= '0' && *end <= '9') || (*end == '.' && !has_dot))) {
      if (*end == '.') { has_dot = true; }
      ++end;
    }

    std::string_view num_str{ sv.data(), static_cast<size_t>(end - sv.begin()) };
    if (num_str.empty()) {
      throw_runtime_error({ "invalid numeric range token near '", sv, "'" });
    }

    std::string const num_s{ num_str };
    char *end_ptr{ nullptr };
    double const val{ std::strtod(num_s.c_str(), &end_ptr) };
    if (end_ptr != num_s.c_str() + num_s.size()) {
      throw_runtime_error({ "invalid numeric range token '", num_s, "'" });
    }

    constraints.push_back({ comparison, val });
    sv.remove_prefix(static_cast<size_t>(end - sv.begin()));
  }

  return constraints;
}

bool numeric_constraint_satisfied(range_constraint const &c, double val) {
  switch (c.comparison) {
    case range_constraint::op::ge: return val >= c.value;
    case range_constraint::op::gt: return val > c.value;
    case range_constraint::op::le: return val <= c.value;
    case range_constraint::op::lt: return val < c.value;
    case range_constraint::op::eq: return val == c.value;
  }
  return false;
}

char const *sol_type_display_name(sol::type t) {
  switch (t) {
    case sol::type::string: return "string";
    case sol::type::number: return "number";
    case sol::type::boolean: return "boolean";
    case sol::type::table: return "table";
    case sol::type::lua_nil: return "nil";
    case sol::type::function: return "function";
    case sol::type::userdata: return "userdata";
    case sol::type::thread: return "thread";
    case sol::type::lightuserdata: return "lightuserdata";
    default: return "unknown";
  }
}

void validate_type_constraint(std::string const &ctx,
                              std::string const &type_str,
                              sol::object const &value,
                              std::string const &identity) {
  sol::type const actual{ value.get_type() };

  if (type_str == "string") {
    if (actual != sol::type::string) {
      throw_runtime_error({ ctx,
                            " must be type 'string', got ",
                            sol_type_display_name(actual),
                            " for ",
                            identity });
    }
  } else if (type_str == "boolean") {
    if (actual != sol::type::boolean) {
      throw_runtime_error({ ctx,
                            " must be type 'boolean', got ",
                            sol_type_display_name(actual),
                            " for ",
                            identity });
    }
  } else if (type_str == "int") {
    if (actual != sol::type::number) {
      throw_runtime_error({ ctx,
                            " must be type 'int', got ",
                            sol_type_display_name(actual),
                            " for ",
                            identity });
    }
    lua_State *L{ value.lua_state() };
    int const stack_before{ lua_gettop(L) };
    value.push();
    bool const is_int{ lua_isinteger(L, -1) != 0 };
    lua_settop(L, stack_before);
    if (!is_int) {
      throw_runtime_error({ ctx, " must be type 'int', got float for ", identity });
    }
  } else if (type_str == "float") {
    if (actual != sol::type::number) {
      throw_runtime_error({ ctx,
                            " must be type 'float', got ",
                            sol_type_display_name(actual),
                            " for ",
                            identity });
    }
  } else if (type_str == "table") {
    if (actual != sol::type::table) {
      throw_runtime_error({ ctx,
                            " must be type 'table', got ",
                            sol_type_display_name(actual),
                            " for ",
                            identity });
    }
  } else if (type_str == "list") {
    if (actual != sol::type::table) {
      throw_runtime_error({ ctx,
                            " must be type 'list' (sequential array), got ",
                            sol_type_display_name(actual),
                            " for ",
                            identity });
    }
    sol::table const tbl{ value.as<sol::table>() };
    lua_State *L{ value.lua_state() };
    int const stack_before{ lua_gettop(L) };
    value.push();
    size_t const raw_len{ lua_rawlen(L, -1) };
    lua_settop(L, stack_before);
    size_t count{ 0 };
    for (auto const &kv : tbl) {
      (void)kv;
      ++count;
    }
    if (count != raw_len) {
      throw_runtime_error(
          { ctx,
            " must be type 'list' (sequential array), got non-sequential table for ",
            identity });
    }
  } else if (type_str == "semver") {
    if (actual != sol::type::string) {
      throw_runtime_error({ ctx,
                            " must be type 'semver', got ",
                            sol_type_display_name(actual),
                            " for ",
                            identity });
    }
    std::string const val_str{ value.as<std::string>() };
    nsv_semver_t ver{};
    if (nsv_parse(&ver, val_str.data(), val_str.size()) != NSV_RET_OK) {
      throw_runtime_error({ ctx, " '", val_str, "' is not valid semver for ", identity });
    }
  } else {
    throw_runtime_error({ ctx, " has unknown type '", type_str, "' for ", identity });
  }
}

void validate_choices_constraint(std::string const &ctx,
                                 sol::table const &choices,
                                 sol::object const &value,
                                 bool is_list,
                                 std::string const &identity) {
  // Build display string {a, b, c}
  std::ostringstream choices_display;
  choices_display << "{";
  bool first{ true };
  for (auto const &[k, v] : choices) {
    if (!first) { choices_display << ", "; }
    first = false;
    if (v.get_type() == sol::type::string) {
      choices_display << v.as<std::string>();
    } else if (v.get_type() == sol::type::number) {
      choices_display << v.as<double>();
    } else if (v.get_type() == sol::type::boolean) {
      choices_display << (v.as<bool>() ? "true" : "false");
    } else {
      choices_display << "<" << sol_type_display_name(v.get_type()) << ">";
    }
  }
  choices_display << "}";
  std::string const choices_str{ choices_display.str() };

  auto check_one = [&](sol::object const &val,
                       std::string const &val_display,
                       std::string const &element_ctx) {
    for (auto const &[k, choice] : choices) {
      if (val == choice) { return; }
    }
    throw_runtime_error({ element_ctx,
                          " value '",
                          val_display,
                          "' not in ",
                          choices_str,
                          " for ",
                          identity });
  };

  if (is_list) {
    sol::table const tbl{ value.as<sol::table>() };
    int idx{ 0 };
    for (auto const &[k, elem] : tbl) {
      ++idx;
      std::string elem_display;
      if (elem.get_type() == sol::type::string) {
        elem_display = elem.as<std::string>();
      } else if (elem.get_type() == sol::type::number) {
        std::ostringstream oss;
        oss << elem.as<double>();
        elem_display = oss.str();
      } else if (elem.get_type() == sol::type::boolean) {
        elem_display = elem.as<bool>() ? "true" : "false";
      } else {
        elem_display = std::string("<") + sol_type_display_name(elem.get_type()) + ">";
      }

      bool found{ false };
      for (auto const &[ck, choice] : choices) {
        if (elem == choice) {
          found = true;
          break;
        }
      }
      if (!found) {
        throw_runtime_error({ ctx,
                              " element '",
                              elem_display,
                              "' at index ",
                              idx,
                              " not in ",
                              choices_str,
                              " for ",
                              identity });
      }
    }
  } else {
    std::string val_display;
    if (value.get_type() == sol::type::string) {
      val_display = value.as<std::string>();
    } else if (value.get_type() == sol::type::number) {
      std::ostringstream oss;
      oss << value.as<double>();
      val_display = oss.str();
    } else if (value.get_type() == sol::type::boolean) {
      val_display = value.as<bool>() ? "true" : "false";
    } else {
      val_display = std::string("<") + sol_type_display_name(value.get_type()) + ">";
    }
    check_one(value, val_display, ctx);
  }
}

void validate_single_option(std::string const &key,
                            sol::table const &constraint,
                            sol::object const &value,
                            std::string const &identity) {
  auto const ctx{ "option '" + key + "'" };

  // 1. type check
  sol::object type_obj{ constraint["type"] };
  if (type_obj.valid() && type_obj.get_type() != sol::type::lua_nil &&
      type_obj.get_type() != sol::type::string) {
    throw_runtime_error({ ctx, " schema 'type' must be a string for ", identity });
  }
  bool const has_type{ type_obj.valid() && type_obj.get_type() == sol::type::string };
  std::string type_str;
  if (has_type) {
    type_str = type_obj.as<std::string>();
    validate_type_constraint(ctx, type_str, value, identity);
  }

  // 2. range check
  sol::object range_obj{ constraint["range"] };
  if (range_obj.valid() && range_obj.get_type() == sol::type::string) {
    std::string const range_str{ range_obj.as<std::string>() };
    if (has_type && type_str == "semver") {
      // The type check already rejected an unparseable value; should one get here, it
      // misses rather than comparing as the zero-initialized 0.0.0.
      std::string const val_str{ value.as<std::string>() };
      nsv_semver_t ver{};
      bool const parsed{ nsv_parse(&ver, val_str.data(), val_str.size()) == NSV_RET_OK };
      // No flags: a pre-release is a version below its release, npm's includePrerelease
      int const match{ nsv_range_match(range_str.data(), range_str.size(), &ver, 0) };
      if (match < 0) {
        throw_runtime_error(
            { ctx, " has invalid range '", range_str, "' for ", identity });
      }
      if (!parsed || match == 0) {
        throw_runtime_error({ ctx,
                              " '",
                              val_str,
                              "' does not satisfy range '",
                              range_str,
                              "' for ",
                              identity });
      }
    } else {
      // numeric range
      auto constraints{ parse_numeric_range(range_str) };

      double num_val{ 0.0 };
      if (value.get_type() == sol::type::number) {
        num_val = value.as<double>();
      } else if (value.get_type() == sol::type::string) {
        std::string const val_str{ value.as<std::string>() };
        char *end_ptr{ nullptr };
        num_val = std::strtod(val_str.c_str(), &end_ptr);
        if (end_ptr != val_str.c_str() + val_str.size()) {
          throw_runtime_error(
              { ctx, " value '", val_str, "' is not numeric for ", identity });
        }
      } else {
        throw_runtime_error({ ctx, " value is not numeric for ", identity });
      }

      for (auto const &c : constraints) {
        if (!numeric_constraint_satisfied(c, num_val)) {
          std::ostringstream oss;
          oss << ctx << " value " << num_val << " does not satisfy range '" << range_str
              << "' for " << identity;
          throw_runtime_error({ oss.str() });
        }
      }
    }
  }

  // 3. choices check
  sol::object choices_obj{ constraint["choices"] };
  if (choices_obj.valid() && choices_obj.get_type() != sol::type::lua_nil) {
    if (choices_obj.get_type() != sol::type::table) {
      throw_runtime_error({ ctx, " schema 'choices' must be a table for ", identity });
    }
    bool const is_list{ has_type && type_str == "list" };
    validate_choices_constraint(ctx,
                                choices_obj.as<sol::table>(),
                                value,
                                is_list,
                                identity);
  }

  // 4. custom validate function
  sol::object validate_fn_obj{ constraint["validate"] };
  if (validate_fn_obj.valid() && validate_fn_obj.get_type() == sol::type::function) {
    sol::protected_function validate_fn{ validate_fn_obj.as<sol::protected_function>() };
    sol::protected_function_result result{ validate_fn(value) };
    if (!result.valid()) {
      sol::error err = result;
      throw_runtime_error({ ctx, " validation failed: ", err.what(), " for ", identity });
    }
    sol::object ret{ result };
    sol::type const ret_type{ ret.get_type() };
    switch (ret_type) {
      case sol::type::lua_nil: break;
      case sol::type::boolean:
        if (!ret.as<bool>()) {
          throw_runtime_error({ ctx, " validation failed for ", identity });
        }
        break;
      case sol::type::string:
        throw std::runtime_error(ctx + " validation failed: " + ret.as<std::string>() +
                                 " for " + identity);
      default:
        throw std::runtime_error(ctx + " validate must return nil/true/false/string for " +
                                 identity);
    }
  }
}

}  // namespace

void validate_options_schema(sol::table const &schema,
                             sol::object const &opts,
                             std::string const &identity) {
  // opts should be a table (or nil for empty options)
  sol::table opts_table;
  bool const has_opts{ opts.valid() && opts.get_type() == sol::type::table };
  if (has_opts) { opts_table = opts.as<sol::table>(); }

  // Check each schema entry
  for (auto const &[key_obj, constraint_obj] : schema) {
    if (key_obj.get_type() != sol::type::string) { continue; }
    std::string const key{ key_obj.as<std::string>() };

    if (constraint_obj.get_type() != sol::type::table) {
      throw_runtime_error(
          { "OPTIONS schema entry for '", key, "' must be a table for ", identity });
    }
    sol::table const constraint{ constraint_obj.as<sol::table>() };

    // Check required
    bool const required{ constraint["required"].valid() &&
                         constraint["required"].get_type() == sol::type::boolean &&
                         constraint["required"].get<bool>() };

    sol::object value{ has_opts ? opts_table[key] : sol::object(sol::lua_nil) };
    bool const present{ value.valid() && value.get_type() != sol::type::lua_nil };

    if (required && !present) {
      throw_runtime_error({ "option '", key, "' is required for ", identity });
    }

    if (present) { validate_single_option(key, constraint, value, identity); }
  }

  // Reject unknown options
  if (has_opts) {
    for (auto const &[key_obj, value_obj] : opts_table) {
      if (key_obj.get_type() != sol::type::string) { continue; }
      std::string const key{ key_obj.as<std::string>() };

      sol::object schema_entry{ schema[key] };
      if (!schema_entry.valid() || schema_entry.get_type() == sol::type::lua_nil) {
        std::ostringstream oss;
        oss << "unknown option '" << key << "' for " << identity << "; valid options:";
        for (auto const &[sk, sv] : schema) {
          if (sk.get_type() == sol::type::string) { oss << " " << sk.as<std::string>(); }
        }
        throw_runtime_error({ oss.str() });
      }
    }
  }
}

void lua_envy_options_install(sol::table &envy_table) {
  envy_table["options"] = [](sol::this_state ts, sol::table schema) {
    sol::state_view lua{ ts };
    sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

    // Derive identity from globals if available
    std::string identity{ "unknown" };
    sol::object id_obj{ lua.globals()["IDENTITY"] };
    if (id_obj.valid() && id_obj.get_type() == sol::type::string) {
      identity = id_obj.as<std::string>();
    }

    validate_options_schema(schema, opts, identity);
  };
}

}  // namespace envy
