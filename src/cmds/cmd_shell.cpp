#include "cmd_shell.h"

#include "manifest.h"

#include "cache.h"
#include "cmd_init.h"
#include "platform.h"
#include "tui.h"

#include "CLI11.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace envy {

namespace fs = std::filesystem;

namespace {

struct shell_info {
  char const *name;
  char const *ext;
  char const *profile_hint;
  char const *source_cmd;  // e.g. "source" or "."
};

// clang-format off
constexpr shell_info kShells[] = {
  {"bash",       "bash", "~/.bashrc",                  "source"},
  {"zsh",        "zsh",  "~/.zshrc",                   "source"},
  {"fish",       "fish", "~/.config/fish/config.fish", "source"},
  {"powershell", "ps1",  "$PROFILE",                   "."},
};
// clang-format on

shell_info const *find_shell(std::string const &name) {
  for (auto const &s : kShells) {
    if (s.name == name) { return &s; }
  }
  return nullptr;
}

// The project this command was invoked from, if it resolves at all -- advisory only.
// Best-effort by design: discovery parses directives and resolution rejects states such as
// both markers existing at once, and neither can move the hook path, so letting either
// escape would break `envy shell` over state that has nothing to do with it.
std::optional<cache_root_resolution> project_cache_best_effort(
    std::optional<fs::path> const &cli_cache_root,
    std::optional<fs::path> const &project_dir) {
  try {
    if (auto const found{
            manifest::discover(false, manifest::discovery_start_dir(project_dir)) }) {
      return resolve_cache_root(
          found->meta.cache_request(cli_cache_root, found->path.parent_path()));
    }
  } catch (std::exception const &) {}
  return std::nullopt;
}

}  // namespace

void cmd_shell::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("shell",
                                "Print shell hook source line for your profile") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("shell", cfg_ptr->shell, "Shell name (bash, zsh, fish, powershell)")
      ->required()
      ->check(CLI::IsMember({ "bash", "zsh", "fish", "powershell" }));
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_shell::cmd_shell(cmd_shell::cfg cfg, std::optional<fs::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_shell::execute() {
  auto const *si{ find_shell(cfg_.shell) };
  if (!si) {
    throw std::runtime_error("shell: unsupported shell '" + cfg_.shell +
                             "'. Use: bash, zsh, fish, powershell");
  }

  // Hooks belong to the user, not to a project: the profile sources one path for every
  // directory the shell ever visits, so the hook root is the override or the platform
  // default and no project tier can move it. It is also the only root a local-cache
  // project must never populate, which is why self-deploy skips hooks there entirely.
  auto const hook_root{ resolve_user_wide_cache_root(cli_cache_root_) };
  if (!hook_root) {
    throw std::runtime_error(
        std::string{ "shell: cannot determine a user-wide cache root (" } +
        platform::get_default_cache_root_env_vars() +
        " not set), so there is nowhere for shell hooks to live. Set ENVY_CACHE_ROOT.");
  }

  auto const project{ project_cache_best_effort(cli_cache_root_, cfg_.project_dir) };
  bool const project_is_local{ project && project->mode == cache_mode::LOCAL };

  // A tree written by an envy that still put hooks under the project root. Named rather
  // than deleted: it may be the very file the user's profile sources today, and nothing
  // refreshes it any more, so the version stamp is frozen wherever it stopped.
  if (project && project_is_local && fs::exists(project->root / "shell")) {
    tui::warn("Ignoring stale shell hooks under %s",
              (project->root / "shell").string().c_str());
    tui::warn("An older envy wrote them there. Source the path below instead, then "
              "delete that directory.");
  }

  fs::path const hook_path{ *hook_root / "shell" / ("hook." + std::string{ si->ext }) };
  if (!fs::exists(hook_path)) {
    // "Run any envy command" is exactly the advice a local-cache-only user cannot act on:
    // every command they run skips hooks by design, so say what would actually populate it.
    throw std::runtime_error(
        "shell: hook file not found at " + hook_path.string() + ". " +
        (project_is_local
             ? "This project uses its own cache tree, which never populates shell hooks. "
               "Run an envy command in a project on the user-wide cache, or set "
               "ENVY_CACHE_ROOT."
             : "Run any envy command to trigger self-deploy."));
  }

  std::string const portable{ make_portable_path(hook_path) };

  // Convert VS Code-style env placeholders to shell-native syntax for display
  std::string display_path{ portable };
  if (cfg_.shell != "powershell") {
    // bash/zsh/fish use $HOME; make_portable_path() returns ${env:HOME} on
    // Unix and ${env:USERPROFILE} on Windows — map both to $HOME.
    constexpr std::string_view kEnvHome{ "${env:HOME}" };
    constexpr std::string_view kEnvUserProfile{ "${env:USERPROFILE}" };

    auto pos{ display_path.find(kEnvHome) };
    if (pos != std::string::npos) { display_path.replace(pos, kEnvHome.size(), "$HOME"); }
    pos = display_path.find(kEnvUserProfile);
    if (pos != std::string::npos) {
      display_path.replace(pos, kEnvUserProfile.size(), "$HOME");
    }
  }

  std::string const source_line{ [&] {
    std::ostringstream oss;
    oss << si->source_cmd << " \"" << display_path << '"';
    return oss.str();
  }() };

  tui::info("Add this line to %s:", si->profile_hint);
  tui::info("");
  tui::info("  %s", source_line.c_str());
  tui::info("");

  // Only an override earns the warning now. A project-local tree used to trigger it, but
  // hooks no longer live there at all -- and a plain platform default is not "easily lost"
  // in any sense worth a line of output.
  if (cli_cache_root_) {
    tui::warn("Hook files are stored in cache at %s", hook_root->string().c_str());
    tui::warn("Moving or deleting this cache will break shell integration.");
  }

  tui::info("Then restart your shell or run the command directly.");
}

}  // namespace envy
