#pragma once

#include "env_ops.h"
#include "util.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace envy {

class engine;
struct manifest;
struct pkg;

// A project's shell activation, computed over `live` with any prior activation undone.
struct activation {
  shell_env_t target;  // the whole environment to switch to, state included
  std::string root;    // canonical manifest dir
  size_t vars{ 0 };    // names it touches
};

// Which project, and the generation an activation would record -- read now, so a later
// install leaves it stale and the next consumer refreshes.
struct activation_anchor {
  std::filesystem::path root;  // canonical manifest dir
  std::filesystem::path genfile;
  std::string gen;
};
activation_anchor activation_anchor_read(std::filesystem::path const &cache_root,
                                         std::filesystem::path const &manifest_path);

// Built-ins (bin dir on PATH, ENVY_PROJECT_ROOT) always apply; installed packages' ENV is
// best-effort over a read-only cache. reuse_current: an already-current `live` is kept.
activation activation_compute(std::optional<std::filesystem::path> const &project_dir,
                              std::optional<std::filesystem::path> const &cli_cache_root,
                              shell_env_t const &live,
                              bool reuse_current = false);

// The same activation, from an engine that has resolved m's PACKAGES already: one graph,
// not two. An ENV that throws in `strict`'s closure propagates; elsewhere it warns.
activation activation_from_engine(engine const &eng,
                                  manifest const &m,
                                  activation_anchor const &anchor,
                                  shell_env_t const &live,
                                  pkg *strict);

// `<cache root>/activate/<root digest>`: bumped by every package that does work and every
// `envy install`, so an activated shell in that project re-activates at its next prompt.
std::filesystem::path activation_genfile(std::filesystem::path const &cache_root,
                                         std::filesystem::path const &project_root);
std::string activation_gen_read(std::filesystem::path const &genfile);
void activation_gen_bump(std::filesystem::path const &genfile);  // best-effort

// Bumps the project's generation file on scope exit, even if every package was a cache
// hit: an explicit install is how a shell picks up manifest and ENV edits.
class activation_refresh_guard : unmovable {
 public:
  activation_refresh_guard(std::filesystem::path const &cache_root,
                           std::filesystem::path const &manifest_path);
  ~activation_refresh_guard();

 private:
  std::filesystem::path genfile_;
};

// The shell `cmd` (activate/deactivate) emits for; Windows supports only powershell.
env_shell activation_shell(std::string_view name, std::string_view cmd);
bool activation_stdout_is_tty();
std::string activation_eval_hint(std::string_view cmd,
                                 std::string_view shell);  // eval "$(…)"

// "envy: entering foo — 3 vars"-style status lines, silenced by
// ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE.
void activation_announce(char const *verb,
                         std::string const &root,
                         std::string const &tail);

}  // namespace envy
