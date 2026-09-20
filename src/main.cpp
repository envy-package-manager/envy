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
  std::setlocale(LC_ALL, "");  // libarchive needs it to convert UTF-8 entry pathnames

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

  // Inside the try: deploy_target reads a manifest, and a bad directive outside it
  // surfaced as `libc++abi: terminating` instead of envy's own error line.
  try {
    auto const target{ envy::deploy_target(args) };
    envy::self_deploy::ensure(target.root, target.mode);

    auto cmd{ std::visit(
        [&args](auto const &cfg) { return envy::cmd::create(cfg, args.cache_root); },
        *args.cmd_cfg) };

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
