#pragma once

#include "env_ops.h"
#include "pkg_phase.h"

#include <filesystem>
#include <optional>

namespace envy {

struct pkg;
struct phase_context;

// Where a cache-managed package's payload lands, without touching the cache: the hash and
// label check will use, as product prediction does.
std::filesystem::path pkg_predicted_path(pkg *p);

// p's ENV for one consumer (nullopt: none declared). `root` holds the consumer's mutable
// tool state: the project root in a shell, the dependent's tmp dir in a build.
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
