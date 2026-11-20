#include "aws_util.h"
#include "cli.h"
#include "cmd.h"
#include "env_ops.h"
#include "libgit2_util.h"
#include "reexec.h"
#include "self_deploy.h"
#include "shell.h"
#include "termination.h"
#include "tui.h"
#include "util.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

int main(int argc, char *argv[]) {
  std::setlocale(LC_ALL, "");
  envy::termination_handler_install();
  envy::tui::init();
  envy::shell_init();

  auto args{ envy::cli_parse(argc, argv) };
  envy::tui::configure_trace_outputs(args.trace_outputs);

  std::optional<envy::exec_request> exec;
  int const rc{ [&] {
    envy::tui::scope tui_scope{ args.verbosity, args.decorated_logging };
    envy::aws_shutdown_guard aws_guard;
    envy::libgit2_scope git_guard;

    if (!args.cli_output.empty()) {
      if (!args.cmd_cfg.has_value()) {
        envy::tui::error("%s", args.cli_output.c_str());
        return EXIT_FAILURE;
      }
      envy::tui::info("%s", args.cli_output.c_str());
    }

    if (!args.cmd_cfg.has_value()) { return EXIT_FAILURE; }

    try {
      auto const target{ envy::deploy_target(args) };
      envy::self_deploy::ensure(target.root, target.mode);

      auto cmd{ envy::visit(
          [&args](auto const &cfg) { return envy::cmd::create(cfg, args.cache_root); },
          *args.cmd_cfg) };

      cmd->execute();
    } catch (envy::reexec_request const &rr) {
      return envy::reexec_exec(rr, argv);
    } catch (envy::exec_request &er) {
      exec = std::move(er);
    } catch (std::exception const &ex) {
      envy::tui::error("%s", ex.what());
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }() };
  if (!exec) { return rc; }

  // Everything above is torn down: the cursor is back, the rows are final, no thread left.
  try {
    envy::env_write_process(exec->from, exec->to);
    return envy::shell_exec(exec->argv);  // POSIX: returns only by throwing
  } catch (std::exception const &ex) {
    std::fprintf(stderr, "envy: %s\n", ex.what());
    return EXIT_FAILURE;
  }
}
