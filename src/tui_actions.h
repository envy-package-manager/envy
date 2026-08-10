#pragma once

#include "extract.h"
#include "fetch.h"
#include "shell.h"
#include "tui.h"
#include "util.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace envy::tui_actions {

// Shell command execution progress tracker
// Lifetime: matches shell_run() blocking call
class run_progress {
 public:
  run_progress(tui::section_handle section,
               std::string const &pkg_identity,
               std::filesystem::path const &cache_root,
               product_map_t products = {});

  void on_command_start(std::string_view cmd);
  void on_output_line(std::string_view line);

 private:
  tui::section_handle section_;
  std::string label_;
  std::filesystem::path cache_root_;
  product_map_t products_;
  std::chrono::steady_clock::time_point start_time_;
  std::vector<std::string> lines_;
  std::string header_text_;
};

// Single-file extraction progress tracker
// Lifetime: matches extract() blocking call
class extract_progress_tracker {
 public:
  extract_progress_tracker(tui::section_handle section,
                           std::string const &pkg_identity,
                           std::string const &filename);

  bool operator()(extract_progress const &prog);

 private:
  tui::section_handle section_;
  std::string label_;
  std::string filename_;
  std::chrono::steady_clock::time_point start_time_;
};

// The one transfer progress tracker: every download in the process reports through
// it, HTTP or git, one file or many. A single label renders as one row labeled with
// the package identity; several render as a parent row (group_text, e.g. "fetch",
// "upload") over indented per-file children.
// Lifetime: matches the blocking call for multiple transfers
class fetch_all_progress_tracker {
 public:
  fetch_all_progress_tracker(tui::section_handle section,
                             std::string const &pkg_identity,
                             std::vector<std::string> const &labels,
                             std::string group_text);

  fetch_progress_cb_t make_callback(std::size_t slot);

 private:
  struct git_state {
    double last_percent{ 0.0 };
    std::uint32_t max_total_objects{ 0 };
    std::uint32_t last_received_objects{ 0 };
    std::uint64_t last_bytes{ 0 };
  };

  void update_transfer(std::size_t slot, fetch_transfer_progress const &prog);
  void update_git(std::size_t slot, fetch_git_progress const &prog);
  void set_frame(std::size_t slot, tui::section_frame child_frame);

  tui::section_handle section_;
  std::string label_;
  std::string group_text_;
  std::mutex mutex_;
  std::vector<tui::section_frame> children_;
  std::vector<git_state> git_states_;
  bool grouped_;
};

// Unified shell execution with TUI progress tracking.
// Creates a run_progress tracker, shows scrubbed command header; the tracker itself
// controls how much output is displayed (e.g., limiting the visible output to 3 lines).
// Callers set up on_stdout_line/on_stderr_line for capture; this function overwrites
// on_output_line in cfg to route output through the progress tracker.
// If section is invalid, runs without progress tracking.
shell_result run_shell_with_progress(std::string_view script,
                                     tui::section_handle section,
                                     std::string const &pkg_identity,
                                     std::filesystem::path const &cache_root,
                                     shell_run_cfg cfg);

// Run a phase's shell script with stdout/stderr capture. On nonzero exit, prints
// captured stdout and a truncated stderr tail via tui::error, then throws
// "<phase_label> shell script failed for <identity> (exit code N)" (or the
// terminated-by-signal variant).
void run_phase_shell_script(std::string_view script,
                            std::string_view phase_label,
                            std::filesystem::path const &cwd,
                            std::string const &identity,
                            resolved_shell shell,
                            tui::section_handle section,
                            std::filesystem::path const &cache_root);

}  // namespace envy::tui_actions
