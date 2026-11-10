#include "cmd_deactivate.h"

#include "activation.h"
#include "env_ops.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace envy {

cli_cmd &cmd_deactivate::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("deactivate", "Print shell code that restores the pre-activate environment") };
  sub.pos("shell", c.shell, "Shell name (bash, zsh, fish, powershell)")
      .required()
      .one_of("bash,zsh,fish,powershell");
  return sub;
}

cmd_deactivate::cmd_deactivate(cfg cfg, std::optional<std::filesystem::path> const &)
    : cfg_{ std::move(cfg) } {}

void cmd_deactivate::execute() {
  env_shell const sh{ activation_shell(cfg_.shell, "deactivate") };
  if (activation_stdout_is_tty()) {
    tui::info("deactivate prints shell code; load it with: %s",
              activation_eval_hint("deactivate", cfg_.shell).c_str());
    return;
  }

  shell_env_t const live{ shell_getenv() };
  std::string const script{ env_script(sh, live, env_restore(live)) };
  std::fwrite(script.data(), 1, script.size(), stdout);
  std::fflush(stdout);

  if (auto const *root{ env_get(live, kEnvStateRoot) }) {
    activation_announce("leaving", *root, "environment restored");
  }
}

}  // namespace envy
