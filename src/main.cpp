#include "aws_util.h"
#include "cli.h"
#include "libgit2_util.h"
#include "manifest.h"
#include "reexec.h"
#include "self_deploy.h"
#include "shell.h"
#include "termination.h"
#include "tui.h"

#include <clocale>
#include <concepts>
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

  // Inside the try: this reads and parses a manifest, so a bad directive throws here, and
  // outside it that surfaced as `libc++abi: terminating due to uncaught exception` instead
  // of envy's own error line.
  try {
    // Manifest-aware: built from the override alone, this deployed one version into two
    // trees. The whole resolution, since a local tree writes no shell hooks.
    auto const deploy_target{ [&] {
      // Best-effort, and deliberately silent on failure. Both halves throw: discovery
      // parses directives, and resolution rejects states such as both override markers
      // existing at once. This step runs for *every* command, including ones that never
      // load a manifest, so letting either escape meant `envy --version` and `envy init`
      // failed before doing anything when run anywhere inside a project with a bad
      // directive -- precisely what a migration leaves behind -- and that `envy cache
      // --local`, the command best placed to repair a bad marker pair, was blocked by the
      // very state it fixes. A command that needs the manifest re-resolves and reports the
      // error properly; the rest land on the default root, exactly as they did before this
      // step became manifest-aware.
      try {
        envy::envy_meta meta;
        std::filesystem::path manifest_dir;
        // Skipped under an override, which already decides the root: discovery and
        // directive parsing both throw, so reading a manifest that cannot change the
        // answer would let any broken envy.lua in an ancestor break every command run with
        // --cache-root.
        if (!args.cache_root) {
          // The same anchor the command itself will use, pulled off whichever config was
          // selected: resolving this pre-step from the CWD while the command resolved from
          // its bin dir would deploy envy into one tree and its packages into another.
          std::optional<std::filesystem::path> project_dir;
          std::visit(
              [&](auto const &c) {
                if constexpr (std::derived_from<std::decay_t<decltype(c)>,
                                                envy::cmd_project_anchor>) {
                  project_dir = c.project_dir;
                }
              },
              *args.cmd_cfg);
          if (auto const found{ envy::manifest::discover(
                  false,
                  envy::manifest::discovery_start_dir(project_dir)) }) {
            meta = found->meta;
            manifest_dir = found->path.parent_path();
          }
        }
        auto const req{ meta.cache_request(args.cache_root, manifest_dir) };

        // Runs before the marker is written, so the deploy names the tree the command is
        // about to establish -- not the one the user is in the middle of abandoning.
        if (auto const *cc{ std::get_if<envy::cmd_cache::cfg>(&*args.cmd_cfg) };
            cc && (cc->act == envy::cmd_cache::cfg::action::SET_LOCAL ||
                   cc->act == envy::cmd_cache::cfg::action::SET_SHARED)) {
          return envy::cache_root_for_mode(
              req,
              cc->act == envy::cmd_cache::cfg::action::SET_LOCAL
                  ? envy::cache_mode::LOCAL
                  : envy::cache_mode::SHARED);
        }

        return envy::resolve_cache_root(req);
      } catch (std::exception const &) {
        return envy::resolve_cache_root(envy::cache_root_request{});
      }
    }() };
    envy::self_deploy::ensure(deploy_target.root, deploy_target.mode);

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
