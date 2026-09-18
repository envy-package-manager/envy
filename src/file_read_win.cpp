#if !defined(_WIN32)
#error "file_read_win.cpp is Windows-only; POSIX builds file_read_posix.cpp"
#endif

#include "file_read.h"

#include "platform.h"  // pulls in <windows.h> with the project's lean/NOMINMAX settings
#include "util.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace envy {

namespace {

// The consumer hashes slot i while the OS fills slot i+1. That is why this is OVERLAPPED
// rather than ReadFile.
constexpr std::size_t kReadChunk{ 1u << 20 };
constexpr std::size_t kReadSlots{ 2 };

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
  if (n <= 0) { throw std::runtime_error("file_read: undecodable path name"); }
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

[[noreturn]] void throw_last_error(char const *what, file_native_string const &path) {
  throw std::runtime_error(std::string(what) + ": " + narrow(path) + " (error " +
                           std::to_string(::GetLastError()) + ")");
}

struct handle_closer {
  void operator()(HANDLE h) const noexcept {
    if (h && h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); }
  }
};
using scoped_handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, handle_closer>;

}  // namespace

file_native_string file_native_path(std::filesystem::path const &path) {
  // \\?\ lifts MAX_PATH. It needs a fully-qualified path with no forward slashes and no
  // '.'/'..', hence the canonicalization.
  auto const abs{ std::filesystem::absolute(path).lexically_normal() };
  auto native{ abs.wstring() };
  std::ranges::replace(native, L'/', L'\\');
  while (!native.empty() && native.back() == L'\\') { native.pop_back(); }
  if (native.starts_with(LR"(\\?\)")) { return native; }
  if (native.starts_with(LR"(\\)")) { return LR"(\\?\UNC\)" + native.substr(2); }
  return LR"(\\?\)" + native;
}

void file_read_chunks(file_native_string const &path,
                      std::uint64_t size,
                      file_chunk_sink const &sink) {
  scoped_handle const h{ ::CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED,
      nullptr) };
  if (h.get() == INVALID_HANDLE_VALUE) {
    throw_last_error("file_read: cannot open", path);
  }

  struct slot {
    std::vector<unsigned char> buf;
    OVERLAPPED ov{};
    scoped_handle event;
    DWORD want{ 0 };
    bool busy{ false };
  };

  // Per worker, not per file: a megabyte and a kernel event per small file cost more
  // than reading it.
  static thread_local std::array<slot, kReadSlots> slots;
  for (auto &s : slots) {
    if (s.buf.empty()) { s.buf.resize(kReadChunk); }
    if (!s.event) {
      s.event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!s.event) { throw_last_error("file_read: cannot create read event", path); }
    }
    s.busy = false;
    s.ov = {};
    s.ov.hEvent = s.event.get();
  }

  std::uint64_t issued{ 0 }, consumed{ 0 };
  std::size_t next{ 0 };

  auto const issue{ [&](slot &s) {
    if (issued >= size) { return; }
    s.want = static_cast<DWORD>(std::min<std::uint64_t>(kReadChunk, size - issued));
    s.ov = {};
    s.ov.hEvent = s.event.get();
    s.ov.Offset = static_cast<DWORD>(issued & 0xFFFFFFFFu);
    s.ov.OffsetHigh = static_cast<DWORD>(issued >> 32);
    ::ResetEvent(s.event.get());
    if (!::ReadFile(h.get(), s.buf.data(), s.want, nullptr, &s.ov) &&
        ::GetLastError() != ERROR_IO_PENDING) {
      throw_last_error("file_read: cannot read", path);
    }
    s.busy = true;
    issued += s.want;
  } };

  for (auto &s : slots) { issue(s); }

  while (consumed < size) {
    slot &s{ slots[next] };
    DWORD got{ 0 };
    // A read at an explicit offset returns `want` or hits EOF, so anything else means
    // the file shrank mid-read. The caller stamps this digest as truth, so fail.
    if (!s.busy || !::GetOverlappedResult(h.get(), &s.ov, &got, TRUE) || got != s.want) {
      if (s.busy && ::GetLastError() != ERROR_HANDLE_EOF && got == s.want) {
        throw_last_error("file_read: cannot read", path);
      }
      throw std::runtime_error("file_read: file shrank while reading: " + narrow(path));
    }
    sink(s.buf.data(), got);
    consumed += got;
    s.busy = false;
    issue(s);
    next = (next + 1) % kReadSlots;
  }
}

}  // namespace envy
