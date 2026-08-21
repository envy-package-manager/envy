#include "aws_util.h"
#include "cli.h"
#include "libgit2_util.h"
#include "reexec.h"
#include "self_deploy.h"
#include "shell.h"
#include "termination.h"
#include "tui.h"

#include <clocale>
#include <cstdlib>
#include <variant>

int main(int argc, char *argv[]) {
  // Adopt the environment's locale so libarchive (and other libc consumers) can
  // convert UTF-8 archive/entry pathnames instead of failing under the "C" locale.
  std::setlocale(LC_ALL, "");

  envy::termination_handler_install();
  envy::tui::init();
  envy::shell_init();

  auto args{ envy::cli_parse(argc, argv) };
  envy::tui::configure_trace_outputs(args.trace_outputs);
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

  envy::self_deploy::ensure(args.cache_root, std::nullopt, {});

  auto cmd{ std::visit(
      [&args](auto const &cfg) { return envy::cmd::create(cfg, args.cache_root); },
      *args.cmd_cfg) };

  try {
    cmd->execute();
  } catch (envy::reexec_request const &rr) {
    // argv belongs to this frame, so this is where a re-exec can happen at all.
    return envy::reexec_exec(rr, argv);
  } catch (envy::subprocess_exit const &se) {
    return se.code;
  } catch (std::exception const &ex) {
    envy::tui::error("%s", ex.what());
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
