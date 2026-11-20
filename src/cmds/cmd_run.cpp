#include "cmd_run.h"

#include "activation.h"
#include "env_ops.h"
#include "manifest.h"
#include "reexec.h"
#include "shell.h"
#include "util.h"

#include "cli_parse.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace envy {

namespace fs = std::filesystem;

namespace {

// The caller's env with the bin dir first on PATH and ENVY_PROJECT_ROOT set.
shell_env_t bin_dir_env(fs::path const &start_dir,
                        bool hint_sentinel,
                        std::optional<fs::path> const &cli_cache_root,
                        shell_env_t const &live) {
  auto const discovered{ manifest::discover(false, start_dir) };
  if (!discovered) {
    std::string msg{ "run: manifest not found (discovery from " + start_dir.string() +
                     ")" };
    if (hint_sentinel) {
      msg += "\nhint: use '--' to specify script location for manifest discovery";
    }
    throw_runtime_error({ msg });
  }
  auto const &manifest_path{ discovered->path };
  auto const &meta{ discovered->meta };  // discovery parsed the directives already

  reexec_if_needed(
      meta,
      resolve_cache_root(meta.cache_request(cli_cache_root, manifest_path.parent_path())),
      manifest_path.parent_path());

  if (!meta.bin) {
    throw_runtime_error({ "run: manifest has no @envy bin directive: ", manifest_path });
  }

  auto const bin_dir_raw{ manifest_path.parent_path() / *meta.bin };
  if (!fs::exists(bin_dir_raw) || !fs::is_directory(bin_dir_raw)) {
    throw_runtime_error({ "run: bin directory does not exist: ", bin_dir_raw });
  }
  auto const bin_dir{ fs::canonical(bin_dir_raw) };

  shell_env_t out{ live };
  env_ops_apply(out,
                { { .name = "ENVY_PROJECT_ROOT",
                    .op = env_set{ manifest_path.parent_path().string() } },
                  { .name = "PATH",
                    .op = env_edit{ .prepend = { bin_dir.string() },
                                    .sep = std::string(1, env_path_sep()) } } });
  return out;
}

}  // namespace

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

  // --activate: what `envy activate` would load; an env already this project's current
  // activation passes through as the caller has it. main execs once the tui is down.
  shell_env_t live{ shell_getenv() };
  shell_env_t to{ cfg_.activate
                      ? activation_compute(start_dir, cli_cache_root_, live, true).target
                      : bin_dir_env(start_dir,
                                    exec_command.size() > 1 && !cfg_.project_dir,
                                    cli_cache_root_,
                                    live) };
  throw exec_request{ .argv = std::move(exec_command),
                      .from = std::move(live),
                      .to = std::move(to) };
}

}  // namespace envy
