#pragma once

#include "env_ops.h"
#include "pkg_phase.h"

#include "sol/forward.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace envy {

struct pkg;
struct phase_context;

// Where a cache-managed package's payload lands, without touching the cache: the hash and
// label check will use, as product prediction does.
std::filesystem::path pkg_predicted_path(pkg *p);

// p's mutable state for ENV and command functions: root/.envy/state/<ns.name>, keyed sans
// revision. root is the project in a shell, a scratch dir in a phase.
std::filesystem::path pkg_state_dir(pkg const *p, std::filesystem::path const &root);

// The root a phase's dependency ENV and pkg_state_dir hang from: its env_root, else its
// lock's tmp dir; nullopt when it has neither, or names no package.
std::optional<std::filesystem::path> pkg_env_root(phase_context const *ctx);

// Calls `fn`, ENV's or a command function, as both are called: (pkg_dir -- nil when
// user-managed --, pkg_state_dir under `root`, a fresh copy of the options). Lua locked.
sol::object pkg_env_call(pkg *p,
                         sol::state_view lua,
                         sol::object const &fn,
                         std::string_view what,
                         std::optional<std::filesystem::path> const &pkg_path,
                         std::filesystem::path const &root);

// p's ENV for one consumer (nullopt: none declared), its pkg_state_dir under `root`.
std::optional<env_ops> pkg_env_eval(pkg *p,
                                    std::optional<std::filesystem::path> const &pkg_path,
                                    std::filesystem::path const &root);

// What p's verbs see at `phase`: the pre-activation env plus the ENV of everything behind
// p's edges needed by `phase`, deepest first so nearer dependencies win (ties by key).
shell_env_t pkg_env_for_phase(pkg *p, pkg_phase phase, std::filesystem::path const &root);

// envy.run's base env: pkg_env_for_phase at the context's phase and root (builtin_shell
// or not), or the pre-activation env when the context names no package or no root.
shell_env_t pkg_env_for_context(phase_context const *ctx);

}  // namespace envy
