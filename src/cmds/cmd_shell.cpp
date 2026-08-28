#include "cmd_shell.h"

#include "manifest.h"

#include "cache.h"
#include "cmd_init.h"
#include "tui.h"

#include "CLI11.hpp"

#include <filesystem>
#include <memory>
#include <sstream>
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

// Any root that is not the user-wide default: a project-local tree counts, and is in fact
// the case that most needs the warning, since `rm -rf` on the build root takes the hooks
// with it. Keying on the CLI flag alone missed exactly that.
bool is_custom_cache(cache_root_resolution const &resolved) {
  return resolved.tier != cache_root_tier::DEFAULT || resolved.mode == cache_mode::LOCAL;
}

// Same resolution every other command performs, so `envy shell` names the tree that
// self-deploy actually wrote its hooks into. Built manifest-blind, this reported the
// platform default and then failed to find a hook that was sitting in the local tree.
cache_root_resolution resolve_for_shell(std::optional<fs::path> const &cli_cache_root,
                                        std::optional<fs::path> const &project_dir) {
  envy_meta meta;
  fs::path manifest_dir;
  if (auto const found{ manifest::discover(
          false, manifest::discovery_start_dir(project_dir)) }) {
    meta = found->meta;
    manifest_dir = found->path.parent_path();
  }
  return resolve_cache_root(meta.cache_request(cli_cache_root, manifest_dir));
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

  auto const resolved{ resolve_for_shell(cli_cache_root_, cfg_.project_dir) };
  auto c{ std::make_unique<cache>(resolved.root) };

  fs::path const hook_path{ c->root() / "shell" / ("hook." + std::string{ si->ext }) };
  if (!fs::exists(hook_path)) {
    throw std::runtime_error("shell: hook file not found at " + hook_path.string() +
                             ". Run any envy command to trigger self-deploy.");
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

  if (is_custom_cache(resolved)) {
    tui::warn("Hook files are stored in cache at %s", c->root().string().c_str());
    tui::warn("Moving or deleting this cache will break shell integration.");
  }

  tui::info("Then restart your shell or run the command directly.");
}

}  // namespace envy
