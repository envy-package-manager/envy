#pragma once

#include "extract.h"
#include "fetch.h"
#include "platform.h"
#include "sha256.h"
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

  // Land the row on a full bar: the last callback fires before the archive is closed out,
  // so the row would otherwise keep whatever partial frame it ended on.
  void finish();

 private:
  void publish(extract_progress const &prog, bool terminal);

  tui::section_handle section_;
  std::string label_;
  std::string filename_;
  std::chrono::steady_clock::time_point start_time_;
  extract_progress last_;
  // extract() reports bytes but no file count and no total, so the tracker tallies its own
  // and spins rather than draw a bar stuck at 0%.
  std::uint64_t files_seen_{ 0 };
  std::filesystem::path last_entry_;
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

  // Land every row on its terminal frame: callbacks stop firing before teardown, so the
  // last frame the renderer sees is otherwise mid-flight. `ok` is by slot and empty means
  // all ok; a failed slot keeps its last frame.
  void finish(std::vector<bool> const &ok = {});

 private:
  // A clone reports two phases through one callback: objects arriving, then deltas
  // resolving. Each percent is monotonic within its own phase — a shared clamp would
  // pin delta resolution at the receive phase's 100%.
  struct git_state {
    double last_receive_percent{ 0.0 };
    double last_delta_percent{ 0.0 };
    std::uint32_t max_total_objects{ 0 };
    std::uint32_t last_received_objects{ 0 };
    std::uint32_t max_total_deltas{ 0 };
    std::uint32_t last_indexed_deltas{ 0 };
    std::uint64_t last_bytes{ 0 };
  };

  // What a completed slot reports: git rows count objects, HTTP rows bytes. `bytes` is the
  // advertised length once one has been seen, otherwise the last count transferred.
  struct slot_total {
    std::uint64_t bytes{ 0 };
    bool bytes_known{ false };
    bool is_git{ false };
  };

  void update_transfer(std::size_t slot, fetch_transfer_progress const &prog);
  void update_git(std::size_t slot, fetch_git_progress const &prog);
  void set_frame(std::size_t slot, tui::section_frame child_frame);
  void publish_unlocked(bool terminal);  // push children_ out; caller holds mutex_
  tui::section_frame final_frame_unlocked(std::size_t slot) const;

  tui::section_handle section_;
  std::string label_;
  std::string group_text_;
  std::chrono::steady_clock::time_point start_time_;
  std::mutex mutex_;
  std::vector<tui::section_frame> children_;
  std::vector<git_state> git_states_;
  std::vector<slot_total> slot_totals_;
  bool grouped_;
};

// A row for a wait with nothing to count: another envy owns the entry for as long as its
// own work takes. Pass to cache::ensure_* as `on_lock_contended`; it names what is waited
// for.
platform::file_lock::contended_cb_t lock_wait_spinner(tui::section_handle section,
                                                      std::string const &row_label,
                                                      std::string what);

// A bar for anything counting bytes toward a known total (sha256, archive writers). `verb`
// leads the status ("hashing 210.00MB/499.97MB pkg.tar.zst"); terminal once done == total.
byte_progress_cb_t byte_progress_bar(tui::section_handle section,
                                     std::string const &row_label,
                                     std::string verb,
                                     std::string item);

// Hash a file with a bar on `section`: sha256() + byte_progress_bar(), for the many
// callers that hash one file and want the wait visible.
sha256_t sha256_tracked(std::filesystem::path const &file,
                        tui::section_handle section,
                        std::string const &row_label,
                        std::string verb = "hashing");

// Download with a progress bar on a scratch section, for command-level and bootstrap
// fetches that have no package row to draw on. Anything downloaded gets a bar; on success
// its row stays at a full bar, on failure it goes and the returned error is the report.
// `item_labels` names each transfer and must match `requests` in size — pass the URL when
// the destination is a temp file whose name would say nothing.
std::vector<fetch_result_t> fetch_tracked(std::vector<fetch_request> requests,
                                          std::string const &row_label,
                                          std::vector<std::string> const &item_labels,
                                          std::string trace_spec = {});

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
