#include "cmd_run.h"

#include "activation.h"
#include "env_ops.h"
#include "manifest.h"
#include "platform.h"
#include "reexec.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace envy {

namespace fs = std::filesystem;

cli_cmd &cmd_run::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("run", "Run a command with envy bin dir on PATH") };
  sub.flag("--activate", c.activate, "Run with the project's activated environment");
  sub.prefix_command(c.command);
  return sub;
}

cmd_run::cmd_run(cmd_run::cfg cfg, std::optional<fs::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_run::execute() {
  if (cfg_.command.empty()) { throw_runtime_error({ "run: no command specified" }); }

  // Determine start_dir for manifest discovery and build the exec command.
  // If `--` is present, the next arg is the script path used to anchor discovery.
  fs::path start_dir;
  std::vector<std::string> exec_command;

  auto const sentinel{ std::find(cfg_.command.begin(), cfg_.command.end(), "--") };
  if (sentinel != cfg_.command.end()) {
    auto const sentinel_pos{ static_cast<size_t>(
        std::distance(cfg_.command.begin(), sentinel)) };
    if (sentinel_pos + 1 >= cfg_.command.size()) {
      throw_runtime_error({ "run: '--' must be followed by a script path" });
    }
    auto const script_path{ fs::absolute(cfg_.command[sentinel_pos + 1]) };
    if (!fs::exists(script_path)) {
      throw_runtime_error({ "run: script not found: ", script_path });
    }
    if (!fs::is_regular_file(script_path)) {
      throw_runtime_error({ "run: script is not a regular file: ", script_path });
    }
    start_dir = script_path.parent_path();
    // Strip sentinel only; keep script path and everything else
    exec_command.reserve(cfg_.command.size() - 1);
    for (size_t i{ 0 }; i < cfg_.command.size(); ++i) {
      if (i != sentinel_pos) { exec_command.push_back(std::move(cfg_.command[i])); }
    }
  } else if (fs::path const first_arg{ fs::absolute(cfg_.command[0]) };
             fs::exists(first_arg) && fs::is_regular_file(first_arg)) {
    // No sentinel; command[0] is an existing file — use its directory
    start_dir = first_arg.parent_path();
    exec_command = std::move(cfg_.command);
  } else {
    // No sentinel, command[0] not a file — use CWD
    start_dir = fs::current_path();
    exec_command = std::move(cfg_.command);
  }

  // --project outranks both inferences above: the caller named the project outright, so
  // there is nothing left to guess from a script path or the CWD.
  if (cfg_.project_dir) { start_dir = manifest::discovery_start_dir(cfg_.project_dir); }

  if (cfg_.activate) {
    activate_process_env(start_dir);
  } else {
    prepend_bin_dir(start_dir, exec_command.size() > 1);
  }

  tui::sections_clear();
  tui::flush();  // activation's warnings: execvp would discard the queue

  auto const argv{ [&] {
    std::vector<char *> a;
    a.reserve(exec_command.size() + 1);
    for (auto &arg : exec_command) { a.push_back(arg.data()); }
    a.push_back(nullptr);
    return a;
  }() };

#ifdef _WIN32
  // Windows has no true execvp; spawn the child, wait, propagate its exit code.
  intptr_t const rc{ _spawnvp(_P_WAIT, argv[0], argv.data()) };
  if (rc == -1) { throw_runtime_error({ "run: spawn failed: ", std::strerror(errno) }); }
  throw subprocess_exit{ static_cast<int>(rc) };
#else
  execvp(argv[0], argv.data());
  throw_runtime_error({ "run: exec failed: ", std::strerror(errno) });
#endif
}

// The activation `envy activate` would load, written into this process just before it
// becomes (or spawns) the command: nothing else runs in between to see it.
void cmd_run::activate_process_env(fs::path const &start_dir) const {
  shell_env_t const live{ shell_getenv() };
  auto const act{ activation_compute(start_dir, cli_cache_root_, live) };
  for (auto const &[name, value] : act.target) {
    if (auto const *cur{ env_get(live, name) }; !cur || *cur != value) {
      platform::env_var_set(name.c_str(), value.c_str());
    }
  }
  for (auto const &[name, _] : live) {
    if (!env_get(act.target, name)) { platform::env_var_unset(name.c_str()); }
  }
}

void cmd_run::prepend_bin_dir(fs::path const &start_dir, bool hint_sentinel) const {
  auto const discovered{ manifest::discover(false, start_dir) };
  if (!discovered) {
    std::string msg{ "run: manifest not found (discovery from " + start_dir.string() +
                     ")" };
    if (!cfg_.project_dir && hint_sentinel) {
      msg += "\nhint: use '--' to specify script location for manifest discovery";
    }
    throw_runtime_error({ msg });
  }
  auto const &manifest_path{ discovered->path };
  auto const &meta{ discovered->meta };  // discovery parsed the directives already
  // No manifest_resolved event here: trace records are queued for the tui worker, and
  // execvp below replaces this image before it drains. It would surface only on Windows,
  // which waits on a child instead -- a trace that exists on one platform is worse than
  // none. What `run` resolved is observable in the PATH the child inherits.

  reexec_if_needed(
      meta,
      resolve_cache_root(meta.cache_request(cli_cache_root_, manifest_path.parent_path())),
      manifest_path.parent_path());

  if (!meta.bin) {
    throw_runtime_error({ "run: manifest has no @envy bin directive: ", manifest_path });
  }

  auto const bin_dir_raw{ manifest_path.parent_path() / *meta.bin };
  if (!fs::exists(bin_dir_raw) || !fs::is_directory(bin_dir_raw)) {
    throw_runtime_error({ "run: bin directory does not exist: ", bin_dir_raw });
  }
  auto const bin_dir{ fs::canonical(bin_dir_raw) };

  auto const manifest_dir{ manifest_path.parent_path().string() };

  char const *existing_path{ std::getenv("PATH") };
  std::string new_path{ bin_dir.string() };
  if (existing_path && *existing_path) {
    new_path += env_path_sep();
    new_path += existing_path;
  }

  platform::env_var_set("PATH", new_path.c_str());
  platform::env_var_set("ENVY_PROJECT_ROOT", manifest_dir.c_str());
}

}  // namespace envy
