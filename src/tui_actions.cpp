#include "tui_actions.h"

#include "uri.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <functional>
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

tui::section_frame archive_progress_frame(std::string const &label,
                                          extract_progress const &prog,
                                          std::string_view suffix,
                                          bool terminal) {
  auto const percent_of{ [](std::uint64_t n, std::uint64_t d) {
    return d ? std::min(100.0, 100.0 * static_cast<double>(n) / static_cast<double>(d))
             : 0.0;
  } };
  std::string status{ std::to_string(prog.files_processed) };
  status += prog.totals ? "/" + std::to_string(prog.totals->files) + " files " +
                              util_format_bytes(prog.bytes_processed) + "/" +
                              util_format_bytes(prog.totals->bytes)
                        : std::string{ " files" };
  if (!suffix.empty()) { status.append(" ").append(suffix); }

  double const percent{
    terminal      ? 100.0
    : prog.totals ? prog.totals->bytes
                        ? percent_of(prog.bytes_processed, prog.totals->bytes)
                        : percent_of(prog.files_processed, prog.totals->files)
                  : percent_of(prog.input_bytes_processed, prog.input_bytes_total)
  };
  return { .label = label,
           .content = tui::progress_data{ .percent = percent, .status = std::move(status) },
           .terminal = terminal };
}

extract_progress_tracker::extract_progress_tracker(tui::section_handle section,
                                                   std::string const &pkg_identity,
                                                   std::vector<std::string> items,
                                                   std::uint64_t input_total,
                                                   std::optional<extract_totals> totals)
    : section_{ section },
      label_{ "[" + pkg_identity + "]" },
      suffix_{ items.size() == 1 ? std::move(items.front()) : std::string{} },
      done_{ .input_bytes_total = input_total, .totals = totals } {
  if (items.size() > 1) {
    for (auto &name : items) {
      children_.push_back({ .label = std::move(name),
                            .content = tui::static_text_data{ .text = "pending" } });
    }
  }
  publish(false);
}

bool extract_progress_tracker::operator()(extract_progress const &item) {
  item_ = item;
  publish(false);
  return true;
}

void extract_progress_tracker::begin(std::size_t idx) {
  if (idx >= children_.size()) { return; }
  current_ = idx;
  children_[idx].content = tui::spinner_data{ .text = "extracting",
                                              .start_time = std::chrono::steady_clock::now() };
  publish(false);
}

void extract_progress_tracker::end(std::uint64_t item_size) {
  done_.files_processed += item_.files_processed;
  done_.bytes_processed += item_.bytes_processed;
  done_.input_bytes_processed += item_size;  // the last read trails the archive's end
  item_ = {};
  if (current_) { children_[*current_].content = tui::static_text_data{ .text = "done" }; }
  current_.reset();
}

void extract_progress_tracker::finish() { publish(true); }

tui::section_frame extract_progress_tracker::frame(bool terminal) const {
  extract_progress p{ done_ };
  p.files_processed += item_.files_processed;
  p.bytes_processed += item_.bytes_processed;
  p.input_bytes_processed += item_.input_bytes_processed;
  if (terminal && p.totals) {  // everything is out, so what the archive stated is the count
    p.files_processed = p.totals->files;
    p.bytes_processed = p.totals->bytes;
  }
  auto f{ archive_progress_frame(label_, p, suffix_, terminal) };
  f.children = children_;
  return f;
}

void extract_progress_tracker::publish(bool terminal) const {
  if (section_) { tui::section_set_content(section_, frame(terminal)); }
}

std::uint64_t extract_tracked(std::filesystem::path const &archive_path,
                              std::filesystem::path const &destination,
                              extract_options options,
                              tui::section_handle section,
                              std::string const &pkg_identity,
                              std::string const &name) {
  if (!section) { return extract(archive_path, destination, options); }

  std::error_code ec;
  auto const size{ std::filesystem::file_size(archive_path, ec) };
  extract_progress_tracker tracker{ section,
                                    pkg_identity,
                                    { name },
                                    ec ? 0 : size,
                                    extract_probe_totals(archive_path, options) };
  options.progress = std::ref(tracker);
  auto const files{ extract(archive_path, destination, options) };
  tracker.finish();
  return files;
}

// ==== fetch_all_progress_tracker ====

namespace {

// Rounded up, so the countdown reaches "1s" rather than "0s" before the next attempt.
std::string retry_text(fetch_retry_progress const &prog) {
  auto const seconds{ std::chrono::duration_cast<std::chrono::seconds>(
                          prog.remaining + std::chrono::milliseconds{ 999 })
                          .count() };
  std::ostringstream oss;
  oss << "retry " << prog.attempt << " in " << seconds << "s (" << prog.reason << ")";
  return oss.str();
}

double percent_of(std::uint64_t part, std::uint64_t whole) {
  return whole ? std::min(100.0,
                          100.0 * static_cast<double>(part) / static_cast<double>(whole))
               : 0.0;
}

// One repository's row: a spinner until the remote has counted, then a bar over objects
// arriving and, after the last lands, over deltas resolving -- so it never stalls on a
// bare count. The callback keeps firing while the pack is indexed; follow the live phase.
tui::section_frame git_frame(std::string const &label,
                             fetch_git_progress const &p,
                             double receive_percent,
                             double delta_percent,
                             std::string_view suffix) {
  std::string const tail{ suffix.empty() ? "" : " " + std::string{ suffix } };
  if (p.total_objects == 0) {
    // Nothing is counted until the remote finishes negotiating, so the spinner carries
    // whatever bytes have arrived rather than a bare "starting...".
    static auto const epoch{ std::chrono::steady_clock::time_point{} };
    return { .label = label,
             .content = tui::spinner_data{
                 .text = (p.received_bytes > 0
                              ? util_format_bytes(p.received_bytes) + " negotiating"
                              : std::string{ "starting..." }) +
                         tail,
                 .start_time = epoch } };
  }

  if (p.checkout_total > 0) {  // the pack is in; the working tree is being written
    return { .label = label,
             .content = tui::progress_data{
                 .percent = percent_of(p.checkout_done, p.checkout_total),
                 .status = "checkout " + std::to_string(p.checkout_done) + "/" +
                           std::to_string(p.checkout_total) + " files" + tail } };
  }

  bool const receiving{ p.received_objects < p.total_objects };
  bool const resolving{ !receiving && p.total_deltas > 0 };
  std::ostringstream oss;
  if (resolving) {
    oss << p.indexed_deltas << "/" << p.total_deltas << " deltas";
  } else {
    oss << p.received_objects << "/" << p.total_objects << " objects";
    if (p.received_bytes > 0) { oss << " " << util_format_bytes(p.received_bytes); }
  }
  oss << tail;
  return { .label = label,
           .content = tui::progress_data{ .percent = resolving   ? delta_percent
                                                     : receiving ? receive_percent
                                                                 : 100.0,
                                          .status = oss.str() } };
}

}  // namespace

tui::section_frame fetch_git_tree_frame(std::string const &label,
                                        fetch_git_tree_progress const &prog,
                                        std::string_view suffix) {
  std::string status{ std::to_string(prog.done) + "/" + std::to_string(prog.total) +
                      " submodules" };
  if (prog.received_bytes > 0) { status += " " + util_format_bytes(prog.received_bytes); }
  if (!suffix.empty()) { status += " " + std::string{ suffix }; }

  // The bar may dip as nested submodules turn up: Y is a count of the found, not a plan.
  tui::section_frame frame{ .label = label,
                            .content = tui::progress_data{
                                .percent = percent_of(prog.done, prog.total),
                                .status = std::move(status) } };
  frame.children.reserve(prog.in_flight.size());
  for (auto const &repo : prog.in_flight) {
    auto const &c{ repo.counters };
    frame.children.push_back(
        repo.retry
            ? tui::section_frame{ .label = repo.path,
                                  .content =
                                      tui::spinner_data{ .text = retry_text(*repo.retry),
                                                         .start_time = {} } }
            : git_frame(repo.path,
                        c,
                        percent_of(c.received_objects, c.total_objects),
                        percent_of(c.indexed_deltas, c.total_deltas),
                        {}));
  }
  return frame;
}

fetch_all_progress_tracker::fetch_all_progress_tracker(
    tui::section_handle section,
    std::string const &pkg_identity,
    std::vector<std::string> const &labels,
    std::string group_text)
    : section_{ section },
      label_{ "[" + pkg_identity + "]" },
      group_text_{ std::move(group_text) },
      start_time_{ std::chrono::steady_clock::now() },
      labels_{ labels },
      mutex_{},
      children_{},
      git_states_(labels.size()),
      slot_totals_(labels.size()),
      grouped_{ labels.size() > 1 } {
  children_.reserve(labels.size());
  for (auto const &label : labels_) {
    children_.push_back(tui::section_frame{
        .label = label,
        .content = tui::spinner_data{ .text = label, .start_time = start_time_ } });
  }

  // Render the initial state so the row is up during slow setup (e.g. AWS init).
  publish_unlocked(false);
}

fetch_progress_cb_t fetch_all_progress_tracker::make_callback(std::size_t slot) {
  return [this, slot](fetch_progress_t const &prog) -> bool {
    envy::visit(
        envy::match{ [&](fetch_transfer_progress const &p) { update_transfer(slot, p); },
                     [&](fetch_git_progress const &p) { update_git(slot, p); },
                     [&](fetch_git_tree_progress const &p) { update_tree(slot, p); },
                     [&](fetch_retry_progress const &p) { update_retry(slot, p); } },
        prog);
    return true;
  };
}

void fetch_all_progress_tracker::update_transfer(std::size_t slot,
                                                 fetch_transfer_progress const &prog) {
  if (slot >= labels_.size()) { return; }

  std::string const &item_label{ labels_[slot] };

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

// A backoff has nothing to count, so it rides a spinner and names what it is waiting on:
// a row frozen at its last byte count is indistinguishable from a stalled transfer.
void fetch_all_progress_tracker::update_retry(std::size_t slot,
                                              fetch_retry_progress const &prog) {
  if (slot >= labels_.size()) { return; }

  std::string const &item_label{ labels_[slot] };
  std::string const text{ retry_text(prog) + (grouped_ ? "" : " " + item_label) };

  set_frame(slot,
            tui::section_frame{
                .label = item_label,
                .content = tui::spinner_data{ .text = text, .start_time = start_time_ } });
}

void fetch_all_progress_tracker::update_git(std::size_t slot,
                                            fetch_git_progress const &prog) {
  if (slot >= labels_.size()) { return; }

  std::uint32_t snapshot_total{ 0 };
  std::uint32_t snapshot_received{ 0 };
  std::uint32_t snapshot_total_deltas{ 0 };
  std::uint32_t snapshot_deltas{ 0 };
  std::uint64_t snapshot_bytes{ 0 };
  double receive_percent{ 0.0 };
  double delta_percent{ 0.0 };
  std::string const &child_label{ labels_[slot] };

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
  }

  set_frame(slot,
            git_frame(child_label,
                      fetch_git_progress{ .total_objects = snapshot_total,
                                          .received_objects = snapshot_received,
                                          .total_deltas = snapshot_total_deltas,
                                          .indexed_deltas = snapshot_deltas,
                                          .received_bytes = snapshot_bytes,
                                          .checkout_done = prog.checkout_done,
                                          .checkout_total = prog.checkout_total },
                      receive_percent,
                      delta_percent,
                      grouped_ ? std::string_view{} : std::string_view{ child_label }));
}

void fetch_all_progress_tracker::update_tree(std::size_t slot,
                                             fetch_git_tree_progress const &prog) {
  if (slot >= labels_.size()) { return; }
  {
    std::lock_guard const lock{ mutex_ };
    slot_totals_[slot].is_git = true;
    slot_totals_[slot].submodules = prog.total;
    git_states_[slot].last_bytes = prog.received_bytes;  // the tree's, for the final frame
  }
  std::string const &item_label{ labels_[slot] };
  set_frame(slot,
            fetch_git_tree_frame(
                item_label,
                prog,
                grouped_ ? std::string_view{} : std::string_view{ item_label }));
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
  std::string const &item_label{ labels_[slot] };

  std::string status{ [&] {
    if (total.is_git && total.submodules > 0) {  // a tree counts repositories
      auto const count{ std::to_string(total.submodules) };
      auto const bytes{ git_states_[slot].last_bytes };
      return count + "/" + count + " submodules" +
             (bytes > 0 ? " " + util_format_bytes(bytes) : "");
    }
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

void fetch_all_progress_tracker::finish(std::vector<fetch_result_t> const &results) {
  std::lock_guard const lock{ mutex_ };

  for (std::size_t slot{ 0 }; slot < children_.size(); ++slot) {
    if (slot < results.size() && !std::holds_alternative<fetch_result>(results[slot])) {
      continue;
    }
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
    envy::visit([&](auto &r) { r.progress = std::move(cb); }, requests[i]);
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

fetch_result_t fetch_on_row(fetch_request req,
                            tui::section_handle section,
                            std::string const &identity,
                            std::string const &url) {
  fetch_all_progress_tracker tracker{ section,
                                      identity,
                                      { uri_extract_filename(url) },
                                      "fetch" };
  envy::visit([&](auto &r) { r.progress = tracker.make_callback(0); }, req);

  auto results{ fetch({ std::move(req) }, identity) };
  if (results.empty()) { return std::string{ "no results" }; }
  if (std::holds_alternative<fetch_result>(results.front())) { tracker.finish(); }
  return std::move(results.front());
}

// ==== outcome rows ====

std::string timed_outcome(std::string_view text,
                          std::chrono::steady_clock::duration elapsed) {
  char buf[32]{};
  std::snprintf(buf,
                sizeof(buf),
                " (%.1fs)",
                std::chrono::duration<double>(elapsed).count());
  return std::string{ text } + buf;
}

void report_outcome(tui::section_handle section,
                    std::string const &identity,
                    std::string const &text,
                    bool keep_row,
                    std::string const &display) {
  if (keep_row) {
    tui::section_set_outcome(
        section,
        tui::section_frame{ .label = "[" + identity + "]",
                            .content = tui::static_text_data{ .text = text } });
  } else {
    tui::section_delete(section);
  }

  // Off a TTY no live row carries it, so it is an INFO line; the ambient log context
  // prefixes "[identity]". On a TTY the row is the only output: no duplicate scrollback.
  if (!tui::is_tty()) {
    std::string const line{ display.empty() ? text : display + " " + text };
    tui::info("%s", line.c_str());
  }
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
                            std::filesystem::path const &cache_root,
                            shell_env_t env) {
  std::ostringstream stdout_capture;
  std::ostringstream stderr_capture;

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
  throw_runtime_error({ phase_label, " shell script failed for ", identity, suffix });
}

}  // namespace envy::tui_actions
