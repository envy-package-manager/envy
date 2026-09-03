#include "tui_actions.h"

#include "util.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

namespace envy::tui_actions {

// ==== run_progress ====

run_progress::run_progress(tui::section_handle section,
                           std::string const &pkg_identity,
                           std::filesystem::path const &cache_root,
                           product_map_t products)
    : section_{ section },
      label_{ "[" + pkg_identity + "]" },
      cache_root_{ cache_root },
      products_{ std::move(products) },
      start_time_{ std::chrono::steady_clock::now() },
      lines_{},
      header_text_{} {}

void run_progress::on_command_start(std::string_view cmd) {
  // Flatten and simplify the command for display
  std::string const flattened{ util_flatten_script_with_semicolons(cmd) };
  header_text_ = util_simplify_cache_paths(flattened, cache_root_, products_);

  // Show spinner immediately with the command
  tui::section_set_content(
      section_,
      tui::section_frame{ .label = label_,
                          .content = tui::spinner_data{ .text = header_text_,
                                                        .start_time = start_time_ } });
}

void run_progress::on_output_line(std::string_view line) {
  if (!section_) { return; }

  lines_.emplace_back(line);
  tui::section_set_content(section_,
                           tui::section_frame{ .label = label_,
                                               .content = tui::text_stream_data{
                                                   .lines = lines_,
                                                   .line_limit = 3,
                                                   .start_time = start_time_,
                                                   .header_text = header_text_ } });
}

// ==== extract_progress_tracker ====

extract_progress_tracker::extract_progress_tracker(tui::section_handle section,
                                                   std::string const &pkg_identity,
                                                   std::string const &filename)
    : section_{ section },
      label_{ "[" + pkg_identity + "]" },
      filename_{ filename },
      start_time_{ std::chrono::steady_clock::now() } {
  // Show initial spinner
  tui::section_set_content(
      section_,
      tui::section_frame{ .label = label_,
                          .content = tui::spinner_data{ .text = "extracting " + filename_,
                                                        .start_time = start_time_ } });
}

bool extract_progress_tracker::operator()(extract_progress const &prog) {
  if (!section_) { return true; }

  if (prog.is_regular_file && prog.current_entry != last_entry_) {
    ++files_seen_;
    last_entry_ = prog.current_entry;
  }

  last_ = prog;
  publish(prog, false);
  return true;
}

void extract_progress_tracker::publish(extract_progress const &prog, bool terminal) {
  auto const files{ std::max(prog.files_processed, files_seen_) };
  auto const total_known{ (prog.total_files && *prog.total_files > 0) ||
                          (prog.total_bytes && *prog.total_bytes > 0) };

  std::ostringstream status;
  status << files;
  if (total_known && prog.total_files) { status << "/" << *prog.total_files; }
  status << " files";
  if (total_known && prog.total_bytes) {
    status << " " << util_format_bytes(prog.bytes_processed) << "/"
           << util_format_bytes(*prog.total_bytes);
  } else if (prog.bytes_processed > 0) {
    status << " " << util_format_bytes(prog.bytes_processed);
  }
  status << " " << filename_;

  // No total to divide by: an animated spinner carrying the running counts says "working"
  // where a bar pinned at 0% would say "stuck".
  if (!total_known && !terminal) {
    tui::section_set_content(
        section_,
        tui::section_frame{ .label = label_,
                            .content = tui::spinner_data{ .text = status.str(),
                                                          .start_time = start_time_ },
                            .terminal = terminal });
    return;
  }

  double const percent{ [&] {
    if (terminal) { return 100.0; }
    if (prog.total_files && *prog.total_files > 0) {
      return std::min(100.0, (files / static_cast<double>(*prog.total_files)) * 100.0);
    }
    return std::min(
        100.0,
        (prog.bytes_processed / static_cast<double>(*prog.total_bytes)) * 100.0);
  }() };

  tui::section_set_content(
      section_,
      tui::section_frame{
          .label = label_,
          .content = tui::progress_data{ .percent = percent, .status = status.str() },
          .terminal = terminal });
}

void extract_progress_tracker::finish() {
  // Nothing was ever reported, so there is nothing to land — leave the row as it stands.
  if (!section_ || (files_seen_ == 0 && last_.bytes_processed == 0)) { return; }

  extract_progress done{ last_ };
  if (done.total_files) { done.files_processed = *done.total_files; }
  if (done.total_bytes) { done.bytes_processed = *done.total_bytes; }
  publish(done, true);  // the archive is unpacked, so the row lands on a full bar
}

// ==== fetch_all_progress_tracker ====

fetch_all_progress_tracker::fetch_all_progress_tracker(
    tui::section_handle section,
    std::string const &pkg_identity,
    std::vector<std::string> const &labels,
    std::string group_text)
    : section_{ section },
      label_{ "[" + pkg_identity + "]" },
      group_text_{ std::move(group_text) },
      start_time_{ std::chrono::steady_clock::now() },
      mutex_{},
      children_{},
      git_states_(labels.size()),
      slot_totals_(labels.size()),
      grouped_{ labels.size() > 1 } {
  children_.reserve(labels.size());
  for (auto const &label : labels) {
    children_.push_back(tui::section_frame{
        .label = label,
        .content = tui::spinner_data{ .text = label, .start_time = start_time_ } });
  }

  // Render the initial state so the row is up during slow setup (e.g. AWS init).
  publish_unlocked(false);
}

fetch_progress_cb_t fetch_all_progress_tracker::make_callback(std::size_t slot) {
  return [this, slot](fetch_progress_t const &prog) -> bool {
    std::visit(
        envy::match{ [&](fetch_transfer_progress const &p) { update_transfer(slot, p); },
                     [&](fetch_git_progress const &p) { update_git(slot, p); } },
        prog);
    return true;
  };
}

void fetch_all_progress_tracker::update_transfer(std::size_t slot,
                                                 fetch_transfer_progress const &prog) {
  if (slot >= children_.size() || slot >= slot_totals_.size()) { return; }

  std::string const &item_label = children_[slot].label;

  // curl advertises no length until the headers land, and none again as the handle winds
  // down. A total, once seen, sticks — else the bar snaps to 0% just as it completes.
  slot_total const total{ [&] {
    std::lock_guard const lock{ mutex_ };
    slot_total &st{ slot_totals_[slot] };
    if (prog.total && *prog.total > 0) {
      st.bytes = *prog.total;
      st.bytes_known = true;
    } else if (!st.bytes_known) {
      st.bytes = prog.transferred;
    }
    return st;
  }() };

  std::ostringstream oss;
  if (total.bytes_known) {
    oss << util_format_bytes(prog.transferred) << "/" << util_format_bytes(total.bytes);
  } else {
    oss << util_format_bytes(prog.transferred);
  }
  if (!grouped_) { oss << " " << item_label; }

  // No advertised length — chunked or streamed. Bytes so far are all anyone can know, so
  // they ride a spinner instead of a bar that would read 0% start to finish.
  if (!total.bytes_known) {
    set_frame(
        slot,
        tui::section_frame{ .label = item_label,
                            .content = tui::spinner_data{ .text = oss.str(),
                                                          .start_time = start_time_ } });
    return;
  }

  double const percent{
    std::min(100.0, (prog.transferred / static_cast<double>(total.bytes)) * 100.0)
  };
  set_frame(slot,
            tui::section_frame{ .label = item_label,
                                .content = tui::progress_data{ .percent = percent,
                                                               .status = oss.str() } });
}

void fetch_all_progress_tracker::update_git(std::size_t slot,
                                            fetch_git_progress const &prog) {
  if (slot >= children_.size() || slot >= git_states_.size()) { return; }

  std::uint32_t snapshot_total{ 0 };
  std::uint32_t snapshot_received{ 0 };
  std::uint32_t snapshot_total_deltas{ 0 };
  std::uint32_t snapshot_deltas{ 0 };
  std::uint64_t snapshot_bytes{ 0 };
  double receive_percent{ 0.0 };
  double delta_percent{ 0.0 };
  std::string child_label;

  {
    std::lock_guard const lock{ mutex_ };
    slot_totals_[slot].is_git = true;
    git_state &state = git_states_[slot];
    state.max_total_objects = std::max(state.max_total_objects, prog.total_objects);
    state.last_received_objects =
        std::max(state.last_received_objects, prog.received_objects);
    state.last_bytes = std::max(state.last_bytes, prog.received_bytes);
    state.max_total_deltas = std::max(state.max_total_deltas, prog.total_deltas);
    state.last_indexed_deltas = std::max(state.last_indexed_deltas, prog.indexed_deltas);

    if (state.max_total_objects > 0) {
      double const pct{ (state.last_received_objects /
                         static_cast<double>(state.max_total_objects)) *
                        100.0 };
      state.last_receive_percent =
          std::min(100.0, std::max(pct, state.last_receive_percent));
    }
    if (state.max_total_deltas > 0) {
      double const pct{
        (state.last_indexed_deltas / static_cast<double>(state.max_total_deltas)) * 100.0
      };
      state.last_delta_percent = std::min(100.0, std::max(pct, state.last_delta_percent));
    }

    snapshot_total = state.max_total_objects;
    snapshot_received = state.last_received_objects;
    snapshot_total_deltas = state.max_total_deltas;
    snapshot_deltas = state.last_indexed_deltas;
    snapshot_bytes = state.last_bytes;
    receive_percent = state.last_receive_percent;
    delta_percent = state.last_delta_percent;
    child_label = children_[slot].label;
  }

  if (snapshot_total == 0) {
    static auto const epoch{ std::chrono::steady_clock::time_point{} };
    // Nothing is counted until the remote finishes negotiating, so the spinner carries
    // whatever bytes have arrived rather than a bare "starting...".
    std::string text{ snapshot_bytes > 0
                          ? util_format_bytes(snapshot_bytes) + " negotiating"
                          : std::string{ "starting..." } };
    if (!grouped_) { text += " " + child_label; }
    set_frame(slot,
              tui::section_frame{
                  .label = child_label,
                  .content = tui::spinner_data{ .text = text, .start_time = epoch } });
    return;
  }

  // The callback keeps firing after the last object lands, while the pack is indexed
  // and its deltas resolved. Follow whichever phase is live so the row keeps a moving
  // bar for the whole clone instead of collapsing to a bare object count.
  bool const receiving{ snapshot_received < snapshot_total };
  bool const resolving{ !receiving && snapshot_total_deltas > 0 };

  double percent{ 100.0 };
  std::ostringstream oss;
  if (resolving) {
    percent = delta_percent;
    oss << snapshot_deltas << "/" << snapshot_total_deltas << " deltas";
  } else {
    percent = receiving ? receive_percent : 100.0;
    oss << snapshot_received << "/" << snapshot_total << " objects";
    if (snapshot_bytes > 0) { oss << " " << util_format_bytes(snapshot_bytes); }
  }
  if (!grouped_) { oss << " " << child_label; }

  set_frame(slot,
            tui::section_frame{ .label = child_label,
                                .content = tui::progress_data{ .percent = percent,
                                                               .status = oss.str() } });
}

void fetch_all_progress_tracker::set_frame(std::size_t slot,
                                           tui::section_frame child_frame) {
  std::lock_guard const lock{ mutex_ };

  if (slot < children_.size()) { children_[slot] = std::move(child_frame); }
  publish_unlocked(false);
}

void fetch_all_progress_tracker::publish_unlocked(bool terminal) {
  if (grouped_) {
    tui::section_set_content(
        section_,
        tui::section_frame{ .label = label_,
                            .content = tui::static_text_data{ .text = group_text_ },
                            .children = children_,
                            .terminal = terminal });
    return;
  }

  if (children_.empty()) { return; }
  tui::section_frame frame{ children_[0] };  // one transfer: it *is* the row
  frame.label = label_;
  frame.terminal = terminal;
  tui::section_set_content(section_, frame);
}

tui::section_frame fetch_all_progress_tracker::final_frame_unlocked(
    std::size_t slot) const {
  auto const &total{ slot_totals_[slot] };
  std::string const &item_label{ children_[slot].label };

  std::string status{ [&] {
    if (total.is_git) {  // a clone counts objects, and by now it has them all
      auto const &git{ git_states_[slot] };
      if (git.max_total_objects == 0) { return std::string{}; }
      auto const count{ std::to_string(git.max_total_objects) };
      return count + "/" + count + " objects" +
             (git.last_bytes > 0 ? " " + util_format_bytes(git.last_bytes) : "");
    }
    if (total.bytes_known) {
      auto const bytes{ util_format_bytes(total.bytes) };
      return bytes + "/" + bytes;
    }
    // No advertised length: the last byte count seen is the whole of it.
    return total.bytes > 0 ? util_format_bytes(total.bytes) : std::string{};
  }() };

  if (!grouped_) {
    if (!status.empty()) { status += " "; }
    status += item_label;
  }

  return tui::section_frame{
    .label = item_label,
    .content = tui::progress_data{ .percent = 100.0, .status = std::move(status) }
  };
}

void fetch_all_progress_tracker::finish(std::vector<bool> const &ok) {
  std::lock_guard const lock{ mutex_ };

  for (std::size_t slot{ 0 }; slot < children_.size(); ++slot) {
    if (slot < ok.size() && !ok[slot]) { continue; }
    children_[slot] = final_frame_unlocked(slot);
  }
  publish_unlocked(true);
}

// ==== lock_wait_spinner ====

platform::file_lock::contended_cb_t lock_wait_spinner(tui::section_handle section,
                                                      std::string const &row_label,
                                                      std::string what) {
  if (!section) { return {}; }

  return [section, label = "[" + row_label + "]", what = std::move(what)] {
    tui::section_set_content(
        section,
        tui::section_frame{ .label = label,
                            .content = tui::spinner_data{
                                .text = "waiting for another envy to release " + what,
                                .start_time = std::chrono::steady_clock::now() } });
  };
}

// ==== byte_progress_bar ====

byte_progress_cb_t byte_progress_bar(tui::section_handle section,
                                     std::string const &row_label,
                                     std::string verb,
                                     std::string item) {
  if (!section) { return {}; }

  return [section,
          label = "[" + row_label + "]",
          verb = std::move(verb),
          item = std::move(item)](std::uint64_t done, std::uint64_t total) {
    auto const percent{ total
                            ? std::min(100.0, (done / static_cast<double>(total)) * 100.0)
                            : 100.0 };
    std::ostringstream status;
    status << verb << " " << util_format_bytes(done) << "/" << util_format_bytes(total);
    if (!item.empty()) { status << " " << item; }

    tui::section_set_content(
        section,
        tui::section_frame{
            .label = label,
            .content = tui::progress_data{ .percent = percent, .status = status.str() },
            .terminal = done >= total });
  };
}

sha256_t sha256_tracked(std::filesystem::path const &file,
                        tui::section_handle section,
                        std::string const &row_label,
                        std::string verb,
                        std::optional<std::string> item) {
  return sha256(file,
                byte_progress_bar(section,
                                  row_label,
                                  std::move(verb),
                                  item ? std::move(*item) : file.filename().string()));
}

// ==== run_shell_with_progress ====

std::vector<fetch_result_t> fetch_tracked(std::vector<fetch_request> requests,
                                          std::string const &row_label,
                                          std::vector<std::string> const &item_labels,
                                          std::string trace_spec) {
  if (requests.empty()) { return {}; }

  tui::section_handle const section{ tui::section_create() };
  fetch_all_progress_tracker tracker{ section, row_label, item_labels, "fetch" };
  for (std::size_t i{ 0 }; i < requests.size(); ++i) {
    auto cb{ tracker.make_callback(i) };
    std::visit([&](auto &r) { r.progress = std::move(cb); }, requests[i]);
  }

  auto results{ fetch(requests, std::move(trace_spec)) };

  // A finished download is the command's record of itself, so its row commits to the
  // scrollback at 100%, above whatever the caller reports next. A failure has an error to
  // speak for it, so the row goes.
  if (std::ranges::all_of(results, [](fetch_result_t const &r) {
        return std::holds_alternative<fetch_result>(r);
      })) {
    tracker.finish();
    tui::section_commit(section);
  } else {
    tui::section_delete(section);
  }

  return results;
}

shell_result run_shell_with_progress(std::string_view script,
                                     tui::section_handle section,
                                     std::string const &pkg_identity,
                                     std::filesystem::path const &cache_root,
                                     shell_run_cfg cfg) {
  if (section) {
    run_progress progress{ section, pkg_identity, cache_root };
    progress.on_command_start(script);
    cfg.on_output_line = [&](std::string_view line) { progress.on_output_line(line); };
    return shell_run(script, cfg);
  }

  return shell_run(script, cfg);
}

// ==== run_phase_shell_script ====

void run_phase_shell_script(std::string_view script,
                            std::string_view phase_label,
                            std::filesystem::path const &cwd,
                            std::string const &identity,
                            resolved_shell shell,
                            tui::section_handle section,
                            std::filesystem::path const &cache_root) {
  std::ostringstream stdout_capture;
  std::ostringstream stderr_capture;

  shell_env_t env{ shell_getenv() };
  shell_run_cfg cfg{
    .on_stdout_line = [&](std::string_view line) { stdout_capture << line << '\n'; },
    .on_stderr_line = [&](std::string_view line) { stderr_capture << line << '\n'; },
    .cwd = cwd,
    .env = std::move(env),
    .shell = std::move(shell)
  };

  shell_result const result{
    run_shell_with_progress(script, section, identity, cache_root, std::move(cfg))
  };
  if (result.exit_code == 0) { return; }

  std::string const stdout_str{ stdout_capture.str() };
  if (!stdout_str.empty()) { tui::error("%s", stdout_str.c_str()); }

  std::string const stderr_str{ stderr_capture.str() };
  if (!stderr_str.empty()) {
    std::ostringstream oss;
    constexpr size_t kMaxStderrBytes{ 2048 };
    if (stderr_str.size() > kMaxStderrBytes) {
      oss << "... (truncated)\n"
          << std::string_view{ stderr_str }.substr(stderr_str.size() - kMaxStderrBytes);
    } else {
      oss << stderr_str;
    }
    tui::error("%s", oss.str().c_str());
  }

  std::string const suffix{
    result.signal ? " (terminated by signal " + std::to_string(*result.signal) + ")"
                  : " (exit code " + std::to_string(result.exit_code) + ")"
  };
  throw std::runtime_error(std::string{ phase_label } + " shell script failed for " +
                           identity + suffix);
}

}  // namespace envy::tui_actions
