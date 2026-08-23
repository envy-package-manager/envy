#include "cmd_git_resolve.h"

#include "git_resolve.h"
#include "tui.h"

#include "CLI11.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace envy {

void cmd_git_resolve::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand(
      "git-resolve",
      "Resolve a git ref (tag/branch/sha) in a remote repo to a full commit sha") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("url", cfg_ptr->repo, "Remote repository URL (https/git/file)")
      ->required();
  sub->add_option("ref", cfg_ptr->ref, "Ref to resolve: tag, branch, or full sha")
      ->required();
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_git_resolve::cmd_git_resolve(
    cmd_git_resolve::cfg cfg,
    std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_git_resolve::execute() {
  // A ref advertisement is a network round trip that reports nothing along the way, so the
  // row is a bare spinner. It goes away once the sha is in hand — stdout carries the
  // answer, and a finished spinner says nothing.
  auto const section{ tui::section_create() };
  tui::section_set_content(
      section,
      tui::section_frame{ .label = "[git-resolve]",
                          .content = tui::spinner_data{
                              .text = "resolving " + cfg_.ref + " in " + cfg_.repo,
                              .start_time = std::chrono::steady_clock::now() } });

  auto const sha{ git_resolve_remote(cfg_.repo, cfg_.ref) };
  tui::section_delete(section);
  tui::print_stdout("%s\n", sha.c_str());
}

}  // namespace envy
