#include "platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wordexp.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" char **environ;

namespace envy::platform {

struct file_lock::impl {
  int fd;
  std::mutex *path_mutex;  // owned by the s_lock_mutexes map
  std::filesystem::path lock_path;

  // POSIX file locks are per-process, not per-thread: multiple threads in the same process
  // can bypass the file lock and acquire it simultaneously. To ensure thread-level mutual
  // exclusion for file locks within a process, we use an in-process mutex per path.
  static std::mutex s_lock_map_mutex;
  static std::unordered_map<std::string, std::unique_ptr<std::mutex> > s_lock_mutexes;
};

std::mutex file_lock::impl::s_lock_map_mutex;
std::unordered_map<std::string, std::unique_ptr<std::mutex> >
    file_lock::impl::s_lock_mutexes;

file_lock::~file_lock() {
  if (impl_) {
    // Unlink while the fcntl lock is still held, then close. Unlinking after close
    // races: a waiter can acquire the released (now-stale) inode while a third
    // party re-creates the path and locks the fresh inode - two "holders". The
    // constructor's inode check below rejects the stale acquisition.
    std::error_code ec;  // Ignore errors - lock file may be deleted or inaccessible.
    std::filesystem::remove(impl_->lock_path, ec);
    ::close(impl_->fd);
    if (impl_->path_mutex) { impl_->path_mutex->unlock(); }
  }
}

file_lock::file_lock(file_lock &&) noexcept = default;
file_lock &file_lock::operator=(file_lock &&) noexcept = default;

file_lock::operator bool() const { return impl_ != nullptr; }

std::optional<std::filesystem::path> get_default_cache_root() {
#ifdef __APPLE__
  if (char const *home{ std::getenv("HOME") }) {
    return std::filesystem::path{ home } / "Library" / "Caches" / "envy";
  }
#else
  if (char const *xdg_cache{ std::getenv("XDG_CACHE_HOME") }) {
    return std::filesystem::path{ xdg_cache } / "envy";
  }

  if (char const *home{ std::getenv("HOME") }) {
    return std::filesystem::path{ home } / ".cache" / "envy";
  }
#endif

  return std::nullopt;
}

char const *get_default_cache_root_env_vars() {
#ifdef __APPLE__
  return "HOME";
#else
  return "XDG_CACHE_HOME or HOME";
#endif
}

std::filesystem::path get_exe_path() {
#ifdef __APPLE__
  uint32_t size{ 0 };
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0) {
    throw std::runtime_error("_NSGetExecutablePath failed");
  }
  return std::filesystem::canonical(buf.data());
#else
  std::vector<char> buf(4096);
  ssize_t const len{ ::readlink("/proc/self/exe", buf.data(), buf.size() - 1) };
  if (len == -1) {
    throw std::system_error(errno,
                            std::system_category(),
                            "readlink /proc/self/exe failed");
  }
  buf[static_cast<size_t>(len)] = '\0';
  return std::filesystem::path{ buf.data() };
#endif
}

void env_var_set(char const *name, char const *value) {
  if (name == nullptr || value == nullptr) {
    throw std::invalid_argument("env_var_set: null name or value");
  }

  if (::setenv(name, value, 1) != 0) {
    throw std::runtime_error(std::string("env_var_set: failed to set ") + name);
  }
}

void env_var_unset(char const *name) {
  if (name == nullptr) { throw std::invalid_argument("env_var_unset: null name"); }
  ::unsetenv(name);
}

file_lock::file_lock(std::filesystem::path const &path, contended_cb_t on_contended) {
  // Canonicalize path to ensure different representations of same path use same mutex
  std::string const canonical_key{
    std::filesystem::absolute(path).lexically_normal().string()
  };

  // Acquire in-process mutex for this lock path, ensure one thread per cache entry
  std::unique_lock<std::mutex> path_lock{ [&]() {
    std::lock_guard<std::mutex> lock(impl::s_lock_map_mutex);
    auto &mutex_ptr{ impl::s_lock_mutexes[canonical_key] };
    if (!mutex_ptr) { mutex_ptr = std::make_unique<std::mutex>(); }
    return std::unique_lock<std::mutex>{ *mutex_ptr };
  }() };

  int fd{ -1 };
  for (;;) {
    fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
      throw std::system_error(errno,
                              std::system_category(),
                              "Failed to open lock file: " + path.string());
    }

    // Assign fields rather than use a designated initializer: struct flock's field
    // order differs across platforms (Linux vs BSD/macOS), so no single designator
    // order is portable (-Wreorder-init-list). Zero-init covers l_start/l_len/l_pid.
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;

    // Probe before committing to the blocking call: a refusal means another process
    // holds the entry, so the wait is open-ended — the only kind worth announcing. The
    // in-process mutex above is deliberately not announced: it serializes threads of
    // this run, which the engine already keys apart, and "another envy" would be a lie.
    if (::fcntl(fd, F_SETLK, &fl) == -1) {
      if ((errno == EAGAIN || errno == EACCES) && on_contended) { on_contended(); }

      if (::fcntl(fd, F_SETLKW, &fl) == -1) {
        int const err{ errno };
        ::close(fd);
        throw std::system_error(err,
                                std::system_category(),
                                "Failed to acquire exclusive lock: " + path.string());
      }
    }

    // A previous holder may have unlinked (or unlinked+recreated) the path while we
    // waited on its inode; a lock on an unlinked inode excludes nobody. Only accept
    // the lock if our fd still refers to the file currently at the path.
    struct stat fd_st{}, path_st{};
    if (::fstat(fd, &fd_st) == 0 && ::stat(path.c_str(), &path_st) == 0 &&
        fd_st.st_dev == path_st.st_dev && fd_st.st_ino == path_st.st_ino) {
      break;
    }
    ::close(fd);
  }

  impl_ = std::make_unique<impl>();
  impl_->fd = fd;
  impl_->path_mutex = path_lock.release();  // Transfer ownership, mutex stays locked
  impl_->lock_path = path;
}

void atomic_rename(std::filesystem::path const &from, std::filesystem::path const &to) {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    throw std::system_error(errno,
                            std::system_category(),
                            "Failed to rename " + from.string() + " to " + to.string());
  }
}

std::filesystem::path create_unique_temp_file(std::string_view prefix) {
  auto pattern{ (std::filesystem::temp_directory_path() / std::string{ prefix }).string() +
                "-XXXXXX" };
  std::vector<char> buf(pattern.begin(), pattern.end());
  buf.push_back('\0');
  int const fd{ ::mkstemp(buf.data()) };
  if (fd == -1) {
    throw std::system_error(errno, std::generic_category(), "mkstemp failed");
  }
  ::close(fd);
  return std::filesystem::path{ buf.data() };
}

std::filesystem::path create_unique_temp_dir(std::string_view prefix) {
  auto pattern{ (std::filesystem::temp_directory_path() / std::string{ prefix }).string() +
                "-XXXXXX" };
  std::vector<char> buf(pattern.begin(), pattern.end());
  buf.push_back('\0');
  // mkdtemp creates with mode 0700 and fails if the name exists, so the claim is atomic.
  if (::mkdtemp(buf.data()) == nullptr) {
    throw std::system_error(errno, std::generic_category(), "mkdtemp failed");
  }
  return std::filesystem::path{ buf.data() };
}

void touch_file(std::filesystem::path const &path) {
  int const fd{ ::open(path.c_str(), O_CREAT | O_WRONLY, 0644) };
  if (fd == -1) {
    throw std::system_error(errno,
                            std::system_category(),
                            "Failed to touch file: " + path.string());
  }
  ::close(fd);
}

void flush_directory(std::filesystem::path const &) {
  // No-op on Unix - directory metadata is not cached in the same way as Windows
}

bool file_exists(std::filesystem::path const &path) {
  // On Unix, directory caching isn't an issue - std::filesystem::exists is sufficient
  return std::filesystem::exists(path);
}

namespace {

dir_scan_string dir_join(dir_scan_string const &dir, char const *name) {
  dir_scan_string out;
  out.reserve(dir.size() + 1 + std::strlen(name));
  out.append(dir);
  if (!out.empty() && out.back() != '/') { out.push_back('/'); }
  out.append(name);
  return out;
}

bool is_dot(char const *name) {
  return name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
}

// An open directory stream and the descriptor it owns. opendir() is open() +
// fdopendir() with no window where the descriptor is orphaned, and dirfd()
// hands the descriptor back for fstatat(), so nothing outside needs to hold it.
class scoped_dir : unmovable {
 public:
  explicit scoped_dir(char const *path) : d_{ ::opendir(path) } {}
  ~scoped_dir() {
    if (d_) { ::closedir(d_); }
  }

  explicit operator bool() const { return d_ != nullptr; }

  int fd() const { return ::dirfd(d_); }
  dirent const *next() { return ::readdir(d_); }

 private:
  DIR *d_;
};

}  // namespace

dir_scan_string dir_scan_root(std::filesystem::path const &root) { return root.native(); }

void dir_scan_one(dir_scan_string const &dir, dir_size &acc, dir_scan_push const &push) {
  // Resolve the directory once, then stat children against its descriptor:
  // fstatat walks a single name instead of the whole path per file, which is
  // where a path-at-a-time walker burns most of its kernel time on deep trees.
  scoped_dir d{ dir.c_str() };
  if (!d) { return; }
  int const fd{ d.fd() };

  while (dirent const *const e{ d.next() }) {
    char const *const name{ e->d_name };
    if (is_dot(name)) { continue; }

    switch (e->d_type) {
      case DT_DIR:
        ++acc.dirs;
        push(dir_join(dir, name));
        continue;
      case DT_LNK: continue;  // a symlinked tree is billed to whoever owns it
      case DT_REG:
      case DT_UNKNOWN: break;  // need st_size, or the fs withheld the type
      default: continue;       // sockets, fifos, devices hold no payload
    }

    struct stat st;
    if (::fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) { continue; }
    if (S_ISDIR(st.st_mode)) {
      ++acc.dirs;
      push(dir_join(dir, name));
    } else if (S_ISREG(st.st_mode)) {
      acc.bytes += static_cast<std::uint64_t>(st.st_size);
      ++acc.files;
    }
  }
}

std::vector<dir_entry> dir_list(std::filesystem::path const &dir) {
  std::vector<dir_entry> out;

  scoped_dir d{ dir.c_str() };
  if (!d) { return out; }
  int const fd{ d.fd() };

  while (dirent const *const e{ d.next() }) {
    if (is_dot(e->d_name)) { continue; }

    dir_entry entry{ e->d_name, false, false };
    switch (e->d_type) {
      case DT_DIR: entry.is_dir = true; break;
      case DT_LNK: entry.is_symlink = true; break;
      case DT_REG: break;
      default: {
        struct stat st;
        if (!::fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
          entry.is_dir = S_ISDIR(st.st_mode);
          entry.is_symlink = S_ISLNK(st.st_mode);
        }
      } break;
    }
    out.push_back(std::move(entry));
  }

  return out;
}

void await_files_accessible(std::filesystem::path const &) {}

void mark_not_indexed(std::filesystem::path const &) {}

// SIGKILL, not abort(): both die without running destructors, but SIGABRT is a
// Mach exception, so macOS writes a crash report and pops a "quit unexpectedly"
// dialog for every deliberate kill the cache tests perform. A kernel kill is
// not reported, and abrupt kernel termination is what those tests simulate.
[[noreturn]] void terminate_process() {
  ::kill(::getpid(), SIGKILL);
  _exit(137);  // Unreachable: SIGKILL cannot be caught, blocked, or ignored.
}

bool is_tty() { return ::isatty(::fileno(stderr)) != 0; }

platform_id native() { return platform_id::POSIX; }

std::string_view os_name() {
#if defined(__APPLE__) && defined(__MACH__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#else
#error "unsupported POSIX OS"
#endif
}

std::string_view arch_name() {
#if defined(__aarch64__) || defined(__arm64__)
  return "arm64";
#elif defined(__x86_64__)
  return "x86_64";
#else
#error "unsupported architecture"
#endif
}

std::error_code remove_all_with_retry(std::filesystem::path const &target) {
  // On POSIX, file deletion works even with open handles (files get unlinked
  // but data persists until all handles close). No retry needed.
  std::error_code ec;
  std::filesystem::remove_all(target, ec);
  return ec;
}

std::filesystem::path expand_path(std::string_view p) {
  if (p.empty()) { return {}; }

  wordexp_t we{};
  std::string const path_str{ p };
  int const flags{ WRDE_NOCMD | WRDE_UNDEF };  // no $(cmd), fail on undefined $VAR

  int const rc{ wordexp(path_str.c_str(), &we, flags) };

  if (rc == 0) {
    if (we.we_wordc == 0) {
      wordfree(&we);
      throw std::runtime_error("path expansion produced no results: " + path_str);
    }
    std::filesystem::path result{ we.we_wordv[0] };
    wordfree(&we);
    return result;
  }

  // POSIX: wordfree() must only be called after successful wordexp()
  if (rc == WRDE_BADVAL) {
    throw std::runtime_error("undefined variable in path: " + path_str);
  }
  throw std::runtime_error("path expansion failed: " + path_str);
}

int get_process_id() { return static_cast<int>(getpid()); }

std::vector<std::string> get_environment() {
  std::vector<std::string> result;
  for (char **ep = environ; *ep; ++ep) { result.emplace_back(*ep); }
  return result;
}

int exec_process(std::filesystem::path const &binary,
                 char **argv,
                 std::vector<std::string> env) {
  std::vector<char *> envp;
  envp.reserve(env.size() + 1);
  for (auto &e : env) { envp.push_back(e.data()); }
  envp.push_back(nullptr);

  execve(binary.c_str(), argv, envp.data());
  throw std::runtime_error(std::string{ "exec_process: execve failed: " } +
                           std::strerror(errno));
}

std::string_view exe_suffix() { return ""; }

std::filesystem::path exe_name(std::string_view base) {
  return std::filesystem::path{ std::string{ base } };
}

}  // namespace envy::platform
