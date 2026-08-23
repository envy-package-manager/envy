#include "platform.h"

#include "tui.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace envy::platform {

struct file_lock::impl {
  HANDLE handle;
  std::filesystem::path lock_path;
};

file_lock::~file_lock() {
  if (impl_) {
    OVERLAPPED ovlp{};
    ::UnlockFileEx(impl_->handle, 0, MAXDWORD, MAXDWORD, &ovlp);
    ::CloseHandle(impl_->handle);

    // Delete lock file after closing handle
    std::error_code ec;
    std::filesystem::remove(impl_->lock_path, ec);
    // Ignore errors - file may be held by another process, which is expected
  }
}

file_lock::file_lock(file_lock &&) noexcept = default;
file_lock &file_lock::operator=(file_lock &&) noexcept = default;

file_lock::operator bool() const { return impl_ != nullptr; }

std::optional<std::filesystem::path> get_default_cache_root() {
  if (char const *local_app_data{ std::getenv("LOCALAPPDATA") }) {
    return std::filesystem::path{ local_app_data } / "envy";
  }

  if (char const *user_profile{ std::getenv("USERPROFILE") }) {
    return std::filesystem::path{ user_profile } / "AppData" / "Local" / "envy";
  }

  return std::nullopt;
}

char const *get_default_cache_root_env_vars() { return "LOCALAPPDATA or USERPROFILE"; }

std::filesystem::path get_exe_path() {
  std::vector<wchar_t> buf(32768);
  if (::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size())) == 0) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "GetModuleFileNameW failed");
  }
  return std::filesystem::path{ buf.data() };
}

void env_var_set(char const *name, char const *value) {
  if (name == nullptr || value == nullptr) {
    throw std::invalid_argument("env_var_set: null name or value");
  }

  if (::_putenv_s(name, value) != 0) {
    throw std::runtime_error(std::string("env_var_set: failed to set ") + name);
  }
}

void env_var_unset(char const *name) {
  if (name == nullptr) { throw std::invalid_argument("env_var_unset: null name"); }
  if (::_putenv_s(name, "") != 0) {
    throw std::runtime_error(std::string("env_var_unset: failed to unset ") + name);
  }
}

file_lock::file_lock(std::filesystem::path const &path, contended_cb_t on_contended) {
  HANDLE const h{ ::CreateFileW(path.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr) };
  if (h == INVALID_HANDLE_VALUE) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "Failed to open lock file: " + path.string());
  }

  // Probe first: a refusal means someone else holds the entry, so the wait below is
  // open-ended — the only kind worth announcing.
  OVERLAPPED probe{};
  if (!::LockFileEx(h,
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0,
                    MAXDWORD,
                    MAXDWORD,
                    &probe)) {
    // A held lock is contention; a bad handle is not, and the call below reports it.
    if (::GetLastError() == ERROR_LOCK_VIOLATION && on_contended) { on_contended(); }

    OVERLAPPED ovlp{};
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ovlp)) {
      DWORD const err{ ::GetLastError() };
      ::CloseHandle(h);
      throw std::system_error(err,
                              std::system_category(),
                              "Failed to acquire file lock: " + path.string());
    }
  }

  impl_ = std::make_unique<impl>();
  impl_->handle = h;
  impl_->lock_path = path;
}

void atomic_rename(std::filesystem::path const &from, std::filesystem::path const &to) {
  if (!::MoveFileExW(from.c_str(),
                     to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "Failed to rename " + from.string() + " to " + to.string());
  }
}

std::filesystem::path create_unique_temp_file(std::string_view prefix) {
  static std::atomic<uint64_t> counter{ 0 };
  auto const seq{ counter.fetch_add(1, std::memory_order_seq_cst) };
  DWORD const pid{ ::GetCurrentProcessId() };
  auto name{ std::string{ prefix } + "-" + std::to_string(pid) + "-" +
             std::to_string(seq) };
  auto p{ std::filesystem::temp_directory_path() / name };

  HANDLE const h{ ::CreateFileW(p.c_str(),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr) };
  if (h == INVALID_HANDLE_VALUE) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "Failed to create temp file: " + p.string());
  }
  ::CloseHandle(h);
  return p;
}

std::filesystem::path create_unique_temp_dir(std::string_view prefix) {
  static std::atomic<uint64_t> counter{ 0 };
  DWORD const pid{ ::GetCurrentProcessId() };

  // CreateDirectoryW fails with ERROR_ALREADY_EXISTS rather than reusing an existing
  // directory, so whoever wins the create owns the name. Retry on collision: the counter
  // is per-process, so a concurrent process could pick the same seq.
  for (int attempt{ 0 }; attempt < 16; ++attempt) {
    auto const seq{ counter.fetch_add(1, std::memory_order_seq_cst) };
    auto name{ std::string{ prefix } + "-" + std::to_string(pid) + "-" +
               std::to_string(seq) };
    auto p{ std::filesystem::temp_directory_path() / name };
    if (::CreateDirectoryW(p.c_str(), nullptr)) { return p; }
    if (::GetLastError() != ERROR_ALREADY_EXISTS) {
      throw std::system_error(::GetLastError(),
                              std::system_category(),
                              "Failed to create temp directory: " + p.string());
    }
  }
  throw std::runtime_error("Failed to create a unique temp directory under " +
                           std::filesystem::temp_directory_path().string());
}

void touch_file(std::filesystem::path const &path) {
  HANDLE const h{ ::CreateFileW(path.c_str(),
                                GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr) };

  if (h == INVALID_HANDLE_VALUE) {
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "Failed to touch file: " + path.string());
  }

  // Flush file buffers to ensure file metadata is committed to disk
  // before other processes try to read it. Critical for multi-process
  // cache synchronization on Windows.
  if (!::FlushFileBuffers(h)) {
    ::CloseHandle(h);
    throw std::system_error(::GetLastError(),
                            std::system_category(),
                            "Failed to flush file buffers: " + path.string());
  }

  ::CloseHandle(h);

  // Flush parent directory to ensure file is immediately visible to other processes
  std::filesystem::path const parent{ path.parent_path() };
  if (!parent.empty()) { flush_directory(parent); }
}

void flush_directory(std::filesystem::path const &dir) {
  HANDLE const dir_h{ ::CreateFileW(dir.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr) };

  if (dir_h != INVALID_HANDLE_VALUE) {
    ::FlushFileBuffers(dir_h);
    ::CloseHandle(dir_h);
  }
}

bool file_exists(std::filesystem::path const &path) {
  // On Windows, std::filesystem::exists() uses cached directory listings that aren't
  // invalidated by FlushFileBuffers. To bypass the cache, we directly attempt to open
  // the file - this forces Windows to check the actual filesystem.
  HANDLE const h{ ::CreateFileW(path.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr) };

  if (h != INVALID_HANDLE_VALUE) {
    ::CloseHandle(h);
    return true;
  }

  return false;
}

namespace {

constexpr wchar_t kLongPrefix[]{ LR"(\\?\)" };
constexpr wchar_t kLongPrefixUnc[]{ LR"(\\?\UNC\)" };

dir_scan_string dir_join(dir_scan_string const &dir, wchar_t const *name) {
  dir_scan_string out{ dir };
  if (!out.empty() && out.back() != L'\\') { out.push_back(L'\\'); }
  out.append(name);
  return out;
}

bool is_dot(wchar_t const *name) {
  return name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]));
}

// FindExInfoBasic drops the 8.3 short-name lookup and LARGE_FETCH batches
// entries per syscall; sizes ride along with the enumeration, so measuring a
// tree costs no per-file opens at all.
HANDLE find_first(dir_scan_string const &pattern, WIN32_FIND_DATAW &fd) {
  return ::FindFirstFileExW(pattern.c_str(),
                            FindExInfoBasic,
                            &fd,
                            FindExSearchNameMatch,
                            nullptr,
                            FIND_FIRST_EX_LARGE_FETCH);
}

}  // namespace

dir_scan_string dir_scan_root(std::filesystem::path const &root) {
  // Cache trees nest deeply, so opt out of MAX_PATH once at the root and let
  // every child inherit the prefix. It demands a fully-qualified, backslash-only
  // path; the prefixed form is internal and never shown to the user.
  auto out{ root.lexically_normal().native() };
  for (auto &c : out) {
    if (c == L'/') { c = L'\\'; }
  }
  if (out.rfind(kLongPrefix, 0) == 0) { return out; }
  if (out.rfind(LR"(\\)", 0) == 0) { return kLongPrefixUnc + out.substr(2); }
  if (out.size() >= 2 && out[1] == L':') { return kLongPrefix + out; }
  return out;  // relative: leave alone, MAX_PATH applies
}

void dir_scan_one(dir_scan_string const &dir, dir_size &acc, dir_scan_push const &push) {
  WIN32_FIND_DATAW fd;
  HANDLE const h{ find_first(dir_join(dir, L"*"), fd) };
  if (h == INVALID_HANDLE_VALUE) { return; }

  do {
    if (is_dot(fd.cFileName)) { continue; }
    // A reparse point is billed to whoever owns its target.
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { continue; }

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      ++acc.dirs;
      push(dir_join(dir, fd.cFileName));
    } else {
      acc.bytes += (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
      ++acc.files;
    }
  } while (::FindNextFileW(h, &fd));

  ::FindClose(h);
}

std::vector<dir_entry> dir_list(std::filesystem::path const &dir) {
  std::vector<dir_entry> out;

  WIN32_FIND_DATAW fd;
  HANDLE const h{ find_first(dir_join(dir_scan_root(dir), L"*"), fd) };
  if (h == INVALID_HANDLE_VALUE) { return out; }

  do {
    if (is_dot(fd.cFileName)) { continue; }
    out.push_back({ std::filesystem::path{ fd.cFileName }.string(),
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 });
  } while (::FindNextFileW(h, &fd));

  ::FindClose(h);
  return out;
}

[[noreturn]] void terminate_process() { ::TerminateProcess(::GetCurrentProcess(), 1); }

bool is_tty() { return ::_isatty(::_fileno(stderr)) != 0; }

platform_id native() { return platform_id::WINDOWS; }

std::string_view os_name() { return "windows"; }

std::string_view arch_name() {
#if defined(_M_ARM64)
  return "arm64";
#elif defined(_M_X64)
  return "x86_64";
#else
#error "unsupported architecture"
#endif
}

std::error_code remove_all_with_retry(std::filesystem::path const &target) {
  // Windows antivirus (Defender) and Search indexer often hold file handles
  // briefly after files are created/downloaded. Retry with exponential backoff.
  constexpr int kMaxRetries{ 8 };
  constexpr int kInitialDelayMs{ 50 };
  constexpr int kMaxDelayMs{ 1000 };

  std::error_code ec;
  for (int attempt{ 0 }; attempt < kMaxRetries; ++attempt) {
    std::filesystem::remove_all(target, ec);
    if (!ec) { return ec; }

    // ERROR_SHARING_VIOLATION (32) and ERROR_LOCK_VIOLATION (33) are the
    // typical errors when another process has the file/directory open.
    // Also handle ERROR_ACCESS_DENIED (5) which can occur during AV scans.
    DWORD const win_err{ static_cast<DWORD>(ec.value()) };
    bool const retryable{ win_err == ERROR_SHARING_VIOLATION ||
                          win_err == ERROR_LOCK_VIOLATION ||
                          win_err == ERROR_ACCESS_DENIED };
    if (!retryable) { break; }

    if (attempt + 1 < kMaxRetries) {
      // Exponential backoff: 50, 100, 200, 400, 800, 1000, 1000ms (~3.5s total)
      int delay_ms{ kInitialDelayMs << attempt };
      if (delay_ms > kMaxDelayMs) { delay_ms = kMaxDelayMs; }
      ::Sleep(static_cast<DWORD>(delay_ms));
    }
  }

  // remove_all is progressive — each retry removes more entries.  The last
  // call may error on the directory itself (Defender holding a dir handle)
  // yet the target is effectively gone by the time we check.  Also handles
  // the race where the handle is released between remove_all returning and
  // our probe.
  if (ec) {
    std::error_code probe_ec;
    bool const exists{ std::filesystem::exists(target, probe_ec) };
    if (!probe_ec && !exists) { return {}; }

    // Target still exists but might be an empty dir whose children were all
    // removed.  One non-recursive remove is cheap and handles that case.
    if (!probe_ec && exists) {
      std::filesystem::remove(target, probe_ec);
      if (!probe_ec) { return {}; }
    }
  }

  return ec;
}

namespace {

// Probe whether a file can be opened for reading.  Detects Defender's
// on-access scan lock (ERROR_SHARING_VIOLATION at the file-handle level).
bool probe_file_readable(std::filesystem::path const &path) {
  HANDLE const h{ ::CreateFileW(path.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr) };
  if (h != INVALID_HANDLE_VALUE) {
    ::CloseHandle(h);
    return true;
  }
  return false;
}

// Probe whether an executable can be launched.  Defender's on-execution scan
// (WdFilter.sys) hooks NtCreateSection(SEC_IMAGE) inside CreateProcess —
// a level that CreateFileW cannot reach regardless of access flags.  The only
// reliable probe is to actually call CreateProcess with CREATE_SUSPENDED,
// which exercises the full image-mapping path without running any user code.
bool probe_exe_launchable(std::filesystem::path const &path) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  if (!::CreateProcessW(path.c_str(),
                        nullptr,
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_SUSPENDED,
                        nullptr,
                        nullptr,
                        &si,
                        &pi)) {
    return false;
  }

  ::TerminateProcess(pi.hProcess, 0);
  ::WaitForSingleObject(pi.hProcess, 1000);
  ::CloseHandle(pi.hProcess);
  ::CloseHandle(pi.hThread);
  return true;
}

}  // namespace

void await_files_accessible(std::filesystem::path const &dir) {
  std::error_code ec;
  std::filesystem::directory_iterator it{ dir, ec };
  if (ec) { return; }

  constexpr int kMaxRetries{ 10 };
  constexpr int kInitialDelayMs{ 100 };
  constexpr int kMaxDelayMs{ 1500 };

  for (auto const &entry : it) {
    if (!entry.is_regular_file(ec) || ec) { continue; }

    bool const is_exe{ ::_wcsicmp(entry.path().extension().c_str(), L".exe") == 0 };

    for (int attempt{ 0 }; attempt < kMaxRetries; ++attempt) {
      bool const ok{ is_exe ? probe_exe_launchable(entry.path())
                            : probe_file_readable(entry.path()) };
      if (ok) { break; }

      DWORD const err{ ::GetLastError() };
      if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) { break; }

      if (attempt == 0) {
        tui::debug("await_files_accessible: %s locked, waiting",
                   entry.path().filename().string().c_str());
      }

      if (attempt + 1 == kMaxRetries) {
        tui::warn("await_files_accessible: %s still locked after retries, proceeding",
                  entry.path().filename().string().c_str());
        break;
      }

      int delay_ms{ kInitialDelayMs << attempt };
      if (delay_ms > kMaxDelayMs) { delay_ms = kMaxDelayMs; }
      ::Sleep(static_cast<DWORD>(delay_ms));
    }
  }
}

void mark_not_indexed(std::filesystem::path const &dir) {
  DWORD const attrs{ ::GetFileAttributesW(dir.c_str()) };
  if (attrs == INVALID_FILE_ATTRIBUTES) { return; }
  if (attrs & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) { return; }
  ::SetFileAttributesW(dir.c_str(), attrs | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
}

std::filesystem::path expand_path(std::string_view p) {
  if (p.empty()) { return {}; }

  std::string result;
  size_t i{ 0 };

  // Leading ~ → USERPROFILE
  if (p[0] == '~' && (p.size() == 1 || p[1] == '/' || p[1] == '\\')) {
    char const *home{ std::getenv("USERPROFILE") };
    if (!home) { throw std::runtime_error("USERPROFILE not set for tilde expansion"); }
    result = home;
    i = 1;
  }

  while (i < p.size()) {
    if (p[i] == '$') {
      ++i;
      bool const braced{ i < p.size() && p[i] == '{' };
      if (braced) { ++i; }

      size_t const start{ i };
      while (i < p.size() &&
             (std::isalnum(static_cast<unsigned char>(p[i])) || p[i] == '_')) {
        ++i;
      }

      std::string var_name{ p.substr(start, i - start) };
      if (braced && i < p.size() && p[i] == '}') { ++i; }

      if (char const *val{ std::getenv(var_name.c_str()) }) {
        result += val;
      } else if (var_name == "HOME") {
        // $HOME is common in cross-platform scripts; map to USERPROFILE on Windows
        if (char const *profile{ std::getenv("USERPROFILE") }) { result += profile; }
      }
      // other undefined vars → empty string on Windows
    } else {
      result += p[i++];
    }
  }

  return result;
}

int get_process_id() { return static_cast<int>(GetCurrentProcessId()); }

std::vector<std::string> get_environment() {
  std::vector<std::string> result;
  if (char *block{ GetEnvironmentStringsA() }; block) {
    for (char const *p = block; *p; p += std::strlen(p) + 1) { result.emplace_back(p); }
    FreeEnvironmentStringsA(block);
  }
  return result;
}

namespace {

// Build a flat command line string from argv with proper Windows quoting.
std::string build_cmdline(char **argv) {
  std::string cmdline;
  for (int i = 0; argv[i]; ++i) {
    if (i > 0) { cmdline += ' '; }
    std::string_view arg{ argv[i] };
    bool const needs_quote{ arg.empty() ||
                            arg.find_first_of(" \t\"") != std::string_view::npos };
    if (!needs_quote) {
      cmdline += arg;
      continue;
    }

    cmdline += '"';
    for (auto it = arg.begin();;) {
      int n_bs{ 0 };
      while (it != arg.end() && *it == '\\') {
        ++it;
        ++n_bs;
      }

      if (it == arg.end()) {
        cmdline.append(static_cast<size_t>(n_bs * 2), '\\');
        break;
      }

      if (*it == '"') {
        cmdline.append(static_cast<size_t>(n_bs * 2 + 1), '\\');
      } else {
        cmdline.append(static_cast<size_t>(n_bs), '\\');
      }
      cmdline += *it++;
    }
    cmdline += '"';
  }
  return cmdline;
}

// Build a double-null-terminated environment block for CreateProcessA.
std::string build_env_block(std::vector<std::string> const &env) {
  std::string block;
  for (auto const &e : env) {
    block.append(e);
    block.push_back('\0');
  }
  block.push_back('\0');  // double-null terminator
  return block;
}

}  // namespace

int exec_process(std::filesystem::path const &binary,
                 char **argv,
                 std::vector<std::string> env) {
  auto cmdline{ build_cmdline(argv) };
  auto env_block{ build_env_block(env) };

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  if (!CreateProcessA(binary.string().c_str(),
                      cmdline.data(),
                      nullptr,
                      nullptr,
                      TRUE,
                      0,
                      env_block.data(),
                      nullptr,
                      &si,
                      &pi)) {
    throw std::runtime_error("exec_process: CreateProcess failed: " +
                             std::to_string(GetLastError()));
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code{};
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exit_code);
}

std::string_view exe_suffix() { return ".exe"; }

std::filesystem::path exe_name(std::string_view base) {
  return std::filesystem::path{ std::string{ base } + ".exe" };
}

}  // namespace envy::platform
