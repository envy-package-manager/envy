#pragma once

#include "trace.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#if defined(__clang__) || defined(__GNUC__)
#define ENVY_TUI_PRINTF(idx, first) __attribute__((format(printf, idx, first)))
#else
#define ENVY_TUI_PRINTF(idx, first)
#endif

namespace envy::tui {

// Log severity. Trace events are a separate stream (see trace.h) and are NOT a
// log level — they bypass this threshold entirely.
enum class level { TUI_DEBUG, TUI_INFO, TUI_WARN, TUI_ERROR };

enum class trace_output_type { std_err, file };

struct trace_output_spec {
  trace_output_type type;
  std::optional<std::filesystem::path> file_path;
};

void init();
void configure_trace_outputs(std::vector<trace_output_spec> outputs);
void set_output_handler(std::function<void(std::string_view)> handler);
void run(std::optional<level> threshold = std::nullopt, bool decorated_logging = false);
void shutdown();

extern bool g_trace_enabled;

// Trace emission lives in trace.h (tui::trace(std::string spec, trace_event_t)).
void debug(char const *fmt, ...) ENVY_TUI_PRINTF(1, 2);
void info(char const *fmt, ...) ENVY_TUI_PRINTF(1, 2);
void warn(char const *fmt, ...) ENVY_TUI_PRINTF(1, 2);
void error(char const *fmt, ...) ENVY_TUI_PRINTF(1, 2);

void print_stdout(char const *fmt, ...) ENVY_TUI_PRINTF(1, 2);

bool is_tty();
void pause_rendering();
void resume_rendering();

struct scope {  // raii helper
  explicit scope(std::optional<level> threshold, bool decorated_logging);
  ~scope();

 private:
  bool active{ false };
};

// Ambient per-package log context. While in scope, debug/info/warn/error lines
// emitted on this thread are auto-prefixed "[<identity>] " so per-package
// narrative is attributable without every call site threading the identity
// through. Nests (restores the previous value on destruction). Does not affect
// print_stdout. The engine sets one around each package worker's phase steps, so
// calls made from cache/extract/etc. inherit it.
struct log_ctx_scope {
  explicit log_ctx_scope(std::string identity);
  ~log_ctx_scope();

  log_ctx_scope(log_ctx_scope const &) = delete;
  log_ctx_scope &operator=(log_ctx_scope const &) = delete;

 private:
  std::string previous_;
};

// This thread's ambient log context, empty when unset. The context is thread-local, so
// code that fans work out to its own threads has to read it on the spawning thread and
// reopen a log_ctx_scope inside each worker or those lines lose their attribution.
std::string const &log_ctx();

// Section progress API
using section_handle = unsigned;
inline constexpr section_handle kInvalidSection = 0;

struct progress_data {
  double percent;
  std::string status;
};

struct text_stream_data {
  std::vector<std::string> lines;
  std::size_t line_limit{ 0 };  // 0 = show all lines, N = show last N lines
  std::chrono::steady_clock::time_point start_time;  // For spinner animation
  std::string header_text;  // Text shown after spinner (e.g., flattened command)
};

struct spinner_data {
  std::string text;
  std::chrono::steady_clock::time_point start_time;
  std::chrono::milliseconds frame_duration{ 100 };
};

struct static_text_data {
  std::string text;
};

struct section_frame {
  std::string label;
  std::variant<progress_data, text_stream_data, spinner_data, static_text_data> content;
  std::vector<section_frame> children;  // Optional grouped children (indented render)
  std::string phase_label;              // Optional phase suffix for grouped parents
  // This row's last word. Off a TTY it prints as soon as it is set instead of waiting out
  // the throttle window, so a finished bar is not the frame that gets swallowed.
  bool terminal{ false };
};

// Helper for providers/tests that need the rendered label width for alignment.
std::size_t measure_label_width(section_frame const &frame);

section_handle section_create();
void section_set_content(section_handle h, section_frame const &frame);
void section_set_complete(section_handle h);
void section_delete(section_handle h);

// Land this row's current frame in the scrollback, in order with the log and stdout
// streams, and retire the row. A finished step's last frame belongs above whatever the
// command prints next, not pinned below it by the live region. The handle is spent, so a
// command with a wait apiece draws a row per item rather than reusing one.
void section_commit(section_handle h);
bool section_has_content(section_handle h);

// Drop every row, for handing the terminal to someone else: nothing may be painted back
// over output that is no longer ours. Pair with interactive_mode_guard to erase the area.
void sections_clear();

// Interactive mode API
void acquire_interactive_mode();
void release_interactive_mode();

class interactive_mode_guard {
 public:
  interactive_mode_guard();
  ~interactive_mode_guard();
};

#ifdef ENVY_UNIT_TEST
namespace test {
extern int g_terminal_width;
extern bool g_isatty;
extern std::chrono::steady_clock::time_point g_now;

std::string render_section_frame(section_frame const &frame);

// Helper functions for testing ANSI-aware line padding and truncation
int calculate_visible_length(std::string_view str);
std::string truncate_to_width_ansi_aware(std::string const &str, int target_width);
std::string pad_to_width(std::string const &str, int target_width);
}  // namespace test
#endif

}  // namespace envy::tui

#undef ENVY_TUI_PRINTF
