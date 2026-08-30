#include "tui.h"

#include "platform.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using envy::tui::level;

constexpr std::size_t kSeverityLabelWidth{ 3 };
constexpr std::chrono::milliseconds kRefreshIntervalMs{ 33 };  // 30fps

constexpr char const *kAnsiClearToEol{ "\x1b[K" };    // Clear from cursor to end of line
constexpr char const *kAnsiClearToEos{ "\x1b[0J" };   // Clear from cursor to end of screen
constexpr char const *kAnsiEnableWrap{ "\x1b[?7h" };  // Enable auto-wrap mode
constexpr char const *kAnsiDisableWrap{ "\x1b[?7l" };  // Disable auto-wrap mode
constexpr char const *kAnsiShowCursor{ "\x1b[?25h" };  // Show cursor
constexpr char const *kAnsiHideCursor{ "\x1b[?25l" };  // Hide cursor
constexpr char const *kAnsiCursorUpFmt{ "\x1b[%dF" };  // Move cursor up N lines

struct log_event {
  std::chrono::system_clock::time_point timestamp;
  envy::tui::level severity;
  std::string message;
};

struct stdout_event {
  std::string message;
};

using log_entry = std::variant<log_event, stdout_event, envy::trace_record>;

struct tui {
  std::queue<log_entry> messages;
  std::function<void(std::string_view)> output_handler;
  std::thread worker;
  std::mutex mutex;  // protects messages queue and cv
  std::condition_variable cv;
  std::atomic_bool stop_requested{ false };
  std::optional<envy::tui::level> level_threshold;
  bool decorated{ false };
  bool initialized{ false };
  bool trace_stderr{ false };
  std::FILE *trace_file{ nullptr };
} s_tui{};

struct section_state {
  unsigned handle;
  envy::tui::section_frame cached_frame;
  bool has_content{ false };  // True after first set_content call
  bool complete{ false };     // Suppresses fallback rendering

  std::string last_fallback_output;
  std::chrono::steady_clock::time_point last_fallback_print_time;
};

struct tui_progress_state {
  std::vector<section_state> sections;
  unsigned next_handle{ 1 };
  int last_line_count{ 0 };
  std::size_t max_label_width{ 0 };
  std::mutex interactive_mutex;
  bool interactive_paused{ false };
  bool enabled{ true };
} s_progress{};

bool envy::tui::g_trace_enabled{ false };

#ifdef ENVY_UNIT_TEST
namespace envy::tui::test {
int g_terminal_width{ 0 };
bool g_isatty{ true };
std::chrono::steady_clock::time_point g_now{};
}  // namespace envy::tui::test
#endif

namespace {

int get_terminal_width() {
#ifdef ENVY_UNIT_TEST
  if (envy::tui::test::g_terminal_width > 0) { return envy::tui::test::g_terminal_width; }
#endif

#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &csbi)) {
    return csbi.dwSize.X;
  }
  return 80;
#else
  struct winsize ws;
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { return ws.ws_col; }
  return 80;
#endif
}

bool is_ansi_supported() {
#ifdef ENVY_UNIT_TEST
  return envy::tui::test::g_isatty;
#endif

  if (!envy::platform::is_tty()) { return false; }

#ifdef _WIN32
  HANDLE const h{ GetStdHandle(STD_ERROR_HANDLE) };
  DWORD mode{ 0 };
  if (GetConsoleMode(h, &mode)) {
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (SetConsoleMode(h, mode)) { return true; }
  }
  return false;
#else
  char const *const term{ std::getenv("TERM") };
  return term && std::strcmp(term, "dumb") != 0;
#endif
}

std::chrono::steady_clock::time_point get_now() {
#ifdef ENVY_UNIT_TEST
  if (envy::tui::test::g_now.time_since_epoch().count() > 0) {
    return envy::tui::test::g_now;
  }
#endif
  return std::chrono::steady_clock::now();
}

// Calculate visible character count, ignoring ANSI escape sequences.
// Handles SGR (Select Graphic Rendition) sequences: ESC [ ... m
// Returns the number of printable characters that will appear on screen.
int calculate_visible_length(std::string_view str) {
  int visible_count{ 0 };
  bool in_escape{ false };

  for (std::size_t i = 0; i < str.size(); ++i) {
    char const c{ str[i] };

    if (c == '\x1b' && i + 1 < str.size() && str[i + 1] == '[') {
      // Start of ANSI escape sequence
      in_escape = true;
      ++i;  // Skip the '['
    } else if (in_escape) {
      // Inside escape sequence - look for terminator
      if (c == 'm') { in_escape = false; }
      // Otherwise stay in escape mode (consuming sequence chars)
    } else if (c == '\t') {
      // Tabs render as 8 columns (simplified - proper tab stops would be more complex)
      visible_count += 8;
    } else {
      // Regular printable character
      ++visible_count;
    }
  }

  return visible_count;
}

// Truncate a string to exactly `target_width` visible characters, preserving ANSI codes.
// Truncates at the last visible character that fits within target_width.
// Returns the truncated string with all ANSI escape sequences preserved.
std::string truncate_to_width_ansi_aware(std::string const &str, int target_width) {
  if (target_width <= 0) { return ""; }

  int visible_count{ 0 };
  bool in_escape{ false };
  std::size_t last_valid_pos{ 0 };

  for (std::size_t i = 0; i < str.size(); ++i) {
    char const c{ str[i] };

    if (c == '\x1b' && i + 1 < str.size() && str[i + 1] == '[') {
      in_escape = true;
      ++i;  // Skip the '['
    } else if (in_escape) {
      if (c == 'm') {
        in_escape = false;
        last_valid_pos = i + 1;  // Include the 'm'
      }
    } else if (c == '\t') {
      // Tabs render as 8 columns
      visible_count += 8;
      if (visible_count <= target_width) {
        last_valid_pos = i + 1;
      } else {
        // Exceeded width, truncate here
        break;
      }
    } else {
      // Regular visible character
      ++visible_count;
      if (visible_count <= target_width) {
        last_valid_pos = i + 1;
      } else {
        // Exceeded width, truncate here
        break;
      }
    }
  }

  return str.substr(0, last_valid_pos);
}

// Pad a string to exactly `target_width` visible characters by appending spaces.
// Preserves ANSI escape sequences while calculating visible length.
// If visible length already >= target_width, truncates to target_width.
std::string pad_to_width(std::string const &str, int target_width) {
  int const visible{ calculate_visible_length(str) };

  if (visible > target_width) {
    // Too long - truncate first, then pad
    std::string truncated{ truncate_to_width_ansi_aware(str, target_width) };
    int const truncated_visible{ calculate_visible_length(truncated) };
    if (truncated_visible < target_width) {
      // Truncation may result in fewer chars than target (e.g., tab boundaries)
      int const padding{ target_width - truncated_visible };
      return truncated + std::string(padding, ' ');
    }
    return truncated;
  } else if (visible == target_width) {
    // Exact fit
    return str;
  }

  // Too short - pad
  int const padding{ target_width - visible };
  return str + std::string(padding, ' ');
}

constexpr char const *kSpinnerFrames[]{ "|", "/", "-", "\\" };

std::string render_progress_bar(envy::tui::progress_data const &data,
                                std::string_view label,
                                std::size_t max_label_width,
                                int width) {
  constexpr int kBarChars{ 20 };
  int const filled{ static_cast<int>((data.percent / 100.0) * kBarChars) };

  std::ostringstream oss;
  oss << label;
  if (label.size() < max_label_width) {
    oss << std::string(max_label_width - label.size(), ' ');
  }

  // Right-justified percentage (3 chars: "  5%", " 42%", "100%")
  oss << " " << std::setw(3) << static_cast<int>(data.percent) << "%";

  // Progress bar
  oss << " [";
  for (int i{ 0 }; i < kBarChars; ++i) {
    if (i < filled) {
      oss << "=";
    } else if (i == filled) {
      oss << ">";
    } else {
      oss << " ";
    }
  }
  oss << "]";

  // Status text (downloaded amount) on the right
  if (!data.status.empty()) {
    // Truncate status if it would wrap the terminal width
    std::string const prefix{ oss.str() };
    int const base_len{ static_cast<int>(prefix.size()) +
                        1 };  // pending space before status
    int const available{ width > 0 ? width - base_len : std::numeric_limits<int>::max() };
    std::string status{ data.status };
    if (available > 0 && static_cast<int>(status.size()) > available) {
      if (available > 3) {
        status.resize(static_cast<std::size_t>(available - 3));
        status += "...";
      } else {
        status.resize(static_cast<std::size_t>(available));
      }
    }

    oss << " " << status;
  }

  oss << "\n";
  return oss.str();
}

std::string render_text_stream(envy::tui::text_stream_data const &data,
                               std::string_view label,
                               std::size_t max_label_width,
                               int width,
                               std::chrono::steady_clock::time_point now) {
  // Determine which lines to render
  std::size_t const num_lines{ data.lines.size() };
  std::size_t const start_idx{ data.line_limit > 0 && num_lines > data.line_limit
                                   ? num_lines - data.line_limit
                                   : 0 };

  // Compute spinner frame
  auto const elapsed{ now - data.start_time };
  auto const elapsed_ms{
    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
  };
  std::size_t const frame_index{ static_cast<std::size_t>((elapsed_ms / 100) % 4) };

  std::ostringstream oss;
  oss << label;
  if (label.size() < max_label_width) {
    oss << std::string(max_label_width - label.size(), ' ');
  }
  oss << " " << kSpinnerFrames[frame_index] << " "
      << (data.header_text.empty() ? "build output:" : data.header_text) << "\n";

  for (std::size_t i = start_idx; i < data.lines.size(); ++i) {
    oss << "   " << data.lines[i] << "\n";
  }

  return oss.str();
}

std::string render_spinner(envy::tui::spinner_data const &data,
                           std::string_view label,
                           std::size_t max_label_width,
                           int width,
                           std::chrono::steady_clock::time_point now) {
  auto const elapsed{ now - data.start_time };
  auto const elapsed_ms{
    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
  };
  std::size_t const frame_index{ static_cast<std::size_t>(
      (elapsed_ms / data.frame_duration.count()) % 4) };

  std::ostringstream oss;
  oss << label;
  if (label.size() < max_label_width) {
    oss << std::string(max_label_width - label.size(), ' ');
  }
  oss << " " << kSpinnerFrames[frame_index] << " " << data.text << "\n";

  return oss.str();
}

std::string render_static_text(envy::tui::static_text_data const &data,
                               std::string_view label,
                               std::size_t max_label_width,
                               int width) {
  std::ostringstream oss;
  oss << label;
  if (label.size() < max_label_width) {
    oss << std::string(max_label_width - label.size(), ' ');
  }
  oss << " " << data.text << "\n";

  return oss.str();
}

std::string render_section_frame_fallback(envy::tui::section_frame const &frame,
                                          std::chrono::steady_clock::time_point now) {
  if (!frame.children.empty()) {
    std::string output;
    auto parent_copy{ frame };
    parent_copy.children.clear();
    if (!parent_copy.phase_label.empty()) {
      parent_copy.label += " (" + parent_copy.phase_label + ")";
      parent_copy.phase_label.clear();
    }
    output += render_section_frame_fallback(parent_copy, now);

    for (auto const &child : frame.children) {
      auto child_copy{ child };
      child_copy.label = "  " + child_copy.label;
      output += render_section_frame_fallback(child_copy, now);
    }
    return output;
  }

  // Elapsed-time dot cycle, 1-4 dots, shared by the two animated rows.
  auto const dots{ [&](std::chrono::steady_clock::time_point start) {
    auto const secs{
      std::chrono::duration_cast<std::chrono::seconds>(now - start).count()
    };
    return std::string(static_cast<std::size_t>((secs % 4) + 1), '.');
  } };

  // One overload per alternative, so a new content type is a compile error here rather
  // than the silently-empty row an if-constexpr cascade's trailing return would give.
  return std::visit(
      envy::match{ [&](envy::tui::progress_data const &data) {
                    std::ostringstream oss;
                    oss << "[" << frame.label << "] " << data.status << ": " << std::fixed
                        << std::setprecision(1) << data.percent << "%\n";
                    return oss.str();
                  },
                   [&](envy::tui::text_stream_data const &data) {
                     std::size_t const start_idx{ (data.line_limit > 0 &&
                                                   data.lines.size() > data.line_limit)
                                                      ? data.lines.size() - data.line_limit
                                                      : 0 };
                     std::ostringstream oss;
                     oss << "[" << frame.label << "] " << dots(data.start_time) << " "
                         << (data.header_text.empty() ? "build output:" : data.header_text)
                         << "\n";
                     for (std::size_t i{ start_idx }; i < data.lines.size(); ++i) {
                       oss << "   " << data.lines[i] << "\n";
                     }
                     return oss.str();
                   },
                   [&](envy::tui::spinner_data const &data) {
                     std::ostringstream oss;
                     oss << "[" << frame.label << "] " << data.text
                         << dots(data.start_time) << "\n";
                     return oss.str();
                   },
                   [&](envy::tui::static_text_data const &data) {
                     std::ostringstream oss;
                     oss << "[" << frame.label << "] " << data.text << "\n";
                     return oss.str();
                   } },
      frame.content);
}

std::string render_section_frame(envy::tui::section_frame const &frame,
                                 std::size_t max_label_width,
                                 int width,
                                 bool ansi_mode,
                                 std::chrono::steady_clock::time_point now) {
  if (!ansi_mode) { return render_section_frame_fallback(frame, now); }

  if (!frame.children.empty()) {
    // Parent line (with optional phase suffix), then children indented by two spaces
    std::string output;
    auto parent_copy{ frame };
    parent_copy.children.clear();
    if (!parent_copy.phase_label.empty()) {
      parent_copy.label += " (" + parent_copy.phase_label + ")";
      parent_copy.phase_label.clear();
    }
    output += render_section_frame(parent_copy, max_label_width, width, ansi_mode, now);

    for (auto const &child : frame.children) {
      envy::tui::section_frame const child_copy{ .label = "  " + child.label,
                                                 .content = child.content,
                                                 .children = child.children,
                                                 .phase_label = child.phase_label };
      output += render_section_frame(child_copy, max_label_width, width, ansi_mode, now);
    }
    return output;
  }

  return std::visit(
      envy::match{
          [&](envy::tui::progress_data const &data) {
            return render_progress_bar(data, frame.label, max_label_width, width);
          },
          [&](envy::tui::text_stream_data const &data) {
            return render_text_stream(data, frame.label, max_label_width, width, now);
          },
          [&](envy::tui::spinner_data const &data) {
            return render_spinner(data, frame.label, max_label_width, width, now);
          },
          [&](envy::tui::static_text_data const &data) {
            return render_static_text(data, frame.label, max_label_width, width);
          } },
      frame.content);
}

int count_wrapped_lines(std::string const &text, int width_hint) {
  int const effective_width{ width_hint > 0 ? width_hint : 80 };
  int lines{ 0 };
  int col{ 0 };
  for (char c : text) {
    if (c == '\n') {
      ++lines;
      col = 0;
      continue;
    }
    ++col;
    if (col > effective_width) {
      ++lines;
      col = 1;  // Current char is first char of new line
    }
  }
  if (col > 0) { ++lines; }
  return lines;
}

std::string truncate_lines_to_width(std::string const &text, int width_hint) {
  if (width_hint <= 0) { return text; }

  std::string out;
  out.reserve(text.size());

  int col{ 0 };
  for (char c : text) {
    if (c == '\n') {
      out.push_back(c);
      col = 0;
      continue;
    }
    if (col < width_hint) {
      out.push_back(c);
      ++col;
    }
  }

  return out;
}

int render_progress_sections_ansi(std::vector<section_state> const &sections,
                                  std::size_t max_label_width,
                                  int last_line_count,
                                  int width,
                                  std::chrono::steady_clock::time_point now) {
  std::vector<std::string> rendered_lines;

  // Render each section and split into individual lines
  for (auto const &sec : sections) {
    if (!sec.has_content) { continue; }
    std::string frame{
      render_section_frame(sec.cached_frame, max_label_width, width, true, now)
    };

    // Split frame into lines and truncate each to terminal width
    std::istringstream iss{ frame };
    std::string line;
    while (std::getline(iss, line)) {
      rendered_lines.push_back(truncate_to_width_ansi_aware(line, width));
    }
  }

  std::fprintf(stderr, "%s", kAnsiDisableWrap);

  // Ensure column 0, then move up to start position
  std::fprintf(stderr, "\r");
  if (last_line_count > 1) { std::fprintf(stderr, kAnsiCursorUpFmt, last_line_count - 1); }

  // Render each line with per-line clear
  int cur_frame_line_count{ 0 };
  for (auto const &line : rendered_lines) {
    if (cur_frame_line_count > 0) {  // Print \n before all lines except first
      std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "%s%s", line.c_str(), kAnsiClearToEol);
    ++cur_frame_line_count;
  }

  // Clear remaining old lines if shrinking
  if (cur_frame_line_count < last_line_count) {
    if (cur_frame_line_count == 0) {  // cursor already parked on the first stale line
      std::fprintf(stderr, "%s", kAnsiClearToEos);
    } else {
      std::fprintf(stderr, "\n%s", kAnsiClearToEos);
      ++cur_frame_line_count;  // Account for the newline we just printed
    }
  }

  std::fprintf(stderr, "%s", kAnsiEnableWrap);
  std::fflush(stderr);

  return cur_frame_line_count;
}

void render_fallback_frame_unlocked(std::vector<section_state> const &sections,
                                    std::chrono::steady_clock::time_point now) {
  // Throttle: print only if changed and >= 2s elapsed
  static auto const kFallbackThrottle = [] {
#if defined(ENVY_FUNCTIONAL_TESTER)
    if (auto const *val = std::getenv("ENVY_TEST_FALLBACK_THROTTLE_MS")) {
      return std::chrono::milliseconds{ std::atoi(val) };
    }
#endif
    return std::chrono::milliseconds{ 2000 };
  }();

  struct update_info {
    unsigned handle;
    std::string output;
  };
  std::vector<update_info> updates;

  for (auto const &sec : sections) {
    if (!sec.has_content || sec.complete) { continue; }

    std::string const output{ render_section_frame_fallback(sec.cached_frame, now) };

    auto const elapsed{ now - sec.last_fallback_print_time };
    bool const due{ sec.cached_frame.terminal || elapsed >= kFallbackThrottle };
    if (output != sec.last_fallback_output && due) {
      std::fprintf(stderr, "%s", output.c_str());
      updates.push_back(update_info{ .handle = sec.handle, .output = output });
    }
  }

  std::fflush(stderr);

  // Update shared state (requires lock)
  if (!updates.empty()) {
    std::lock_guard lock{ s_tui.mutex };
    for (auto const &upd : updates) {
      for (auto &sec : s_progress.sections) {
        if (sec.handle == upd.handle) {
          sec.last_fallback_output = upd.output;
          sec.last_fallback_print_time = now;
          break;
        }
      }
    }
  }
}

}  // namespace

namespace {

std::string_view level_to_string(level value) {
  switch (value) {
    case level::TUI_DEBUG: return "DBG";
    case level::TUI_INFO: return "INF";
    case level::TUI_WARN: return "WRN";
    case level::TUI_ERROR: return "ERR";
  }
  return "UNKNOWN";
}

std::tm make_local_tm(std::time_t time) {
  std::tm result{};

#if defined(_WIN32)
  localtime_s(&result, &time);
#else
  localtime_r(&time, &result);
#endif

  return result;
}

// Decorated prefix "[ts] [LBL] " for the given 3-char label (a log level or the
// literal "TRC" for trace-to-stderr). Empty when decoration is off.
std::string format_prefix(std::string_view label) {
  if (!s_tui.decorated) { return {}; }

  auto const now{ std::chrono::system_clock::now() };
  auto const seconds{ std::chrono::time_point_cast<std::chrono::seconds>(now) };
  auto const millis{
    std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count()
  };

  std::time_t const timestamp{ std::chrono::system_clock::to_time_t(now) };
  std::tm const local_tm{ make_local_tm(timestamp) };

  char timestamp_buf[32]{};
  if (std::strftime(timestamp_buf,
                    sizeof(timestamp_buf),
                    "%Y-%m-%d %H:%M:%S",
                    &local_tm) == 0) {
    return {};
  }

  std::ostringstream oss;
  oss << '[' << timestamp_buf << '.' << std::setfill('0') << std::setw(3) << millis
      << "] [" << std::left << std::setfill(' ') << std::setw(kSeverityLabelWidth) << label
      << "] ";
  return oss.str();
}

void flush_messages(std::queue<log_entry> &pending,
                    std::function<void(std::string_view)> const &handler) {
  bool wrote_to_stderr{ false };

  while (!pending.empty()) {
    auto entry{ std::move(pending.front()) };
    pending.pop();

    if (auto *log_ptr{ std::get_if<log_event>(&entry) }) {
      std::string output;
      if (s_tui.decorated) {
        auto const prefix{ format_prefix(level_to_string(log_ptr->severity)) };
        output.reserve(prefix.size() + log_ptr->message.size() + 1);
        output.append(prefix);
      } else {
        // Undecorated (default): mark warnings/errors compiler-style so they are
        // visible and greppable amid unmarked INFO lines. INFO/DEBUG unprefixed.
        char const *tag{ log_ptr->severity == level::TUI_WARN    ? "warning: "
                         : log_ptr->severity == level::TUI_ERROR ? "error: "
                                                                 : "" };
        output.reserve(std::strlen(tag) + log_ptr->message.size() + 1);
        output.append(tag);
      }
      output.append(log_ptr->message);
      output.push_back('\n');

      if (handler) {
        handler(output);
      } else {
        if (!output.empty()) { std::fwrite(output.data(), 1, output.size(), stderr); }
        wrote_to_stderr = true;
      }
    } else if (auto *stdout_ptr{ std::get_if<stdout_event>(&entry) }) {
      if (!stdout_ptr->message.empty()) {
        std::fwrite(stdout_ptr->message.data(), 1, stdout_ptr->message.size(), stdout);
        std::fflush(stdout);
      }
    } else if (auto *trace_ptr{ std::get_if<envy::trace_record>(&entry) }) {
      if (s_tui.trace_stderr) {
        std::string output;
        if (s_tui.decorated) {
          auto const prefix{ format_prefix("TRC") };
          auto const trace_str{ envy::trace_record_to_string(*trace_ptr) };
          output.reserve(prefix.size() + trace_str.size() + 1);
          output.append(prefix);
          output.append(trace_str);
        } else {
          output = envy::trace_record_to_string(*trace_ptr);
        }
        output.push_back('\n');

        if (handler) {
          handler(output);
        } else {
          std::fwrite(output.data(), 1, output.size(), stderr);
          wrote_to_stderr = true;
        }
      }

      if (s_tui.trace_file) {
        auto const json{ envy::trace_record_to_json(*trace_ptr) + "\n" };
        if (std::fwrite(json.data(), 1, json.size(), s_tui.trace_file) != json.size() ||
            std::fflush(s_tui.trace_file) != 0) {
          std::fflush(stderr);
          std::fprintf(stderr, "Fatal: failed to write trace file\n");
          std::fflush(stderr);
          std::abort();
        }
      }
    }
  }

  if (!handler && wrote_to_stderr) { std::fflush(stderr); }
}

// Single render cycle: clear section area if needed, flush messages, render sections.
// Returns the number of section lines rendered.
int render_cycle(std::queue<log_entry> &pending,
                 std::vector<section_state> const &sections,
                 std::size_t max_label_width,
                 int last_line_count) {
  int rendered_line_count{ 0 };
  bool const ansi{ is_ansi_supported() };
  auto const now{ get_now() };
  bool const has_messages{ !pending.empty() };

  if (ansi && has_messages && last_line_count > 0) {
    std::fprintf(stderr, "\r");
    if (last_line_count > 1) {
      std::fprintf(stderr, kAnsiCursorUpFmt, last_line_count - 1);
    }
    std::fprintf(stderr, "%s", kAnsiClearToEos);
    std::fflush(stderr);
  }

  flush_messages(pending, s_tui.output_handler);

  if (s_progress.enabled) {
    int const width{ get_terminal_width() };

    if (ansi) {
      int const effective_last{ has_messages ? 0 : last_line_count };
      rendered_line_count = render_progress_sections_ansi(sections,
                                                          max_label_width,
                                                          effective_last,
                                                          width,
                                                          now);
    } else {
      render_fallback_frame_unlocked(sections, now);
    }
  }

  return rendered_line_count;
}

void worker_thread() {
  std::unique_lock<std::mutex> lock{ s_tui.mutex };

  while (!s_tui.stop_requested) {
    try {
      std::queue<log_entry> pending;
      pending.swap(s_tui.messages);

      std::vector<section_state> sections_snapshot;
      std::size_t max_label_width{ 0 };
      int last_line_count{ 0 };

      if (s_progress.enabled && !s_progress.interactive_paused) {
        sections_snapshot = s_progress.sections;
        max_label_width = s_progress.max_label_width;
        last_line_count = s_progress.last_line_count;
      }

      lock.unlock();

      int const rendered_line_count{
        render_cycle(pending, sections_snapshot, max_label_width, last_line_count)
      };

      lock.lock();

      if (is_ansi_supported() && s_progress.enabled && !s_progress.interactive_paused) {
        s_progress.last_line_count = rendered_line_count;
      }
      s_tui.cv.wait_until(lock, std::chrono::steady_clock::now() + kRefreshIntervalMs, [] {
        return s_tui.stop_requested.load();
      });
    } catch (std::exception const &e) {
      if (!lock.owns_lock()) { lock.lock(); }
      std::fprintf(stderr, "[TUI worker thread exception: %s]\n", e.what());
      std::fflush(stderr);
    } catch (...) {
      if (!lock.owns_lock()) { lock.lock(); }
      std::fprintf(stderr, "[TUI worker thread exception: unknown]\n");
      std::fflush(stderr);
    }
  }

  // Final render: same as regular render, plus trailing newline if sections rendered
  try {
    std::queue<log_entry> pending;
    pending.swap(s_tui.messages);

    std::vector<section_state> sections_snapshot;
    std::size_t max_label_width{ 0 };
    int last_line_count{ 0 };

    if (s_progress.enabled) {
      sections_snapshot = s_progress.sections;
      max_label_width = s_progress.max_label_width;
      last_line_count = s_progress.last_line_count;
    }

    lock.unlock();

    int const rendered_line_count{
      render_cycle(pending, sections_snapshot, max_label_width, last_line_count)
    };

    if (rendered_line_count > 0) {
      std::fprintf(stderr, "\n");
      std::fflush(stderr);
    }
  } catch (std::exception const &e) {
    std::fprintf(stderr, "[TUI final render exception: %s]\n", e.what());
    std::fflush(stderr);
  } catch (...) {
    std::fprintf(stderr, "[TUI final render exception: unknown]\n");
    std::fflush(stderr);
  }
}

thread_local std::string s_log_ctx;

void log_formatted(level severity, char const *fmt, va_list args) {
  if (!s_tui.initialized || fmt == nullptr) { return; }
  if (s_tui.level_threshold && severity < *s_tui.level_threshold) { return; }

  std::string buffer(1024, '\0');

  va_list args_copy;
  va_copy(args_copy, args);
  int written{ std::vsnprintf(buffer.data(), buffer.size(), fmt, args) };
  if (written <= 0) {
    va_end(args_copy);
    return;
  }

  if (static_cast<std::size_t>(written) >= buffer.size()) {
    buffer.resize(static_cast<std::size_t>(written) + 1);
    written = std::vsnprintf(buffer.data(), buffer.size(), fmt, args_copy);
  }
  va_end(args_copy);

  if (written <= 0) { return; }

  buffer.resize(static_cast<std::size_t>(written));
  if (buffer.empty()) { return; }

  // Ambient package context: "[identity] message". Applied here (on the caller's
  // thread) so the correct thread_local is read before the event is queued.
  if (!s_log_ctx.empty()) { buffer.insert(0, "[" + s_log_ctx + "] "); }

  log_event ev{ .timestamp = std::chrono::system_clock::now(),
                .severity = severity,
                .message = std::move(buffer) };

  {
    std::lock_guard<std::mutex> lock{ s_tui.mutex };
    s_tui.messages.push(log_entry{ std::move(ev) });
  }

  s_tui.cv.notify_one();
}

}  // namespace

namespace envy::tui {

void init() {
  if (s_tui.initialized) {
    throw std::logic_error{ "envy::tui::init called more than once" };
  }

  s_tui.level_threshold = std::nullopt;
  s_tui.decorated = false;
  s_tui.initialized = true;
  g_trace_enabled = false;
}

void configure_trace_outputs(std::vector<trace_output_spec> outputs) {
  if (!s_tui.initialized) {
    throw std::logic_error{ "envy::tui::configure_trace_outputs called before init" };
  }

  if (s_tui.worker.joinable()) {
    throw std::logic_error{ "envy::tui::configure_trace_outputs called while running" };
  }

  // Close previous file if any
  if (s_tui.trace_file) {
    std::fclose(s_tui.trace_file);
    s_tui.trace_file = nullptr;
  }

  s_tui.trace_stderr = false;

  for (auto const &spec : outputs) {
    if (spec.type == trace_output_type::std_err) {
      s_tui.trace_stderr = true;
    } else if (spec.type == trace_output_type::file && spec.file_path) {
      if (s_tui.trace_file) {
        throw std::logic_error{ "Only one trace file output supported" };
      }
      // util_open_file uses _wfopen on Windows so non-ASCII trace paths work.
      s_tui.trace_file = util_open_file(*spec.file_path, "w").release();
      if (!s_tui.trace_file) {
        throw std::runtime_error("Failed to open trace file: " + spec.file_path->string());
      }
    }
  }

  g_trace_enabled = s_tui.trace_stderr || s_tui.trace_file;

  // Header record: first line of every trace stream carries the schema version.
  ENVY_TRACE(trace_start, "", .schema = kTraceSchemaVersion);
}

void run(std::optional<level> threshold, bool decorated_logging) {
  if (!s_tui.initialized) {
    throw std::logic_error{ "envy::tui::run called before init" };
  }

  if (s_tui.worker.joinable()) {
    throw std::logic_error{ "envy::tui::run called while already running" };
  }

  s_tui.level_threshold = std::move(threshold);
  s_tui.decorated = decorated_logging;
  s_tui.stop_requested = false;
  s_tui.worker = std::thread{ worker_thread };
}

void shutdown() {
  if (!s_tui.worker.joinable()) {
    throw std::logic_error{ "envy::tui::shutdown called while not running" };
  }

  s_tui.stop_requested = true;
  s_tui.cv.notify_all();
  s_tui.worker.join();
  s_tui.worker = std::thread{};
  s_tui.stop_requested = false;
  g_trace_enabled = false;
  if (s_tui.trace_file) {
    std::fclose(s_tui.trace_file);
    s_tui.trace_file = nullptr;
  }
}

bool is_tty() { return platform::is_tty(); }

void trace(std::string spec, trace_event_t event) {
  if (!g_trace_enabled) { return; }

  static std::atomic<std::uint32_t> s_next_tid{ 0 };
  thread_local std::uint32_t const tid{ s_next_tid.fetch_add(1) };

  trace_record rec{ .seq = 0,
                    .ts = std::chrono::system_clock::now(),
                    .tid = tid,
                    .spec = std::move(spec),
                    .event = std::move(event) };
  {
    std::lock_guard<std::mutex> lock{ s_tui.mutex };
    // seq assigned under the queue mutex: file order == seq order, strictly.
    static std::uint64_t s_next_seq{ 0 };
    rec.seq = s_next_seq++;
    s_tui.messages.push(log_entry{ std::move(rec) });
  }
  s_tui.cv.notify_one();
}

void debug(char const *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_formatted(level::TUI_DEBUG, fmt, args);
  va_end(args);
}

void info(char const *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_formatted(level::TUI_INFO, fmt, args);
  va_end(args);
}

void warn(char const *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_formatted(level::TUI_WARN, fmt, args);
  va_end(args);
}

void error(char const *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_formatted(level::TUI_ERROR, fmt, args);
  va_end(args);
}

void print_stdout(char const *fmt, ...) {
  if (!s_tui.initialized || !fmt) { return; }

  std::string buffer(1024, '\0');

  va_list args;
  va_start(args, fmt);
  va_list args_copy;
  va_copy(args_copy, args);
  int written{ std::vsnprintf(buffer.data(), buffer.size(), fmt, args) };
  va_end(args);

  if (written <= 0) {
    va_end(args_copy);
    return;
  }

  if (static_cast<std::size_t>(written) >= buffer.size()) {
    buffer.resize(static_cast<std::size_t>(written) + 1);
    written = std::vsnprintf(buffer.data(), buffer.size(), fmt, args_copy);
  }
  va_end(args_copy);

  if (written <= 0) { return; }
  buffer.resize(static_cast<std::size_t>(written));

  {
    std::lock_guard<std::mutex> lock{ s_tui.mutex };
    s_tui.messages.push(log_entry{ stdout_event{ .message = std::move(buffer) } });
  }
  s_tui.cv.notify_one();
}

void pause_rendering() {
  std::lock_guard lock{ s_tui.mutex };
  if (!is_ansi_supported() || s_progress.last_line_count == 0) { return; }

  // Same arithmetic as the renderer, which leaves the cursor on the last row it drew: up
  // one *less* than the row count. The full count erases a line the region never owned.
  std::fprintf(stderr, "\r");
  if (s_progress.last_line_count > 1) {
    std::fprintf(stderr, kAnsiCursorUpFmt, s_progress.last_line_count - 1);
  }
  std::fprintf(stderr, "%s%s%s", kAnsiClearToEos, kAnsiEnableWrap, kAnsiShowCursor);
  s_progress.last_line_count = 0;
  std::fflush(stderr);
}

void resume_rendering() {
  // No mutex needed: only writes to stderr without accessing shared state
  // pause_rendering() needs mutex because it reads/modifies s_progress.last_line_count
  if (is_ansi_supported()) {
    std::fprintf(stderr, "%s", kAnsiHideCursor);
    std::fflush(stderr);
  }
  // Next render cycle will redraw
}

section_handle section_create() {
  if (!s_progress.enabled) { return 0; }  // Skip if disabled

  std::lock_guard lock{ s_tui.mutex };

  unsigned const handle{ s_progress.next_handle++ };
  s_progress.sections.push_back(
      section_state{ .handle = handle, .cached_frame = {}, .last_fallback_output = {} });

  return handle;
}

void section_set_content(section_handle h, section_frame const &frame) {
  if (h == 0 || !s_progress.enabled) { return; }

  std::lock_guard lock{ s_tui.mutex };

  if (auto it{ std::ranges::find_if(s_progress.sections,
                                    [h](auto const &sec) { return sec.handle == h; }) };
      it != s_progress.sections.end()) {
    it->cached_frame = frame;
    it->has_content = true;
    s_progress.max_label_width =
        std::max(s_progress.max_label_width, measure_label_width(frame));
  }
}

void section_set_complete(section_handle h) {
  if (h == 0 || !s_progress.enabled) { return; }

  std::lock_guard lock{ s_tui.mutex };

  if (auto it{ std::ranges::find_if(s_progress.sections,
                                    [h](auto const &sec) { return sec.handle == h; }) };
      it != s_progress.sections.end()) {
    it->complete = true;
  }
}

void sections_clear() {
  if (!s_progress.enabled) { return; }

  std::lock_guard lock{ s_tui.mutex };
  s_progress.sections.clear();
}

void section_delete(section_handle h) {
  if (h == 0 || !s_progress.enabled) { return; }

  std::lock_guard lock{ s_tui.mutex };

  auto it{ std::ranges::find_if(s_progress.sections,
                                [h](auto const &sec) { return sec.handle == h; }) };
  if (it != s_progress.sections.end()) { s_progress.sections.erase(it); }
}

bool section_has_content(section_handle h) {
  if (h == 0 || !s_progress.enabled) { return false; }

  std::lock_guard lock{ s_tui.mutex };

  auto it{ std::ranges::find_if(s_progress.sections,
                                [h](auto const &sec) { return sec.handle == h; }) };
  return it != s_progress.sections.end() && it->has_content;
}

void acquire_interactive_mode() {
  s_progress.interactive_mutex.lock();
  {
    std::lock_guard lock{ s_tui.mutex };
    s_progress.interactive_paused = true;
  }
  pause_rendering();
}

void release_interactive_mode() {
  {
    std::lock_guard lock{ s_tui.mutex };
    s_progress.interactive_paused = false;
  }
  resume_rendering();
  s_progress.interactive_mutex.unlock();
}

interactive_mode_guard::interactive_mode_guard() { acquire_interactive_mode(); }

interactive_mode_guard::~interactive_mode_guard() { release_interactive_mode(); }

void set_output_handler(std::function<void(std::string_view)> handler) {
  if (!s_tui.initialized) {
    throw std::logic_error{ "envy::tui::set_output_handler called before init" };
  }

  if (s_tui.worker.joinable() || s_tui.stop_requested) {
    throw std::logic_error{ "envy::tui::set_output_handler called while running" };
  }

  std::lock_guard<std::mutex> lock{ s_tui.mutex };
  s_tui.output_handler = std::move(handler);
}

scope::scope(std::optional<level> threshold, bool decorated_logging) {
  if (!s_tui.initialized) { return; }
  run(std::move(threshold), decorated_logging);
  active = true;

  // Hide cursor during TUI session (auto-wrap managed per-render in sections)
  if (is_ansi_supported()) {
    std::fprintf(stderr, "%s", kAnsiHideCursor);
    std::fflush(stderr);
  }
}

scope::~scope() {
  if (active) {
    shutdown();

    if (is_ansi_supported()) {
      std::fprintf(stderr, "%s%s", kAnsiEnableWrap, kAnsiShowCursor);
      std::fflush(stderr);
    }
  }
}

log_ctx_scope::log_ctx_scope(std::string identity) : previous_{ s_log_ctx } {
  s_log_ctx = std::move(identity);
}

log_ctx_scope::~log_ctx_scope() { s_log_ctx = std::move(previous_); }

std::string const &log_ctx() { return s_log_ctx; }

#ifdef ENVY_UNIT_TEST
namespace test {
std::string render_section_frame(section_frame const &frame) {
  int const width{ g_terminal_width > 0 ? g_terminal_width : 80 };
  auto const now{ g_now.time_since_epoch().count() > 0
                      ? g_now
                      : std::chrono::steady_clock::now() };
  std::size_t const max_width{ measure_label_width(frame) };
  return ::render_section_frame(frame, max_width, width, g_isatty, now);
}

int calculate_visible_length(std::string_view str) {
  return ::calculate_visible_length(str);
}

std::string truncate_to_width_ansi_aware(std::string const &str, int target_width) {
  return ::truncate_to_width_ansi_aware(str, target_width);
}

std::string pad_to_width(std::string const &str, int target_width) {
  return ::pad_to_width(str, target_width);
}
}  // namespace test
#endif

namespace {
std::size_t measure_label_width_impl(section_frame const &frame, std::size_t indent) {
  std::size_t const len{ indent + frame.label.size() +
                         (frame.phase_label.empty() ? 0 : frame.phase_label.size() + 3) };

  return std::accumulate(frame.children.begin(),
                         frame.children.end(),
                         len,
                         [&](std::size_t acc, auto const &child) {
                           return std::max(acc,
                                           measure_label_width_impl(child, indent + 2));
                         });
}
}  // namespace

std::size_t measure_label_width(section_frame const &frame) {
  return measure_label_width_impl(frame, 0);
}

}  // namespace envy::tui
