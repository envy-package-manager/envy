#pragma once

#include "env_ops.h"
#include "util.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace envy {

// A project's shell activation, computed over `live` with any prior activation undone.
struct activation {
  shell_env_t target;  // the whole environment to switch to, state included
  std::string root;    // canonical manifest dir
  size_t vars{ 0 };    // names it touches
};

// Built-ins (bin dir on PATH, ENVY_PROJECT_ROOT) always apply; installed packages' ENV is
// best-effort over a read-only cache, so a Lua error or uncached spec only warns.
activation activation_compute(std::optional<std::filesystem::path> const &project_dir,
                              std::optional<std::filesystem::path> const &cli_cache_root,
                              shell_env_t const &live);

// `<cache root>/activate/<root digest>`: bumped by every `envy install`, so an activated
// shell in that project re-activates at its next prompt.
std::filesystem::path activation_genfile(std::filesystem::path const &cache_root,
                                         std::filesystem::path const &project_root);
std::string activation_gen_read(std::filesystem::path const &genfile);

// Bumps the project's generation file on scope exit, success or failure: a partial
// install still installed something.
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
