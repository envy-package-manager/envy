#include "file_read.h"

#include "util.h"

#include <stdexcept>
#include <system_error>

namespace envy {

std::uint64_t file_read_chunks(std::filesystem::path const &path, file_chunk_sink sink) {
  // Sized up front so the reader can queue reads instead of finding EOF a syscall at a
  // time.
  std::error_code ec;
  auto const size{ std::filesystem::file_size(path, ec) };
  if (ec) { throw_runtime_error({ "file_read: cannot size ", path, ": ", ec.message() }); }
  file_read_chunks(file_native_path(path), size, sink);
  return size;
}

}  // namespace envy
