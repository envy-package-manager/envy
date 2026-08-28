#pragma once

#include "util.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Platform-specific unreachable hint. Use compiler intrinsics where available
// while remaining safe for MSVC which lacks __builtin_unreachable.
#if defined(_MSC_VER)
#define ENVY_UNREACHABLE() __assume(0)
#else
#define ENVY_UNREACHABLE() __builtin_unreachable()
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <io.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace envy { enum class platform_id { POSIX, WINDOWS }; }  // namespace envy

namespace envy::platform {

class file_lock : uncopyable {
 public:
  explicit file_lock(std::filesystem::path const &path);
  ~file_lock();
  file_lock(file_lock &&) noexcept;
  file_lock &operator=(file_lock &&) noexcept;

  explicit operator bool() const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

void atomic_rename(std::filesystem::path const &from, std::filesystem::path const &to);
void touch_file(std::filesystem::path const &path);
std::filesystem::path create_unique_temp_file(std::string_view prefix);

// Atomically claim a new private directory under the system temp dir. Owner-only on POSIX.
// A predictable name would let another user in a shared sticky /tmp pre-create the path
// and see (or influence) whatever is staged into it.
std::filesystem::path create_unique_temp_dir(std::string_view prefix);
void flush_directory(std::filesystem::path const &dir);
bool file_exists(std::filesystem::path const &path);

// One immediate child of a directory, as reported by the platform's native
// enumeration (POSIX d_type, Windows file attributes).
struct dir_entry {
  std::string name;
  bool is_dir{ false };
  bool is_symlink{ false };  // reparse point on Windows; never traversed
};

// Aggregate of one directory tree. Bytes are apparent file sizes. A symlink
// *encountered during the walk* is counted as neither file nor directory and
// its target is not descended into, so no tree is measured twice however it is
// linked. A root named by the caller is opened as named — like every other path
// here, and unlike its children, since refusing to open it would break a cache
// root that lives behind a symlink or a junction.
struct dir_size {
  std::uint64_t bytes{ 0 };
  std::uint64_t files{ 0 };
  std::uint64_t dirs{ 0 };
};

// Immediate children of `dir`, in filesystem order; empty if unreadable.
std::vector<dir_entry> dir_list(std::filesystem::path const &dir);

// Measure every root concurrently; results are index-aligned with `roots`.
// Missing or unreadable roots yield zeroes. `threads == 0` uses hardware
// concurrency. Traversal is native (openat/fdopendir/fstatat, FindFirstFileExW
// with FIND_FIRST_EX_LARGE_FETCH) rather than std::filesystem, which
// re-resolves a full path per entry and cannot be driven from many threads
// without re-walking shared prefixes.
std::vector<dir_size> dir_sizes(std::vector<std::filesystem::path> const &roots,
                                unsigned threads = 0);

// Native path string: UTF-16 on Windows, bytes elsewhere. Traversal stays in
// the OS encoding; conversion happens only at the public boundary.
using dir_scan_string = std::filesystem::path::string_type;
using dir_scan_push = std::function<void(dir_scan_string)>;

// Per-platform traversal hooks driving dir_sizes(); not called directly.
// dir_scan_root() adapts a path for native traversal; dir_scan_one() folds one
// directory's files into `acc` and hands each child directory to `push`.
dir_scan_string dir_scan_root(std::filesystem::path const &root);
void dir_scan_one(dir_scan_string const &dir, dir_size &acc, dir_scan_push const &push);

std::optional<std::filesystem::path> get_default_cache_root();
char const *get_default_cache_root_env_vars();

std::filesystem::path get_exe_path();

void env_var_set(char const *name, char const *value);
void env_var_unset(char const *name);

// Remove directory recursively with retry logic for Windows file locking issues.
// On Windows, antivirus/indexer may hold file handles briefly after creation.
// Returns default error_code on success, or the final OS error on failure.
std::error_code remove_all_with_retry(std::filesystem::path const &target);

// Wait until all regular files in dir are readable (no sharing violations).
// On Windows, probes each file with CreateFileW and, for ".exe" files,
// with CreateProcessW(CREATE_SUSPENDED); retries with backoff on
// ERROR_SHARING_VIOLATION (Defender/SmartScreen/Indexer).  POSIX: no-op.
void await_files_accessible(std::filesystem::path const &dir);

// Mark a directory as not interesting to the Windows Search Indexer
// (FILE_ATTRIBUTE_NOT_CONTENT_INDEXED).  Children inherit the attribute.
// POSIX: no-op.
void mark_not_indexed(std::filesystem::path const &dir);

[[noreturn]] void terminate_process();

bool is_tty();

platform_id native();
std::string_view os_name();
std::string_view arch_name();

// Platform-correct executable suffix: ".exe" on Windows, "" otherwise.
std::string_view exe_suffix();
// Platform-correct executable filename: base + exe_suffix().
std::filesystem::path exe_name(std::string_view base);

// Read the current process environment as a list of "KEY=VALUE" strings.
std::vector<std::string> get_environment();

int get_process_id();

// Execute a child process with explicit environment.
int exec_process(std::filesystem::path const &binary,
                 char **argv,
                 std::vector<std::string> env);

}  // namespace envy::platform
