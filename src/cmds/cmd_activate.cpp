#include "cmd_activate.h"

#include "activation.h"
#include "env_ops.h"
#include "reexec.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"

#include <cstdio>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#define ENVY_DUP _dup
#define ENVY_DUP2 _dup2
#define ENVY_CLOSE _close
#define ENVY_WRITE _write
#define ENVY_FILENO _fileno
#else
#include <unistd.h>
#define ENVY_DUP ::dup
#define ENVY_DUP2 ::dup2
#define ENVY_CLOSE ::close
#define ENVY_WRITE ::write
#define ENVY_FILENO ::fileno
#endif

namespace envy {

namespace {

// Points fd 1 at stderr for the rest of the process, so nothing but the script -- no
// envy.stdout from a spec's Lua, no queued tui output -- reaches the shell that evals it.
class stdout_to_stderr : unmovable {
 public:
  stdout_to_stderr() : saved_{ (std::fflush(stdout), ENVY_DUP(ENVY_FILENO(stdout))) } {
    ENVY_DUP2(ENVY_FILENO(stderr), ENVY_FILENO(stdout));
  }
  ~stdout_to_stderr() {
    if (saved_ >= 0) { ENVY_CLOSE(saved_); }
  }

  void restore() {  // a re-exec'd envy must inherit the real stdout
    std::fflush(stdout);
    ENVY_DUP2(saved_, ENVY_FILENO(stdout));
  }

  void write(std::string_view s) const {
    while (!s.empty()) {
      auto const n{ ENVY_WRITE(saved_, s.data(), static_cast<unsigned>(s.size())) };
      if (n <= 0) { throw_runtime_error({ "activate: failed to write the script" }); }
      s.remove_prefix(static_cast<size_t>(n));
    }
  }

 private:
  int saved_;
};

}  // namespace

cli_cmd &cmd_activate::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("activate", "Print shell code that loads the project's environment") };
  sub.pos("shell", c.shell, "Shell name (bash, zsh, fish, powershell)")
      .required()
      .one_of("bash,zsh,fish,powershell");
  return sub;
}

cmd_activate::cmd_activate(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root)
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
