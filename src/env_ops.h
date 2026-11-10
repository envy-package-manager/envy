#pragma once

#include "shell.h"

#include "sol/forward.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace envy {

// One variable's effect, as a spec's ENV declares it: `NAME = "v"` sets, `NAME = false`
// unsets, `NAME = { prepend =, append =, sep = }` wraps the current value.
struct env_set {
  std::string value;
};
struct env_unset {};
struct env_edit {
  std::vector<std::string> prepend;  // outermost first
  std::vector<std::string> append;   // innermost first
  std::string sep;
};
using env_op = std::variant<env_set, env_unset, env_edit>;

struct env_entry {
  std::string name;
  env_op op;
};
using env_ops = std::vector<env_entry>;  // one package's ENV, sorted by name

inline constexpr char kEnvStatePrefix[]{ "ENVY_ACTIVATE_" };
inline constexpr char kEnvStateRoot[]{ "ENVY_ACTIVATE_ROOT" };
inline constexpr char kEnvStateSchema[]{ "1" };

char env_path_sep();  // ':' on POSIX, ';' on Windows

// [A-Za-z_][A-Za-z0-9_]*: the names every shell can assign without quoting.
bool env_name_valid(std::string_view name);

// Windows names are case-insensitive; lookups and writes go through these so a map never
// holds both "Path" and "PATH". A write keeps an existing key's spelling.
std::string const *env_get(shell_env_t const &env, std::string_view name);
void env_put(shell_env_t &env, std::string_view name, std::string value);
void env_erase(shell_env_t &env, std::string_view name);

// Parse an ENV table. Throws naming `context` on a bad name, a reserved ENVY_ name, or a
// value that is not a string, false, or an edit table.
env_ops env_ops_parse(sol::object const &obj, std::string_view context);

// A set replaces, an unset removes, an edit wraps what is there. Layers applied later
// therefore win: their sets replace and their segments land outermost.
void env_ops_apply(shell_env_t &env, env_ops const &ops);

// What an activation recorded beyond the variables it touched.
struct env_state_fields {
  std::string root;     // canonical manifest dir
  std::string exe;      // binary that wrote the state; deactivation runs it
  std::string genfile;  // per-project counter file envy install bumps
  std::string gen;      // its contents at activation
};

// `live` with any activation undone: every recorded name back to its backup (or unset),
// every ENVY_ACTIVATE_ variable removed. Throws on a state schema this envy cannot read.
shell_env_t env_restore(shell_env_t const &live);

// The process's pre-activation environment, captured once. What phases and Lua see.
shell_env_t const &env_pristine();

// `target` plus the state env_restore needs to get back to `base`: the touched names and
// a backup of each one `base` had.
shell_env_t env_with_state(shell_env_t const &base,
                           shell_env_t target,
                           env_state_fields const &fields);

// Names whose value differs between `a` and `b`, the ENVY_ACTIVATE_ state excluded.
std::vector<std::string> env_changed_names(shell_env_t const &a, shell_env_t const &b);

enum class env_shell { BASH, ZSH, FISH, POWERSHELL };
std::optional<env_shell> env_shell_parse(std::string_view name);

// A script that, evaluated by `sh`, turns environment `from` into `to`.
std::string env_script(env_shell sh, shell_env_t const &from, shell_env_t const &to);

}  // namespace envy
