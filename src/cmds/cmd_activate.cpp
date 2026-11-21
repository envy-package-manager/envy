#include "cmd_activate.h"

#include "activation.h"
#include "env_ops.h"
#include "reexec.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"

#include <string>
#include <string_view>

namespace envy {

cli_cmd &cmd_activate::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("activate",
                     "Print shell code that loads the project's environment") };
  sub.pos("shell", c.shell, "Shell name (bash, zsh, fish, powershell, cmd)")
      .required()
      .one_of("bash,zsh,fish,powershell,cmd");
  return sub;
}

cmd_activate::cmd_activate(cfg cfg,
                           std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_activate::execute() {
  env_shell const sh{ activation_shell(cfg_.shell, "activate") };
  if (activation_stdout_is_tty()) {
    tui::info("activate prints shell code; load it with: %s",
              activation_eval_hint("activate", cfg_.shell).c_str());
    return;
  }

  stdout_to_stderr redirect;
  shell_env_t const live{ shell_getenv() };
  activation const act{ [&] {
    try {
      return activation_compute(cfg_.project_dir, cli_cache_root_, live);
    } catch (reexec_request const &) {
      redirect.restore();
      throw;
    }
  }() };
  redirect.write(env_script(sh, live, act.target));

  std::string const prior{ [&] {
    auto const *r{ env_get(live, kEnvStateRoot) };
    return r ? *r : std::string{};
  }() };
  std::string const tail{ std::to_string(act.vars) +
                          (act.vars == 1 ? " variable set" : " variables set") };
  if (prior == act.root) {
    activation_announce("refreshed", act.root, tail);
    return;
  }
  if (!prior.empty()) { activation_announce("leaving", prior, "environment restored"); }
  activation_announce("entering", act.root, tail);
}

}  // namespace envy
