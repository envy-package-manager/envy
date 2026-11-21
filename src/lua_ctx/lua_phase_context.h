#pragma once

#include "cache.h"
#include "util.h"

#include "sol/sol.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace envy {

// Forward declarations
class engine;
struct pkg;

// Phase execution context - all state available to envy.* functions during phase
// execution. Stored in Lua registry, accessed via sol::this_state.
struct phase_context {
  engine *eng;
  pkg *p;
  std::optional<std::filesystem::path> run_dir;  // default cwd for envy.run()

  // May not be the same as p->lock: "custom fetch" runs with child package's lock!
  cache::scoped_entry_lock const *lock;

  // Lua running in the manifest state (manifest bundle fetch, depot FETCH, the
  // DEFAULT_SHELL function itself): envy.run gets the platform built-in. That work
  // runs before the manifest shell can exist, and the manifest state is one
  // non-recursive mutex — evaluating the shell from inside it would self-deadlock.
  bool builtin_shell{ false };

  // `root` for dependencies' ENV in this phase: the package's tmp dir. Unset falls back to
  // lock's tmp dir; with neither, only the pre-activation env applies.
  std::optional<std::filesystem::path> env_root;
};

// Get phase context from Lua registry (returns nullptr if not in phase execution)
phase_context const *lua_phase_context_get(sol::this_state ts);

// A relative path from Lua, anchored to the phase's run_dir, else its stage_dir, else cwd.
std::filesystem::path lua_phase_context_resolve(sol::this_state ts,
                                                std::filesystem::path const &path);

// Whole-file IO for envy.* functions; paths resolve as above, errors name `fn` and the
// path. A write creates parent directories and replaces the file atomically, exec bit
// included.
std::vector<unsigned char> lua_phase_context_read_file(sol::this_state ts,
                                                       std::string_view path,
                                                       char const *fn);
void lua_phase_context_write_file(sol::this_state ts,
                                  std::string_view path,
                                  std::string_view content,
                                  char const *fn,
                                  bool executable = false);

// RAII guard that sets Lua registry context for phase execution scope.
// Automatically clears context on destruction (including exception unwinding).
// The guard owns the context struct on the stack; registry stores pointer to it.
// Caller passes the lua_State explicitly and must hold the owning package's
// sol_state_guard accessor for the guard's full lifetime.
class phase_context_guard : unmovable {
 public:
  phase_context_guard(engine *eng,
                      pkg *p,
                      lua_State *L,
                      std::optional<std::filesystem::path> run_dir = std::nullopt,
                      cache::scoped_entry_lock const *lock = nullptr,
                      std::optional<std::filesystem::path> env_root = std::nullopt);
  ~phase_context_guard();

 private:
  lua_State *lua_state_;
  phase_context ctx_;
};

}  // namespace envy
