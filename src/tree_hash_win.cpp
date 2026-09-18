#if !defined(_WIN32)
#error "tree_hash_win.cpp is Windows-only; POSIX builds tree_hash_posix.cpp"
#endif

#include "file_read.h"
#include "platform.h"  // pulls in <windows.h> with the project's lean/NOMINMAX settings
#include "tree_hash.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace envy {

namespace {

std::string narrow(std::wstring_view w) {
  if (w.empty()) { return {}; }
  int const n{ ::WideCharToMultiByte(CP_UTF8,
                                     0,
                                     w.data(),
                                     static_cast<int>(w.size()),
                                     nullptr,
                                     0,
                                     nullptr,
                                     nullptr) };
  if (n <= 0) { throw std::runtime_error("tree_hash: undecodable path name"); }
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8,
                        0,
                        w.data(),
                        static_cast<int>(w.size()),
                        out.data(),
                        n,
                        nullptr,
                        nullptr);
  return out;
}

[[noreturn]] void throw_last_error(char const *what, tree_scan_string const &path) {
  throw std::runtime_error(std::string(what) + ": " + narrow(path) + " (error " +
                           std::to_string(::GetLastError()) + ")");
}

struct handle_closer {
  void operator()(HANDLE h) const noexcept {
    if (h && h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); }
  }
};
using scoped_handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, handle_closer>;

struct find_closer {
  void operator()(HANDLE h) const noexcept {
    if (h && h != INVALID_HANDLE_VALUE) { ::FindClose(h); }
  }
};
using scoped_find = std::unique_ptr<std::remove_pointer_t<HANDLE>, find_closer>;

bool is_dot(wchar_t const *name) {
  return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

}  // namespace

unsigned tree_hash_default_threads() {
  unsigned const hw{ std::thread::hardware_concurrency() };
  return hw ? hw : 4u;
}

tree_scan_string tree_scan_root(std::filesystem::path const &root) {
  return file_native_path(root);
}

tree_scan_string tree_scan_join(tree_scan_string const &dir,
                                tree_scan_string const &name) {
  tree_scan_string joined{ dir };
  if (!joined.empty() && joined.back() != L'\\') { joined.push_back(L'\\'); }
  joined += name;
  return joined;
}

std::string tree_scan_utf8(tree_scan_string const &name) { return narrow(name); }

void tree_scan_one(tree_scan_string const &dir, std::vector<tree_scan_entry> &out) {
  std::size_t count{ 0 };
  auto const emit{
    [&out, &count](wchar_t const *name, tree_entry_kind kind, std::uint64_t size = 0) {
      if (count == out.size()) { out.emplace_back(); }
      auto &e{ out[count++] };
      e.name.assign(name);  // assign over the old name; the buffer is already there
      e.kind = kind;
      e.executable = false;  // no such concept here; the digest reads it as 0
      e.size = size;
    }
  };

  WIN32_FIND_DATAW fd{};
  // FIND_FIRST_EX_LARGE_FETCH batches directory reads; on a payload with tens of
  // thousands of files that is the difference between one syscall and hundreds.
  scoped_find const find{ ::FindFirstFileExW(tree_scan_join(dir, L"*").c_str(),
                                             FindExInfoBasic,
                                             &fd,
                                             FindExSearchNameMatch,
                                             nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH) };
  if (find.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("tree_hash: cannot open directory", dir);
  }

  do {
    if (is_dot(fd.cFileName)) { continue; }

    // A reparse point is a symlink for our purposes and is never traversed, so no tree
    // is hashed twice however it is linked.
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      emit(fd.cFileName, tree_entry_kind::SYMLINK);
    } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      emit(fd.cFileName, tree_entry_kind::DIRECTORY);
    } else {
      emit(fd.cFileName,
           tree_entry_kind::FILE,
           (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
    }
  } while (::FindNextFileW(find.get(), &fd));

  if (::GetLastError() != ERROR_NO_MORE_FILES) {
    throw_last_error("tree_hash: cannot read directory", dir);
  }

  out.resize(count);
}

std::string tree_scan_link_target(tree_scan_string const &path) {
  scoped_handle const h{ ::CreateFileW(
      path.c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr) };
  if (h.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("tree_hash: cannot open symlink", path);
  }

  std::wstring buf(1024, L'\0');
  auto const n{ ::GetFinalPathNameByHandleW(h.get(),
                                            buf.data(),
                                            static_cast<DWORD>(buf.size()),
                                            FILE_NAME_NORMALIZED) };
  if (!n || n >= buf.size()) { throw_last_error("tree_hash: cannot read symlink", path); }
  buf.resize(n);
  return narrow(buf);
}

}  // namespace envy
