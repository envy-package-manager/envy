#include "cmd_extract.h"

#include "extract.h"
#include "tui.h"
#include "tui_actions.h"

#include "CLI11.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace envy {

void cmd_extract::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("extract", "Extract archive to destination") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("archive", cfg_ptr->archive_path, "Archive file to extract")
      ->required()
      ->check(CLI::ExistingFile);
  sub->add_option("destination",
                  cfg_ptr->destination,
                  "Destination directory (defaults to current directory)");
  sub->add_option("--only",
                  cfg_ptr->only,
                  "Extract only this archive-relative path or glob; a directory takes its "
                  "whole subtree (repeatable, default: everything)")
      ->allow_extra_args(false);
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_extract::cmd_extract(cmd_extract::cfg cfg,
                         std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_extract::execute() {
  std::filesystem::path destination{ cfg_.destination };
  if (destination.empty()) { destination = std::filesystem::current_path(); }

  std::error_code ec;
  if (!std::filesystem::exists(cfg_.archive_path, ec)) {
    throw std::runtime_error("extract: archive not found: " + cfg_.archive_path.string());
  }

  if (!std::filesystem::is_regular_file(cfg_.archive_path, ec)) {
    throw std::runtime_error("extract: not a regular file: " + cfg_.archive_path.string());
  }

  if (!std::filesystem::exists(destination, ec)) {
    std::filesystem::create_directories(destination, ec);
    if (ec) {
      throw std::runtime_error("extract: failed to create destination directory: " +
                               ec.message());
    }
  }

  if (!std::filesystem::is_directory(destination, ec)) {
    throw std::runtime_error("extract: destination is not a directory: " +
                             destination.string());
  }

  tui::info("Extracting %s to %s",
            cfg_.archive_path.filename().string().c_str(),
            destination.string().c_str());

  // No pre-scan, so no total: the row spins on the running counts and lands on a full bar
  // once the archive is out. The section outlives the call as the command's record of it.
  auto const section{ tui::section_create() };
  tui_actions::extract_progress_tracker tracker{ section,
                                                 "extract",
                                                 cfg_.archive_path.filename().string() };

  auto const file_count{ extract(
      cfg_.archive_path,
      destination,
      { .selectors = cfg_.only, .progress = std::ref(tracker) }) };
  tracker.finish();
  tui::info("Extracted %llu files", static_cast<unsigned long long>(file_count));
}

}  // namespace envy
