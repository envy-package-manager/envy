#if !defined(_WIN32)
#error "shell_win.cpp should only be compiled on Windows builds"
#else

#include "shell.h"

#include "platform.h"
#include "tui.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

namespace envy {
namespace {

constexpr DWORD kPipeBufferSize{ 4096 };
constexpr DWORD kPipePollIntervalMs{ 10 };  // idle latency before re-peeking a pipe

HANDLE g_job_object{ NULL };

class handle_closer {
 public:
  handle_closer() = default;
  explicit handle_closer(HANDLE handle) : handle_{ handle } {}
  ~handle_closer() {
    if (handle_) { ::CloseHandle(handle_); }
  }

  handle_closer(handle_closer const &) = delete;
  handle_closer &operator=(handle_closer const &) = delete;

  HANDLE get() const { return handle_; }
  HANDLE release() {
    HANDLE tmp{ handle_ };
    handle_ = nullptr;
    return tmp;
  }
  void reset(HANDLE handle = nullptr) {
    if (handle_ && handle_ != handle) { ::CloseHandle(handle_); }
    handle_ = handle;
  }

 private:
  HANDLE handle_{ nullptr };
};

std::wstring utf8_to_wstring(std::string_view input) {
  if (input.empty()) { return {}; }
  // Use MB_ERR_INVALID_CHARS to detect malformed UTF-8; fall back to permissive mode on
  // error
  int required{ ::MultiByteToWideChar(CP_UTF8,
                                      MB_ERR_INVALID_CHARS,
                                      input.data(),
                                      static_cast<int>(input.size()),
                                      nullptr,
                                      0) };
  if (required == 0) {
    DWORD const err{ ::GetLastError() };
    if (err == ERROR_NO_UNICODE_TRANSLATION) {
      // Invalid UTF-8 sequence; retry without strict validation (replaces with U+FFFD)
      required = ::MultiByteToWideChar(CP_UTF8,
                                       0,
                                       input.data(),
                                       static_cast<int>(input.size()),
                                       nullptr,
                                       0);
      if (required == 0) {
        DWORD const err2{ ::GetLastError() };
        // Distinguish Unicode errors from other failures (buffer size, etc.)
        if (err2 == ERROR_NO_UNICODE_TRANSLATION) {
          throw std::system_error(
              err2,
              std::system_category(),
              "MultiByteToWideChar (permissive): invalid Unicode translation");
        } else {
          throw std::system_error(err2,
                                  std::system_category(),
                                  "MultiByteToWideChar (permissive)");
        }
      }
    } else {
      throw std::system_error(err, std::system_category(), "MultiByteToWideChar");
    }
  }
  std::wstring result;
  result.resize(static_cast<size_t>(required));
  int const converted{ ::MultiByteToWideChar(CP_UTF8,
                                             0,
                                             input.data(),
                                             static_cast<int>(input.size()),
                                             result.data(),
                                             required) };
  if (converted == 0) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "MultiByteToWideChar");
  }
  return result;
}

std::string wstring_to_utf8(std::wstring_view input) {
  if (input.empty()) { return {}; }
  // WC_ERR_INVALID_CHARS fails on unpaired surrogates; use permissive mode (flag 0)
  // which replaces unmappable chars with U+FFFD or default char
  int const required{ ::WideCharToMultiByte(CP_UTF8,
                                            0,
                                            input.data(),
                                            static_cast<int>(input.size()),
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr) };
  if (required == 0) {
    // Conversion error; pipe output may contain invalid wide chars (rare)
    // Return empty string rather than crash - caller will get truncated output
    DWORD const err{ ::GetLastError() };
    if (err == ERROR_NO_UNICODE_TRANSLATION) { return {}; }
    throw std::system_error(err, std::system_category(), "WideCharToMultiByte");
  }
  std::string result;
  result.resize(static_cast<size_t>(required));
  int const converted{ ::WideCharToMultiByte(CP_UTF8,
                                             0,
                                             input.data(),
                                             static_cast<int>(input.size()),
                                             result.data(),
                                             required,
                                             nullptr,
                                             nullptr) };
  if (converted == 0) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "WideCharToMultiByte");
  }
  return result;
}

// Split script into lines, preserving line content but normalizing line endings.
std::vector<std::wstring> split_script_lines(std::wstring_view script) {
  std::vector<std::wstring> lines;
  std::wstring current_line;

  for (size_t i{ 0 }; i < script.size(); ++i) {
    wchar_t const ch{ script[i] };
    if (ch == L'\r') {
      lines.push_back(std::move(current_line));
      current_line.clear();
      if (i + 1 < script.size() && script[i + 1] == L'\n') { ++i; }
    } else if (ch == L'\n') {
      lines.push_back(std::move(current_line));
      current_line.clear();
    } else {
      current_line.push_back(ch);
    }
  }
  if (!current_line.empty()) { lines.push_back(std::move(current_line)); }
  return lines;
}

bool is_powershell_line_empty_or_comment(std::wstring_view line) {
  for (wchar_t const ch : line) {
    if (ch == L' ' || ch == L'\t') { continue; }
    if (ch == L'#') { return true; }
    return false;
  }
  return true;
}

// Build script contents for PowerShell with optional fail-fast behavior.
// When check=true, wraps each line with $LASTEXITCODE check to exit immediately on
// failure.
std::wstring build_powershell_script_contents(std::string_view script, bool check) {
  std::wstring user_script{ utf8_to_wstring(script) };
  auto lines{ split_script_lines(user_script) };

  std::wstring wrapper;
  wrapper.reserve(user_script.size() * 2 + 256);

  if (check) {
    // Stop on PowerShell cmdlet errors immediately
    wrapper.append(L"$ErrorActionPreference = 'Stop'\r\n");
    wrapper.append(L"$Error.Clear()\r\n");

    // For PowerShell 7.3+, native commands also respect ErrorActionPreference
    wrapper.append(
        L"if ($PSVersionTable.PSVersion.Major -ge 7 -and $PSVersionTable.PSVersion.Minor "
        L"-ge "
        L"3) {\r\n");
    wrapper.append(L"  $PSNativeCommandUseErrorActionPreference = $true\r\n");
    wrapper.append(L"}\r\n");
  }

  // Emit each line, optionally followed by exit-code check for native commands
  for (auto const &line : lines) {
    if (is_powershell_line_empty_or_comment(line)) {
      wrapper.append(line);
      wrapper.append(L"\r\n");
    } else {
      wrapper.append(line);
      wrapper.append(L"\r\n");
      if (check) {
        // Check $LASTEXITCODE after each non-empty line for native command failures.
        // $LASTEXITCODE is only set by native commands, not by PowerShell cmdlets.
        wrapper.append(
            L"if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { exit $LASTEXITCODE "
            L"}\r\n");
      }
    }
  }

  if (check) {
    // Final error check for any accumulated PowerShell errors
    wrapper.append(L"if ($Error.Count -gt 0) { exit 1 }\r\n");
  }
  wrapper.append(L"exit 0\r\n");
  return wrapper;
}

// Build script contents for cmd.exe with optional fail-fast behavior.
// When check=true, appends "|| exit /b !errorlevel!" after each command line.
std::string build_cmd_script_contents(std::string_view script, bool check) {
  std::wstring wide_script{ utf8_to_wstring(script) };
  auto lines{ split_script_lines(wide_script) };

  std::string result;
  result.reserve(script.size() * 2 + 64);

  // Disable command echo for cleaner output
  result.append("@echo off\r\n");

  if (check) {
    // Enable delayed expansion for !errorlevel! in fail-fast checks
    result.append("setlocal enabledelayedexpansion\r\n");
  }

  for (auto const &wide_line : lines) {
    std::string line{ wstring_to_utf8(wide_line) };

    // Trim leading whitespace to check for empty/special lines
    std::string_view trimmed{ line };
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
      trimmed.remove_prefix(1);
    }

    // Skip empty lines, labels (:label), comments (REM/::), @echo off, and exit commands
    bool const is_empty{ trimmed.empty() };
    bool const is_label{ !trimmed.empty() && trimmed.front() == ':' &&
                         (trimmed.size() == 1 || trimmed[1] != ':') };
    bool const is_rem{ trimmed.size() >= 3 &&
                       (trimmed.substr(0, 3) == "rem" || trimmed.substr(0, 3) == "REM" ||
                        trimmed.substr(0, 3) == "Rem") &&
                       (trimmed.size() == 3 || trimmed[3] == ' ' || trimmed[3] == '\t') };
    bool const is_comment{ trimmed.size() >= 2 && trimmed.substr(0, 2) == "::" };
    bool const is_echo_off{ trimmed.size() >= 9 && (trimmed.substr(0, 9) == "@echo off" ||
                                                    trimmed.substr(0, 9) == "@ECHO OFF" ||
                                                    trimmed.substr(0, 9) == "@Echo Off") };
    bool const is_exit{
      trimmed.size() >= 4 &&
      (trimmed.substr(0, 4) == "exit" || trimmed.substr(0, 4) == "EXIT" ||
       trimmed.substr(0, 4) == "Exit") &&
      (trimmed.size() == 4 || trimmed[4] == ' ' || trimmed[4] == '\t' || trimmed[4] == '/')
    };

    if (is_empty || is_label || is_rem || is_comment || is_echo_off || is_exit) {
      result.append(line);
      result.append("\r\n");
    } else {
      result.append(line);
      if (check) {
        // Append fail-fast suffix: exit immediately if command fails
        result.append(" || exit /b !errorlevel!\r\n");
      } else {
        result.append("\r\n");
      }
    }
  }

  return result;
}

std::filesystem::path create_temp_script(std::string_view script,
                                         shell_run_cfg const &inv) {
  wchar_t temp_dir[MAX_PATH + 1];
  DWORD const dir_len{ ::GetTempPathW(MAX_PATH, temp_dir) };
  if (dir_len == 0 || dir_len > MAX_PATH) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "GetTempPathW failed");
  }

  // Generate unique filename directly without GetTempFileNameW to avoid zero-byte file
  // creation that can trigger sharing violations from antivirus/indexers
  static std::atomic<uint64_t> counter{ 0 };
  DWORD const pid{ ::GetCurrentProcessId() };
  ULONGLONG const tick{ ::GetTickCount64() };
  uint64_t const seq{ counter.fetch_add(1, std::memory_order_relaxed) };

  // Determine extension based on shell type
  std::wstring ext{ std::visit(
      match{
          [](shell_choice const &shell_cfg) -> std::wstring {
            return shell_cfg == shell_choice::powershell ? L".ps1" : L".cmd";
          },
          [](custom_shell_file const &shell_cfg) -> std::wstring {
            return utf8_to_wstring(shell_cfg.ext);
          },
          [](custom_shell_inline const &) -> std::wstring {
            return L".tmp";  // Generic extension for inline mode temp files
          },
      },
      inv.shell) };

  std::wstring const filename{ L"env" + std::to_wstring(pid) + L"_" +
                               std::to_wstring(tick) + L"_" + std::to_wstring(seq) + ext };
  std::filesystem::path script_path{ std::wstring{ temp_dir } + filename };

  // Create file with retry on sharing violation
  HANDLE file{ INVALID_HANDLE_VALUE };
  for (int retry{ 0 }; retry < 3; ++retry) {
    file = ::CreateFileW(script_path.c_str(),
                         GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_DELETE,
                         nullptr,
                         CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    if (file != INVALID_HANDLE_VALUE) { break; }

    DWORD const err{ ::GetLastError() };
    if ((err == ERROR_SHARING_VIOLATION || err == ERROR_FILE_EXISTS) && retry < 2) {
      ::Sleep(10);  // Brief wait before retry
      continue;
    }
    throw std::system_error(err, std::system_category(), "CreateFileW failed");
  }
  handle_closer file_guard{ file };

  DWORD written{ 0 };

  std::visit(
      match{
          [&](shell_choice const &shell_cfg) {
            if (shell_cfg == shell_choice::powershell) {
              // UTF-16 BOM + UTF-16 LE content
              std::wstring const content{ build_powershell_script_contents(script,
                                                                           inv.check) };
              wchar_t const bom{ 0xFEFF };
              if (!::WriteFile(file_guard.get(), &bom, sizeof(bom), &written, nullptr) ||
                  written != sizeof(bom)) {
                throw std::system_error(::GetLastError(),
                                        std::system_category(),
                                        "WriteFile failed");
              }
              if (!content.empty()) {
                DWORD const byte_count{ static_cast<DWORD>(content.size() *
                                                           sizeof(wchar_t)) };
                if (!::WriteFile(file_guard.get(),
                                 content.data(),
                                 byte_count,
                                 &written,
                                 nullptr) ||
                    written != byte_count) {
                  throw std::system_error(::GetLastError(),
                                          std::system_category(),
                                          "WriteFile failed");
                }
              }
            } else {  // cmd
              // cmd.exe UTF-8 support: Windows 10 build 17134+ supports UTF-8 (CP_UTF8)
              // natively. Older versions use system codepage (CP1252, CP932, etc.) which
              // breaks non-ASCII. This implementation requires Windows 10+; non-ASCII on
              // older versions will fail.
              std::string const content{ build_cmd_script_contents(script, inv.check) };
              if (!content.empty()) {
                DWORD const byte_count{ static_cast<DWORD>(content.size()) };
                if (!::WriteFile(file_guard.get(),
                                 content.data(),
                                 byte_count,
                                 &written,
                                 nullptr) ||
                    written != byte_count) {
                  throw std::system_error(::GetLastError(),
                                          std::system_category(),
                                          "WriteFile failed");
                }
              }
            }
          },
          [&](custom_shell_file const &) {
            // Write UTF-8 without BOM for custom shells
            std::string content{ script };
            if (!content.empty()) {
              DWORD const byte_count{ static_cast<DWORD>(content.size()) };
              if (!::WriteFile(file_guard.get(),
                               content.data(),
                               byte_count,
                               &written,
                               nullptr) ||
                  written != byte_count) {
                throw std::system_error(::GetLastError(),
                                        std::system_category(),
                                        "WriteFile failed");
              }
            }
          },
          [&](custom_shell_inline const &) {
            // Write UTF-8 without BOM for custom shells
            std::string content{ script };
            if (!content.empty()) {
              DWORD const byte_count{ static_cast<DWORD>(content.size()) };
              if (!::WriteFile(file_guard.get(),
                               content.data(),
                               byte_count,
                               &written,
                               nullptr) ||
                  written != byte_count) {
                throw std::system_error(::GetLastError(),
                                        std::system_category(),
                                        "WriteFile failed");
              }
            }
          },
      },
      inv.shell);

  if (!::FlushFileBuffers(file_guard.get())) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "FlushFileBuffers failed");
  }

  return script_path;
}

std::vector<wchar_t> build_environment_block(shell_env_t const &env) {
  // Inherit parent when no overrides.
  if (env.empty()) { return {}; }

  // Merge parent + overrides (Windows env vars are case-insensitive).
  shell_env_t merged{ shell_getenv() };
  for (auto const &[override_key, override_value] : env) {
    // Find and replace existing entry case-insensitively
    auto it{ std::find_if(merged.begin(),
                          merged.end(),
                          [&override_key](auto const &entry) {
                            return ::_stricmp(entry.first.c_str(), override_key.c_str()) ==
                                   0;
                          }) };

    if (it != merged.end()) {
      // Replace existing entry (preserve override's case for key)
      merged.erase(it);
    }
    merged[override_key] = override_value;
  }

  std::vector<wchar_t> block{};
  for (auto const &[key, value] : merged) {
    std::wstring wkey{ utf8_to_wstring(key) };
    std::wstring wvalue{ utf8_to_wstring(value) };
    block.insert(block.end(), wkey.begin(), wkey.end());
    block.push_back(L'=');
    block.insert(block.end(), wvalue.begin(), wvalue.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

struct pipe_state {
  HANDLE handle;
  shell_stream stream;
  std::string pending;
  size_t offset;
  bool closed;
};

void dispatch_line(std::string_view line, shell_stream stream, shell_run_cfg const &cfg) {
  if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
  while (!line.empty() && line.back() == ' ') { line.remove_suffix(1); }
  if (stream == shell_stream::std_out) {
    if (cfg.on_stdout_line) { cfg.on_stdout_line(line); }
  } else {
    if (cfg.on_stderr_line) { cfg.on_stderr_line(line); }
  }
  if (cfg.on_output_line) { cfg.on_output_line(line); }
}

void emit_complete_lines(pipe_state &pipe, shell_run_cfg const &cfg) {
  size_t newline{ 0 };
  while ((newline = pipe.pending.find('\n', pipe.offset)) != std::string::npos) {
    dispatch_line({ pipe.pending.data() + pipe.offset, newline - pipe.offset },
                  pipe.stream,
                  cfg);
    pipe.offset = newline + 1;
  }

  if (pipe.offset > kPipeBufferSize) {  // compact once the consumed prefix grows large
    pipe.pending.erase(0, pipe.offset);
    pipe.offset = 0;
  }
}

void close_pipe(pipe_state &pipe, shell_run_cfg const &cfg) {
  if (pipe.offset < pipe.pending.size()) {  // trailing partial line
    dispatch_line({ pipe.pending.data() + pipe.offset, pipe.pending.size() - pipe.offset },
                  pipe.stream,
                  cfg);
  }
  pipe.pending.clear();
  pipe.offset = 0;
  pipe.closed = true;
}

// Dispatches everything currently buffered, never blocking. Returns false at pipe EOF.
bool read_available(pipe_state &pipe, shell_run_cfg const &cfg, bool &read_any) {
  std::array<char, kPipeBufferSize> buffer{};

  while (true) {
    DWORD available{ 0 };
    if (!::PeekNamedPipe(pipe.handle, nullptr, 0, nullptr, &available, nullptr)) {
      DWORD const err{ ::GetLastError() };
      if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) { return false; }
      throw std::system_error(err, std::system_category(), "PeekNamedPipe failed");
    }
    if (available == 0) { return true; }

    DWORD read_bytes{ 0 };
    DWORD const want{ static_cast<DWORD>(  // bounded, so ReadFile never blocks
        std::min<size_t>(available, buffer.size())) };
    if (!::ReadFile(pipe.handle, buffer.data(), want, &read_bytes, nullptr)) {
      DWORD const err{ ::GetLastError() };
      if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) { return false; }
      throw std::system_error(err, std::system_category(), "ReadFile failed");
    }
    if (read_bytes == 0) { return false; }

    read_any = true;
    pipe.pending.append(buffer.data(), read_bytes);
    emit_complete_lines(pipe, cfg);
  }
}

// The child is gone; flush what is already buffered instead of waiting for EOF, which a
// surviving descendant holding an inherited write end would delay for its own lifetime.
void drain_after_exit(std::array<pipe_state, 2> &pipes, shell_run_cfg const &cfg) {
  bool inherited{ false };

  for (auto &pipe : pipes) {
    if (pipe.closed) { continue; }
    bool read_any{ false };
    inherited = read_available(pipe, cfg, read_any) || inherited;
    close_pipe(pipe, cfg);
  }

  if (inherited) {
    tui::debug(
        "shell: child exited with output pipes still held by a descendant; "
        "output written after this point is dropped");
  }
}

// One thread drains both pipes, stdout first, so causally ordered writes to the two
// streams reach the callbacks in that order - two readers raced and could invert them.
void stream_pipes(std::array<pipe_state, 2> &pipes,
                  HANDLE process,
                  shell_run_cfg const &cfg) {
  size_t closed_count{ 0 };

  while (closed_count < pipes.size()) {
    bool read_any{ false };
    for (auto &pipe : pipes) {
      if (pipe.closed || read_available(pipe, cfg, read_any)) { continue; }
      close_pipe(pipe, cfg);
      ++closed_count;
    }

    // Idle waits double as the exit check; a talkative child never pays for a sleep.
    // Pipe EOF means every writer let go, not that the child exited - watch for both.
    DWORD const wait_result{
      ::WaitForSingleObject(process, read_any ? 0 : kPipePollIntervalMs)
    };
    if (wait_result == WAIT_TIMEOUT) { continue; }
    if (wait_result != WAIT_OBJECT_0) {
      throw std::system_error(::GetLastError(),
                              std::system_category(),
                              "WaitForSingleObject failed");
    }

    drain_after_exit(pipes, cfg);
    return;
  }
}

shell_result child_result(HANDLE process) {
  DWORD exit_code{ 0 };
  if (!::GetExitCodeProcess(process, &exit_code)) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "GetExitCodeProcess failed");
  }

  return { .exit_code = static_cast<int>(exit_code), .signal = std::nullopt };
}

std::wstring quote_arg(std::wstring_view arg) {
  // Windows command-line quoting: wrap in quotes if contains spaces or special chars
  if (arg.find_first_of(L" \t\"") == std::wstring_view::npos) {
    return std::wstring{ arg };
  }

  std::wstring result{ L"\"" };
  for (size_t i{ 0 }; i < arg.size(); ++i) {
    size_t backslash_count{ 0 };
    while (i < arg.size() && arg[i] == L'\\') {
      ++backslash_count;
      ++i;
    }

    if (i == arg.size()) {
      // Backslashes at end of string: double them before closing quote
      result.append(backslash_count * 2, L'\\');
      break;
    } else if (arg[i] == L'"') {
      // Backslashes before quote: double them, then escape the quote
      result.append(backslash_count * 2 + 1, L'\\');
      result.push_back(L'"');
    } else {
      // Normal backslashes: keep as-is
      result.append(backslash_count, L'\\');
      result.push_back(arg[i]);
    }
  }
  result.push_back(L'"');
  return result;
}

std::wstring build_command_line_builtin(shell_choice shell,
                                        std::filesystem::path const &script_path) {
  std::wstring quoted{ L"\"" };
  quoted.append(script_path.wstring());
  quoted.push_back(L'"');

  if (shell == shell_choice::powershell) {
    // -NoProfile: Skip user profile for consistent, fast startup (intentionally breaks
    // profile-dependent scripts)
    return L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
           L"-File " +
           quoted;
  }

  // cmd shell requires nested quotes: ""C:\path\script.cmd""
  std::wstring command{ L"cmd.exe /D /V:ON /S /C \"" };
  command.append(quoted);
  command.push_back(L'"');
  return command;
}

std::wstring build_command_line_custom(custom_shell_file const &shell,
                                       std::filesystem::path const &script_path) {
  std::wstring command{};
  for (size_t i{ 0 }; i < shell.argv.size(); ++i) {
    if (i > 0) { command.push_back(L' '); }
    command.append(quote_arg(utf8_to_wstring(shell.argv[i])));
  }
  // Append script path as final argument
  command.push_back(L' ');
  command.append(quote_arg(script_path.wstring()));
  return command;
}

std::wstring build_command_line_custom(custom_shell_inline const &shell,
                                       std::string_view script_content) {
  std::wstring command{};
  for (size_t i{ 0 }; i < shell.argv.size(); ++i) {
    if (i > 0) { command.push_back(L' '); }
    command.append(quote_arg(utf8_to_wstring(shell.argv[i])));
  }
  // Append script content as final argument
  command.push_back(L' ');
  command.append(quote_arg(utf8_to_wstring(std::string{ script_content })));
  return command;
}

}  // namespace

void shell_init() {
  g_job_object = ::CreateJobObjectW(nullptr, nullptr);
  if (!g_job_object) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "Failed to create job object for child process management");
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  if (!::SetInformationJobObject(g_job_object,
                                 JobObjectExtendedLimitInformation,
                                 &info,
                                 sizeof(info))) {
    DWORD const err{ ::GetLastError() };
    ::CloseHandle(g_job_object);
    g_job_object = NULL;
    throw std::system_error(err, std::system_category(), "Failed to configure job object");
  }
}

shell_env_t shell_getenv() {
  shell_env_t env{};
  LPWCH block{ ::GetEnvironmentStringsW() };
  if (!block) { return env; }

  auto const free_block = [](LPWCH ptr) { ::FreeEnvironmentStringsW(ptr); };
  std::unique_ptr<wchar_t, decltype(free_block)> guard{ block, free_block };

  for (wchar_t const *entry{ block }; *entry != L'\0'; entry += wcslen(entry) + 1) {
    std::wstring_view const view{ entry };
    size_t const sep{ view.find(L'=') };
    if (sep == std::wstring_view::npos || sep == 0) { continue; }
    std::wstring const key{ view.substr(0, sep) };
    std::wstring const value{ view.substr(sep + 1) };
    env[wstring_to_utf8(key)] = wstring_to_utf8(value);
  }

  return env;
}

shell_result shell_run(std::string_view script, shell_run_cfg const &cfg) {
  std::filesystem::path const script_path{ create_temp_script(script, cfg) };
  scoped_path_cleanup cleanup{ script_path };

  // Environment block must be mutable for CreateProcessW (LPVOID), build then keep
  // non-const.
  std::vector<wchar_t> env_block{ build_environment_block(cfg.env) };

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = nullptr;
  sa.bInheritHandle = TRUE;

  HANDLE stdout_read{ nullptr };
  HANDLE stdout_write{ nullptr };
  if (!::CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
    throw std::system_error(::GetLastError(), std::system_category(), "CreatePipe failed");
  }

  handle_closer stdout_read_end{ stdout_read };
  handle_closer stdout_write_end{ stdout_write };
  if (!::SetHandleInformation(stdout_read_end.get(), HANDLE_FLAG_INHERIT, 0)) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "SetHandleInformation failed");
  }

  HANDLE stderr_read{ nullptr };
  HANDLE stderr_write{ nullptr };
  if (!::CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
    throw std::system_error(::GetLastError(), std::system_category(), "CreatePipe failed");
  }

  handle_closer stderr_read_end{ stderr_read };
  handle_closer stderr_write_end{ stderr_write };
  if (!::SetHandleInformation(stderr_read_end.get(), HANDLE_FLAG_INHERIT, 0)) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "SetHandleInformation failed");
  }

  HANDLE const null_in{ ::CreateFileW(L"NUL",
                                      GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr) };
  if (null_in == INVALID_HANDLE_VALUE) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "CreateFileW NUL failed");
  }
  handle_closer stdin_handle{ null_in };
  if (!::SetHandleInformation(stdin_handle.get(),
                              HANDLE_FLAG_INHERIT,
                              HANDLE_FLAG_INHERIT)) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "SetHandleInformation failed");
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdInput = stdin_handle.get();
  si.hStdOutput = stdout_write_end.get();
  si.hStdError = stderr_write_end.get();

  PROCESS_INFORMATION pi{};

  std::wstring const command_line{ std::visit(
      match{
          [&script_path](shell_choice const &shell_cfg) -> std::wstring {
            return build_command_line_builtin(shell_cfg, script_path);
          },
          [&script_path](custom_shell_file const &shell_cfg) -> std::wstring {
            return build_command_line_custom(shell_cfg, script_path);
          },
          [&script](custom_shell_inline const &shell_cfg) -> std::wstring {
            return build_command_line_custom(shell_cfg, script);
          },
      },
      cfg.shell) };
  std::vector<wchar_t> cmd_buffer{ command_line.begin(), command_line.end() };
  cmd_buffer.push_back(L'\0');

  std::wstring cwd_storage{};
  wchar_t *cwd_ptr{ nullptr };
  if (cfg.cwd) {
    cwd_storage = cfg.cwd->wstring();
    cwd_ptr = cwd_storage.data();
  }

  BOOL const created{ ::CreateProcessW(nullptr,
                                       cmd_buffer.data(),
                                       nullptr,
                                       nullptr,
                                       TRUE,
                                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                       env_block.empty() ? nullptr : env_block.data(),
                                       cwd_ptr,
                                       &si,
                                       &pi) };
  if (!created) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "CreateProcessW failed");
  }

  // Add process to job object to ensure child dies when envy terminates
  if (g_job_object && !::AssignProcessToJobObject(g_job_object, pi.hProcess)) {
    // Log warning but continue - non-fatal, worst case is orphaned child on Ctrl+C
    // (This should never fail in practice unless job was closed or process already exited)
  }

  handle_closer process{ pi.hProcess };
  handle_closer thread{ pi.hThread };

  // Parent no longer needs these handles
  stdin_handle.reset();
  stdout_write_end.reset();
  stderr_write_end.reset();

  std::array<pipe_state, 2> pipes{
    pipe_state{ stdout_read_end.get(), shell_stream::std_out, {}, 0, false },
    pipe_state{ stderr_read_end.get(), shell_stream::std_err, {}, 0, false },
  };

  try {
    stream_pipes(pipes, process.get(), cfg);
    if (::WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0) {
      throw std::system_error(::GetLastError(),
                              std::system_category(),
                              "WaitForSingleObject failed");
    }
  } catch (...) {  // a throwing callback leaves the pipes undrained; do not wait on that
    ::TerminateProcess(process.get(), 1);
    ::WaitForSingleObject(process.get(), INFINITE);
    throw;
  }

  return child_result(process.get());
}

}  // namespace envy

#endif  // _WIN32
