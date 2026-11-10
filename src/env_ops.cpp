#include "env_ops.h"

#include "sol_util.h"
#include "util.h"

#include "sol/sol.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace envy {

namespace {

constexpr char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

bool names_equal(std::string_view a, std::string_view b) {
#ifdef _WIN32
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return ascii_lower(x) == ascii_lower(y);
         });
#else
  return a == b;
#endif
}

bool name_less(std::string_view a, std::string_view b) {
#ifdef _WIN32
  return std::lexicographical_compare(
      a.begin(),
      a.end(),
      b.begin(),
      b.end(),
      [](char x, char y) { return ascii_lower(x) < ascii_lower(y); });
#else
  return a < b;
#endif
}

bool is_state_name(std::string_view name) {
  std::string_view const prefix{ kEnvStatePrefix };
  return name.size() >= prefix.size() &&
         names_equal(name.substr(0, prefix.size()), prefix);
}

std::string state_name(std::string_view suffix) {
  return std::string{ kEnvStatePrefix } + std::string{ suffix };
}

std::string orig_name(std::string_view name) {
  return state_name("ORIG__") + std::string{ name };
}

template <typename Map>
auto find_name(Map &env, std::string_view name) {
#ifdef _WIN32
  return std::find_if(env.begin(), env.end(), [name](auto const &kv) {
    return names_equal(kv.first, name);
  });
#else
  return env.find(std::string{ name });
#endif
}

// A NUL ends a C string: the child would see the value cut short there.
std::string no_nul(std::string s, std::string_view context) {
  if (s.find('\0') != std::string::npos) {
    throw_runtime_error({ context, ": value contains a NUL byte" });
  }
  return s;
}

std::vector<std::string> segments(sol::object const &obj,
                                  std::string_view context,
                                  std::string_view field) {
  std::vector<std::string> out;
  auto const add{ [&](sol::object const &v) {
    if (!v.is<std::string>()) {
      throw_runtime_error({ context, ": ", field, " entries must be strings" });
    }
    if (auto s{ no_nul(v.as<std::string>(), context) }; !s.empty()) {
      out.push_back(std::move(s));
    }
  } };

  if (!obj.valid() || obj.get_type() == sol::type::lua_nil) { return out; }
  if (obj.get_type() != sol::type::table) {
    add(obj);
    return out;
  }

  // pairs skips nil holes, so `{ a, maybe_nil, b }` keeps a and b in order.
  std::vector<std::pair<long long, sol::object>> items;
  for (auto const &[k, v] : obj.as<sol::table>()) {
    if (k.get_type() != sol::type::number) {
      throw_runtime_error(
          { context, ": ", field, " must be a string or a list of strings" });
    }
    items.emplace_back(k.as<long long>(), v);
  }
  util_sort(items, [](auto const &a, auto const &b) { return a.first < b.first; });
  for (auto const &[_, v] : items) { add(v); }
  return out;
}

std::optional<env_op> parse_value(sol::object const &v, std::string const &context) {
  switch (v.get_type()) {
    case sol::type::string: return env_set{ no_nul(v.as<std::string>(), context) };
    case sol::type::boolean:
      if (v.as<bool>()) {
        throw_runtime_error(
            { context, ": true is not a value; use a string, or false to unset" });
      }
      return env_unset{};
    case sol::type::table: {
      sol::table const tbl{ v.as<sol::table>() };
      static constexpr std::array<std::string_view, 3> kKeys{ "append", "prepend", "sep" };
      sol_util_reject_unknown_keys(tbl, kKeys, context);
      env_edit edit{ .prepend = segments(tbl["prepend"], context, "prepend"),
                     .append = segments(tbl["append"], context, "append"),
                     .sep = std::string(1, env_path_sep()) };
      if (sol::object const sep{ tbl["sep"] };
          sep.valid() && sep.get_type() != sol::type::lua_nil) {
        if (!sep.is<std::string>() || sep.as<std::string>().empty()) {
          throw_runtime_error({ context, ": sep must be a non-empty string" });
        }
        edit.sep = no_nul(sep.as<std::string>(), context);
      }
      if (edit.prepend.empty() && edit.append.empty()) { return std::nullopt; }  // no-op
      return edit;
    }
    default:
      throw_runtime_error(
          { context, ": value must be a string, false, or { prepend, append, sep }" });
  }
}

void append_quoted_posix(std::string &out, std::string_view v) {
  out += '\'';
  for (char const c : v) {
    out += (c == '\'') ? std::string_view{ "'\\''" } : std::string_view{ &c, 1 };
  }
  out += '\'';
}

void append_quoted_fish(std::string &out, std::string_view v) {
  out += '\'';
  for (char const c : v) {
    if (c == '\'' || c == '\\') { out += '\\'; }
    out += c;
  }
  out += '\'';
}

// PowerShell treats U+2018..U+201B as single quotes too; each doubles to escape.
void append_quoted_pwsh(std::string &out, std::string_view v) {
  out += '\'';
  for (size_t i{ 0 }; i < v.size(); ++i) {
    if (v[i] == '\'') {
      out += "''";
    } else if (i + 2 < v.size() && v.substr(i, 2) == "\xE2\x80" &&
               static_cast<unsigned char>(v[i + 2]) >= 0x98 &&
               static_cast<unsigned char>(v[i + 2]) <= 0x9B) {
      out.append(v.substr(i, 3)).append(v.substr(i, 3));
      i += 2;
    } else {
      out += v[i];
    }
  }
  out += '\'';
}

// Every name whose value differs between `a` and `b`, sorted; state included on request.
std::vector<std::string> changed(shell_env_t const &a,
                                 shell_env_t const &b,
                                 bool with_state) {
  std::vector<std::string> out;
  auto const differs{
    [](shell_env_t const &x, shell_env_t const &y, std::string const &n) {
      auto const *xv{ env_get(x, n) };
      auto const *yv{ env_get(y, n) };
      return (xv == nullptr) != (yv == nullptr) || (xv && *xv != *yv);
    }
  };
  for (auto const &[name, _] : a) {
    if ((with_state || !is_state_name(name)) && differs(a, b, name)) {
      out.push_back(name);
    }
  }
  for (auto const &[name, _] : b) {
    if ((with_state || !is_state_name(name)) && !env_get(a, name)) { out.push_back(name); }
  }
  util_sort(out,
            [](std::string const &x, std::string const &y) { return name_less(x, y); });
  return out;
}

}  // namespace

char env_path_sep() {
#ifdef _WIN32
  return ';';
#else
  return ':';
#endif
}

bool env_name_valid(std::string_view name) {
  if (name.empty() || !(util_ascii_is_alpha(name[0]) || name[0] == '_')) { return false; }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return util_ascii_is_alnum(c) || c == '_';
  });
}

std::string const *env_get(shell_env_t const &env, std::string_view name) {
  auto const it{ find_name(env, name) };
  return it == env.end() ? nullptr : &it->second;
}

void env_put(shell_env_t &env, std::string_view name, std::string value) {
  if (auto const it{ find_name(env, name) }; it != env.end()) {
    it->second = std::move(value);
  } else {
    env.emplace(std::string{ name }, std::move(value));
  }
}

void env_erase(shell_env_t &env, std::string_view name) {
  if (auto const it{ find_name(env, name) }; it != env.end()) { env.erase(it); }
}

env_ops env_ops_parse(sol::object const &obj, std::string_view context) {
  if (obj.get_type() != sol::type::table) {
    throw_runtime_error({ context, ": ENV must be a table" });
  }

  env_ops ops;
  for (auto const &[k, v] : obj.as<sol::table>()) {
    if (k.get_type() != sol::type::string) {
      throw_runtime_error({ context, ": ENV keys must be variable names" });
    }
    std::string name{ k.as<std::string>() };
    std::string const where{ std::string{ context } + ": ENV." + name };
    if (!env_name_valid(name)) {
      throw_runtime_error(
          { where, " is not a valid variable name ([A-Za-z_][A-Za-z0-9_]*)" });
    }
    if (names_equal(std::string_view{ name }.substr(0, std::min<size_t>(5, name.size())),
                    "ENVY_")) {
      throw_runtime_error({ where, ": names starting with ENVY_ are reserved for envy" });
    }
    if (auto op{ parse_value(v, where) }) {
      ops.push_back({ .name = std::move(name), .op = std::move(*op) });
    }
  }

  util_sort(ops, [](env_entry const &a, env_entry const &b) {
    return name_less(a.name, b.name);
  });
  for (size_t i{ 1 }; i < ops.size(); ++i) {
    if (names_equal(ops[i - 1].name, ops[i].name)) {
      throw_runtime_error({ context,
                            ": ENV names '",
                            ops[i - 1].name,
                            "' and '",
                            ops[i].name,
                            "' are the same variable" });
    }
  }
  return ops;
}

void env_ops_apply(shell_env_t &env, env_ops const &ops) {
  for (auto const &e : ops) {
    envy::visit(match{ [&](env_set const &s) { env_put(env, e.name, s.value); },
                       [&](env_unset const &) { env_erase(env, e.name); },
                       [&](env_edit const &ed) {
                         std::string out;
                         auto const add{ [&](std::string_view s) {
                           if (s.empty()) { return; }
                           if (!out.empty()) { out += ed.sep; }
                           out += s;
                         } };
                         for (auto const &s : ed.prepend) { add(s); }
                         if (auto const *cur{ env_get(env, e.name) }) { add(*cur); }
                         for (auto const &s : ed.append) { add(s); }
                         if (!out.empty()) { env_put(env, e.name, std::move(out)); }
                       } },
                e.op);
  }
}

shell_env_t env_restore(shell_env_t const &live) {
  shell_env_t out{ live };
  if (std::string const *vars{ env_get(live, state_name("VARS")) }) {
    std::string const *schema{ env_get(live, state_name("SCHEMA")) };
    if (!schema || *schema != kEnvStateSchema) {
      throw_runtime_error({ "activation state schema '",
                            schema ? *schema : std::string{},
                            "' was written by a different envy; restart your shell" });
    }
    std::string_view rest{ *vars };
    while (!rest.empty()) {
      size_t const colon{ rest.find(':') };
      std::string_view const name{ rest.substr(0, colon) };
      rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
      if (name.empty()) { continue; }
      if (auto const *orig{ env_get(live, orig_name(name)) }) {
        env_put(out, name, *orig);
      } else {
        env_erase(out, name);
      }
    }
  }
  std::erase_if(out, [](auto const &kv) { return is_state_name(kv.first); });
  return out;
}

shell_env_t const &env_pristine() {
  static shell_env_t const pristine{ env_restore(shell_getenv()) };
  return pristine;
}

std::vector<std::string> env_changed_names(shell_env_t const &a, shell_env_t const &b) {
  return changed(a, b, false);
}

shell_env_t env_with_state(shell_env_t const &base,
                           shell_env_t target,
                           env_state_fields const &fields) {
  std::string vars;
  for (auto const &name : env_changed_names(base, target)) {
    if (auto const *v{ env_get(base, name) }) { env_put(target, orig_name(name), *v); }
    if (!vars.empty()) { vars += ':'; }
    vars += name;
  }
  env_put(target, state_name("SCHEMA"), kEnvStateSchema);
  env_put(target, state_name("VARS"), std::move(vars));
  env_put(target, state_name("ROOT"), fields.root);
  env_put(target, state_name("EXE"), fields.exe);
  env_put(target, state_name("GENFILE"), fields.genfile);
  env_put(target, state_name("GEN"), fields.gen);
  return target;
}

std::optional<env_shell> env_shell_parse(std::string_view name) {
  if (name == "bash") { return env_shell::BASH; }
  if (name == "zsh") { return env_shell::ZSH; }
  if (name == "fish") { return env_shell::FISH; }
  if (name == "powershell") { return env_shell::POWERSHELL; }
  return std::nullopt;
}

std::string env_script(env_shell sh, shell_env_t const &from, shell_env_t const &to) {
  std::string out;
  for (auto const &name : changed(from, to, true)) {
    if (!env_name_valid(name)) {
      throw_runtime_error({ "activation: cannot emit variable '", name, "'" });
    }
    std::string const *v{ env_get(to, name) };
    switch (sh) {
      case env_shell::BASH:
      case env_shell::ZSH:
        if (v) {
          out.append("export ").append(name).append("=");
          append_quoted_posix(out, *v);
        } else {
          out.append("unset ").append(name);
        }
        break;
      case env_shell::FISH:
        if (!v) {
          out.append("set -e -g ").append(name);
        } else if (name.ends_with("PATH")) {  // fish keeps *PATH variables as lists
          out.append("set -gx ").append(name);
          std::string_view rest{ *v };
          while (true) {
            size_t const colon{ rest.find(':') };
            out += ' ';
            append_quoted_fish(out, rest.substr(0, colon));
            if (colon == std::string_view::npos) { break; }
            rest.remove_prefix(colon + 1);
          }
        } else {
          out.append("set -gx ").append(name).append(" ");
          append_quoted_fish(out, *v);
        }
        break;
      case env_shell::POWERSHELL:
        if (v) {
          out.append("$env:").append(name).append(" = ");
          append_quoted_pwsh(out, *v);
        } else {
          out.append("Remove-Item -LiteralPath 'Env:")
              .append(name)
              .append("' -ErrorAction SilentlyContinue");
        }
        break;
    }
    out += '\n';
  }
  return out;
}

}  // namespace envy
