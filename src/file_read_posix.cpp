#if defined(_WIN32)
#error "file_read_posix.cpp is POSIX-only; Windows builds file_read_win.cpp"
#endif

#include "file_read.h"

#include "util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace envy {

namespace {

// Overlap comes from the kernel's readahead window, not queued reads. This is the seam
// an io_uring backend replaces.
constexpr std::size_t kReadChunk{ 1u << 20 };

[[noreturn]] void throw_errno(char const *what, std::string const &path) {
  throw std::system_error(errno, std::generic_category(), std::string(what) + ": " + path);
}

class scoped_fd : uncopyable {
 public:
  explicit scoped_fd(int fd) : fd_{ fd } {}
  ~scoped_fd() {
    if (fd_ >= 0) { ::close(fd_); }
  }
  explicit operator bool() const { return fd_ >= 0; }
  int get() const { return fd_; }

 private:
  int fd_;
};

}  // namespace

file_native_string file_native_path(std::filesystem::path const &path) {
  return path.string();
}

void file_read_chunks(file_native_string const &path,
                      std::uint64_t size,
                      file_chunk_sink const &sink) {
  scoped_fd const fd{ ::open(path.c_str(), O_RDONLY | O_CLOEXEC) };
  if (!fd) { throw_errno("file_read: cannot open", path); }

  // Readahead pays off only when a second read follows. Measured neutral warm; this is
  // the cheap side of neutral.
  if (size > kReadChunk) {
#if defined(__APPLE__)
    ::fcntl(fd.get(), F_RDAHEAD, 1);  // macOS has no fadvise
#elif defined(__linux__)
    ::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }

  // Per worker, not per file: zero-filling a fresh 1 MiB vector costs more than reading
  // a small file, and a payload is mostly small files.
  static thread_local std::vector<unsigned char> buf(kReadChunk);
  std::uint64_t offset{ 0 };
  while (offset < size) {
    auto const want{ static_cast<std::size_t>(
        std::min<std::uint64_t>(kReadChunk, size - offset)) };
    auto const got{ ::pread(fd.get(), buf.data(), want, static_cast<off_t>(offset)) };
    if (got < 0) {
      if (errno == EINTR) { continue; }
      throw_errno("file_read: cannot read", path);
    }
    if (got == 0) {
      // The file shrank mid-read. The caller stamps this digest as truth, so fail.
      throw std::runtime_error("file_read: file shrank while reading: " + path);
    }
    sink(buf.data(), static_cast<std::size_t>(got));
    offset += static_cast<std::uint64_t>(got);
  }
}

}  // namespace envy
