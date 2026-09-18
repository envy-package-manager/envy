#if defined(_WIN32)
#error "tree_hash_posix.cpp is POSIX-only; Windows builds tree_hash_win.cpp"
#endif

#include "file_read.h"
#include "tree_hash.h"
#include "util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>  // Apple-only; glibc dropped this header in 2.32
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace envy {

namespace {

[[noreturn]] void throw_errno(char const *what, std::string const &path) {
  throw std::system_error(errno, std::generic_category(), std::string(what) + ": " + path);
}

class scoped_dir : uncopyable {
 public:
  explicit scoped_dir(char const *path) : d_{ ::opendir(path) } {}
  ~scoped_dir() {
    if (d_) { ::closedir(d_); }
  }
  explicit operator bool() const { return d_ != nullptr; }
  int fd() const { return ::dirfd(d_); }
  dirent const *next() const { return ::readdir(d_); }

 private:
  DIR *d_;
};

bool is_dot(char const *name) {
  return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

}  // namespace

unsigned tree_hash_default_threads() {
#if defined(__APPLE__)
  // perflevel0 is the performance cluster; the call fails on an Intel Mac, which has no
  // clusters and where hardware_concurrency is already the right answer.
  int cores{ 0 };
  std::size_t size{ sizeof(cores) };
  if (!::sysctlbyname("hw.perflevel0.logicalcpu", &cores, &size, nullptr, 0) &&
      cores > 0) {
    return static_cast<unsigned>(cores);
  }
#endif
  unsigned const hw{ std::thread::hardware_concurrency() };
  return hw ? hw : 4u;
}

tree_scan_string tree_scan_root(std::filesystem::path const &root) {
  return file_native_path(root);
}

tree_scan_string tree_scan_join(tree_scan_string const &dir,
                                tree_scan_string const &name) {
  tree_scan_string joined{ dir };
  if (!joined.empty() && joined.back() != '/') { joined.push_back('/'); }
  joined += name;
  return joined;
}

std::string tree_scan_utf8(tree_scan_string const &name) { return name; }

void tree_scan_one(tree_scan_string const &dir, std::vector<tree_scan_entry> &out) {
  std::size_t count{ 0 };
  auto const emit{ [&out, &count](char const *name,
                                  tree_entry_kind kind,
                                  bool executable = false,
                                  std::uint64_t size = 0) {
    if (count == out.size()) { out.emplace_back(); }
    auto &e{ out[count++] };
    e.name.assign(name);  // assign over the old name; the buffer is already there
    e.kind = kind;
    e.executable = executable;
    e.size = size;
  } };

  // Stat children against the directory's descriptor: fstatat walks one name instead of
  // the whole path per entry.
  scoped_dir const d{ dir.c_str() };
  if (!d) { throw_errno("tree_hash: cannot open directory", dir); }
  int const fd{ d.fd() };

  errno = 0;
  while (dirent const *const e{ d.next() }) {
    char const *const name{ e->d_name };
    if (is_dot(name)) {
      errno = 0;
      continue;
    }

    // d_type answers for most entries, but the mode bits and size never come from it, so
    // anything but a symlink still needs the stat.
    if (e->d_type == DT_LNK) {
      emit(name, tree_entry_kind::SYMLINK);
      errno = 0;
      continue;
    }

    struct stat st;
    if (::fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
      throw_errno("tree_hash: cannot stat", tree_scan_join(dir, name));
    }

    if (S_ISLNK(st.st_mode)) {
      emit(name, tree_entry_kind::SYMLINK);
    } else if (S_ISDIR(st.st_mode)) {
      emit(name, tree_entry_kind::DIRECTORY);
    } else if (S_ISREG(st.st_mode)) {
      emit(name,
           tree_entry_kind::FILE,
           (st.st_mode & S_IXUSR) != 0,
           static_cast<std::uint64_t>(st.st_size));
    } else {
      // Sockets, fifos and devices hold no payload a vendored copy could carry.
      throw std::runtime_error("tree_hash: unsupported file type: " +
                               tree_scan_join(dir, name));
    }
    errno = 0;
  }
  if (errno) { throw_errno("tree_hash: cannot read directory", dir); }

  out.resize(count);
}

std::string tree_scan_link_target(tree_scan_string const &path) {
  // A link's st_size is its target length, but it can change between the stat and the
  // read, so grow until the answer stops filling the buffer.
  for (std::size_t cap{ 256 }; cap <= (1u << 16); cap *= 2) {
    std::string buf(cap, '\0');
    auto const n{ ::readlink(path.c_str(), buf.data(), cap) };
    if (n < 0) { throw_errno("tree_hash: cannot read symlink", path); }
    if (static_cast<std::size_t>(n) < cap) {
      buf.resize(static_cast<std::size_t>(n));
      return buf;
    }
  }
  throw std::runtime_error("tree_hash: symlink target too long: " + path);
}

}  // namespace envy
